"""Portable, real-time UDP impairment relay. It does NOT implement reliability.

Production transport endpoints remain responsible for ACK, retransmit and ordering.
Network serialization is computed in ns. OS scheduling is real and must be calibrated.
Not suitable for uncalibrated high packet-rate CPU/syscall performance claims.
"""
from __future__ import annotations
import collections,hashlib,heapq,json,selectors,socket,threading,time
from pathlib import Path
from .common import timing_summary
from .feedback import FeedbackTiming, SELECTED_KINDS, validate_feedback, packet_kind, ACK, DATA, ACKVEC, OVERHEAD, DELAYACK, AOA

def packet_metadata(data):
    """Visible protected UDP2 headers only; never retain application payload."""
    kind=packet_kind(data)
    result={'kind':kind,'udp_bytes':len(data)}
    if kind=='unclassified':return result
    restored=bytearray(data);restored[0],restored[7]=restored[7],restored[0]
    layout=restored[1:];header=int.from_bytes(layout[:2],'little');flags=header&0xFFF
    result['log_window']=header>>12;cursor=2
    if flags&ACK:
        result['ack_base']=int.from_bytes(layout[cursor:cursor+2],'little')
        cursor+=7+(layout[cursor+6]&15)
    for flag,size in ((OVERHEAD,1),(DELAYACK,3),(AOA,2)):
        if flags&flag:
            if flag==AOA:result['aoa']=int.from_bytes(layout[cursor:cursor+2],'little')
            cursor+=size
    if flags&DATA:
        result['data_seq']=int.from_bytes(layout[cursor:cursor+2],'little');cursor+=2
    if flags&ACKVEC:
        result['ackvec_base']=int.from_bytes(layout[cursor:cursor+2],'little')
        v=layout[cursor+2];cursor+=3+(4 if v&128 else 0)+(v&127)
    if flags&DATA:result['channel_seq']=int.from_bytes(layout[cursor:cursor+2],'little')
    return result

class RelayTrace:
    CAPACITY=250000
    def __init__(self):self.records=[];self.overflow=0;self.unclassified=0
    def add(self,event_type,**fields):
        if len(self.records)<self.CAPACITY:self.records.append(dict(type=event_type,**fields))
        else:self.overflow+=1
    def summary(self):
        return {'version':1,'enabled':True,'valid':not(self.overflow or self.unclassified),
                'count':len(self.records),'capacity':self.CAPACITY,'overflow':self.overflow,
                'unclassified':self.unclassified,'scope':'visible UDP2 header, offer/drop and delivery deadlines; no payload'}

class Link:
    def __init__(self,profile,seed,direction,feedback_delay_ms=None):
        self.p=dict(profile);self.seed=seed;self.direction=direction;self.next_free=0
        self.n=0;self.start_ns=0;self.count=collections.Counter();self.lateness=[]
        self.feedback=FeedbackTiming(feedback_delay_ms) if feedback_delay_ms is not None else None
    def uniform(self,index,lane):
        b=f'{self.seed}:{self.direction}:{index}:{lane}'.encode()
        return int.from_bytes(hashlib.blake2b(b,digest_size=8).digest(),'big')/2**64
    def offer(self,now,data):
        self.n+=1;i=self.n;p=self.p;self.count['received_datagrams']+=1
        self.count['received_udp_bytes']+=len(data)
        t_ms=(now-self.start_ns)/1e6
        if any(a<=t_ms<a+b for a,b in p.get('blackouts_ms',[])):
            self.count['blackout_drops']+=1;return []
        rate=p['rate_mbps']*1e6
        service=int((len(data)+p.get('overhead_bytes',28))*8*1e9/rate) if rate else 0
        start=max(now,self.next_free)
        if rate and start-now>p['queue_ms']*1e6:
            self.count['queue_drops']+=1;return []
        self.next_free=start+service
        if self.uniform(i,0)<p.get('loss',0):
            self.count['random_drops']+=1;return []
        delay=p['one_way_ms']+(2*self.uniform(i,1)-1)*p.get('jitter_ms',0)
        if self.uniform(i,2)<p.get('reorder',0):
            delay+=p.get('reorder_extra_ms',15);self.count['reorder_events']+=1
        due=self.next_free+max(0,int(delay*1e6))
        out=[(due,data)]
        if self.uniform(i,3)<p.get('duplicate',0):
            self.count['duplicates']+=1
            self.next_free+=service;out.append((due+service,data))
        return out
    def schedule(self,now,data):
        kind=self.feedback.classify(data) if self.feedback else None
        selected=kind in SELECTED_KINDS
        return [(self.feedback.schedule(due) if selected else due,payload,(due,kind) if selected else None)
                for due,payload in self.offer(now,data)]
    def delivered(self,due,actual,length,feedback_base_due=None):
        self.count['delivered_datagrams']+=1;self.count['delivered_udp_bytes']+=length
        # Bounded reservoir with stable sampling. Counts/maxima include every delivery.
        lag=max(0,actual-due)/1e6
        self.count['late_over_1ms']+=lag>1; self.count['late_over_5ms']+=lag>5
        self.count['max_lateness_us']=max(self.count['max_lateness_us'],int(lag*1000))
        if len(self.lateness)<200000:self.lateness.append(lag)
        else:
            j=int(self.uniform(self.count['delivered_datagrams'],7)*self.count['delivered_datagrams'])
            if j<len(self.lateness):self.lateness[j]=lag
        if feedback_base_due is not None:
            # Stable reservoir lane separate from network impairment decisions.
            index=int(self.uniform(self.feedback.delivered+1,8)*(self.feedback.delivered+1))
            base_due,kind=feedback_base_due
            self.feedback.observe(base_due,actual,index,length,kind)
    def summary(self):
        result=dict(self.count,lateness_ms=timing_summary(self.lateness),reservoir_capacity=200000,
                    lateness_sampled=self.count['delivered_datagrams']>len(self.lateness))
        if self.feedback:result['ack_feedback']=self.feedback.summary()
        return result

class Relay:
    def __init__(self,left_port,right_port,profile,seed,ack_feedback=None,feedback_ledger=None,loss_ledger=None):
        self.sel=selectors.DefaultSelector();self.sockets=[];self.heap=[];self.serial=0
        treatment=validate_feedback(ack_feedback)
        self.feedback_ledger=feedback_ledger
        self.loss_ledger=loss_ledger;self.loss_trace=RelayTrace() if loss_ledger else None
        self.links=[Link(profile['c2s'],seed,'c2s',treatment['c2s_delay_ms'] if treatment else None),
                    Link(profile['s2c'],seed+1000003,'s2c',treatment['s2c_delay_ms'] if treatment else None)]
        self.peers=[('127.0.0.1',left_port),('127.0.0.1',right_port)]
        for i in range(2):
            s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',0))
            s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
            s.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,4*1024*1024)
            s.setblocking(False);self.sockets.append(s);self.sel.register(s,selectors.EVENT_READ,i)
        self.stop=threading.Event();self.error=None;self.thread=None
    @property
    def ports(self):return [s.getsockname()[1] for s in self.sockets]
    def start(self,epoch_ns):
        for l in self.links:l.start_ns=epoch_ns
        self.thread=threading.Thread(target=self.run,name='udp-impairment',daemon=True);self.thread.start()
    def run(self):
        try:
            while not self.stop.is_set():
                now=time.monotonic_ns()
                timeout=min(.005,max(0,(self.heap[0][0]-now)/1e9)) if self.heap else .005
                for key,_ in self.sel.select(timeout):
                    direction=key.data
                    # Bounded drain: arrival floods must not starve already-scheduled deliveries.
                    for _ in range(64):
                        try:data,addr=key.fileobj.recvfrom(65535)
                        except BlockingIOError:break
                        if addr!=self.peers[direction]:
                            self.links[direction].count['unexpected_peer']+=1;continue
                        offered=time.monotonic_ns();link=self.links[direction]
                        before={key:link.count[key] for key in ('blackout_drops','queue_drops','random_drops')} if self.loss_trace else None
                        scheduled=link.schedule(offered,data)
                        metadata=packet_metadata(data) if self.loss_trace else None
                        if self.loss_trace:
                            self.loss_trace.unclassified+=metadata['kind']=='unclassified'
                            self.loss_trace.add('offer',direction=link.direction,packet_id=link.n,offered_ns=offered,
                                                scheduled_copies=len(scheduled),**metadata)
                            if not scheduled:
                                reason=next((key for key in before if link.count[key]>before[key]),'unknown_drop')
                                self.loss_trace.add('drop',direction=link.direction,packet_id=link.n,offered_ns=offered,
                                                    reason=reason,**metadata)
                        for due,payload,feedback_base_due in scheduled:
                            if len(self.heap)>=65536:raise RuntimeError('relay capacity exceeded; invalid harness')
                            self.serial+=1
                            heapq.heappush(self.heap,(due,self.serial,direction,payload,feedback_base_due,
                                                     (link.n,offered,metadata) if self.loss_trace else None))
                now=time.monotonic_ns()
                while self.heap and self.heap[0][0]<=now:
                    due,serial,d,data,feedback_base_due,loss_info=heapq.heappop(self.heap)
                    try:
                        self.sockets[1-d].sendto(data,self.peers[1-d])
                        actual=time.monotonic_ns();self.links[d].delivered(due,actual,len(data),feedback_base_due)
                        if self.loss_trace:
                            packet_id,offered,metadata=loss_info
                            self.loss_trace.add('dispatch',direction=self.links[d].direction,packet_id=packet_id,
                                                copy_id=serial,offered_ns=offered,due_ns=due,
                                                network_due_ns=feedback_base_due[0] if feedback_base_due else due,
                                                actual_dispatch_ns=actual,lateness_ns=max(0,actual-due),**metadata)
                    except (BlockingIOError,OSError):
                        self.links[d].count['socket_send_errors']+=1
                        if self.loss_trace:
                            self.loss_trace.add('send_error',direction=self.links[d].direction,copy_id=serial,due_ns=due)
                    now=time.monotonic_ns()
        except BaseException as e:self.error=repr(e)
    def close(self):
        self.stop.set()
        if self.thread:self.thread.join(5)
        pending=len(self.heap)
        self.sel.close()
        for s in self.sockets:s.close()
        if self.feedback_ledger:
            records=[dict(record,direction=link.direction) for link in self.links if link.feedback
                     for record in link.feedback.records]
            with Path(self.feedback_ledger).open('w',encoding='utf-8') as stream:
                for record in sorted(records,key=lambda x:x['actual_dispatch_ns']):
                    stream.write(json.dumps(record,separators=(',',':'))+'\n')
        result={'engine':'python_udp_relay_v1','c2s':self.links[0].summary(),'s2c':self.links[1].summary(),
                'pending_on_close':pending,'error':self.error,'kernel_drop_counter':None}
        if self.loss_trace:
            with Path(self.loss_ledger).open('w',encoding='utf-8') as stream:
                for record in self.loss_trace.records:stream.write(json.dumps(record,separators=(',',':'))+'\n')
            result['loss_trace']=dict(self.loss_trace.summary(),ledger=Path(self.loss_ledger).name)
        return result
