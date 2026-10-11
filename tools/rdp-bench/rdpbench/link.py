"""Portable, real-time UDP impairment relay. It does NOT implement reliability.

Production transport endpoints remain responsible for ACK, retransmit and ordering.
Network serialization is computed in ns. OS scheduling is real and must be calibrated.
Not suitable for uncalibrated high packet-rate CPU/syscall performance claims.
"""
from __future__ import annotations
import collections,hashlib,heapq,selectors,socket,threading,time
from .common import timing_summary

class Link:
    def __init__(self,profile,seed,direction):
        self.p=dict(profile);self.seed=seed;self.direction=direction;self.next_free=0
        self.n=0;self.start_ns=0;self.count=collections.Counter();self.lateness=[]
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
    def delivered(self,due,actual,length):
        self.count['delivered_datagrams']+=1;self.count['delivered_udp_bytes']+=length
        # Bounded reservoir with stable sampling. Counts/maxima include every delivery.
        lag=max(0,actual-due)/1e6
        self.count['late_over_1ms']+=lag>1; self.count['late_over_5ms']+=lag>5
        self.count['max_lateness_us']=max(self.count['max_lateness_us'],int(lag*1000))
        if len(self.lateness)<200000:self.lateness.append(lag)
        else:
            j=int(self.uniform(self.count['delivered_datagrams'],7)*self.count['delivered_datagrams'])
            if j<len(self.lateness):self.lateness[j]=lag
    def summary(self):
        return dict(self.count,lateness_ms=timing_summary(self.lateness),reservoir_capacity=200000,
                    lateness_sampled=self.count['delivered_datagrams']>len(self.lateness))

class Relay:
    def __init__(self,left_port,right_port,profile,seed):
        self.sel=selectors.DefaultSelector();self.sockets=[];self.heap=[];self.serial=0
        self.links=[Link(profile['c2s'],seed,'c2s'),Link(profile['s2c'],seed+1000003,'s2c')]
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
                        for due,payload in self.links[direction].offer(time.monotonic_ns(),data):
                            if len(self.heap)>=65536:raise RuntimeError('relay capacity exceeded; invalid harness')
                            self.serial+=1
                            heapq.heappush(self.heap,(due,self.serial,direction,payload))
                now=time.monotonic_ns()
                while self.heap and self.heap[0][0]<=now:
                    due,_,d,data=heapq.heappop(self.heap)
                    try:
                        self.sockets[1-d].sendto(data,self.peers[1-d])
                        self.links[d].delivered(due,time.monotonic_ns(),len(data))
                    except (BlockingIOError,OSError):self.links[d].count['socket_send_errors']+=1
                    now=time.monotonic_ns()
        except BaseException as e:self.error=repr(e)
    def close(self):
        self.stop.set()
        if self.thread:self.thread.join(5)
        pending=len(self.heap)
        self.sel.close()
        for s in self.sockets:s.close()
        return {'engine':'python_udp_relay_v1','c2s':self.links[0].summary(),'s2c':self.links[1].summary(),
                'pending_on_close':pending,'error':self.error,'kernel_drop_counter':None}
