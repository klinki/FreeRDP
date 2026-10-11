"""Single-owner application driver around the actual production reliable byte stream.
Payload generation is open-loop where specified. No application stop-and-wait is added.
"""
from __future__ import annotations
import argparse,hashlib,json,os,random,resource,socket,struct,sys,time,zlib
from .common import Journal,read_json,sleep_until,write_json,timing_summary
from .native import Native
H=struct.Struct('!4sB3xIQQII') # magic,kind,id,scheduled_ns,injected_ns,length,crc
MAGIC=b'RB01';MAX_RECORD=32768

def record(kind,seq,scheduled,injected,payload):
    base=struct.pack('!BIQQ',kind,seq,scheduled,injected)+payload
    return H.pack(MAGIC,kind,seq,scheduled,injected,len(payload),zlib.crc32(base))+payload

class Parser:
    def __init__(self):
        self.buf=bytearray()
        self.measured=bytearray()
        self.measured_payload_bytes=0
    def feed(self,b,measured=False):
        self.buf.extend(b)
        self.measured.extend(bytes([bool(measured)])*len(b))
        out=[]
        if len(self.buf)>2**20:raise ValueError('unbounded application buffer')
        while len(self.buf)>=H.size:
            magic,kind,seq,scheduled,injected,n,crc=H.unpack_from(self.buf)
            if magic!=MAGIC or n>MAX_RECORD:raise ValueError('corrupt record header')
            if len(self.buf)<H.size+n:break
            payload=bytes(self.buf[H.size:H.size+n])
            if zlib.crc32(struct.pack('!BIQQ',kind,seq,scheduled,injected)+payload)!=crc:raise ValueError('CRC mismatch')
            # Attribute only payload bytes read during the measurement window.
            # Validation/drain may finish a record later; headers never count.
            if kind==0:self.measured_payload_bytes+=sum(self.measured[H.size:H.size+n])
            del self.buf[:H.size+n]
            del self.measured[:H.size+n]
            out.append((kind,seq,scheduled,injected,payload))
        return out

class PlainUDP:
    """Harness calibration ONLY. No RDP reliability and no claims about FreeRDP."""
    def __init__(self,bind,port,peer,peer_port):
        self.s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.s.bind((bind,port));self.s.connect((peer,peer_port));self.s.setblocking(False)
    def send(self,b):self.s.send(b)
    def read(self,timeout_ms=0):
        import select
        if timeout_ms:select.select([self.s],[],[],timeout_ms/1000)
        try:return self.s.recv(65535)
        except BlockingIOError:return b''
    def stats(self):return None
    def configure_queue_measurement(self,measure_ns,stop_ns):pass
    def queue_stats(self):return None
    def close(self):self.s.close()

def run(c):
    out=Journal(c['events']);driver=None;error=None
    observer_enabled=os.environ.get('RDP_BENCH_OBSERVER','1')!='0'
    def emit(kind,**data):
        if observer_enabled or kind in ('phase','endpoint_error','probe_timeout','probe_not_injected'):
            out.emit(kind,**data)
    counters={'sent_bytes':0,'received_bytes':0,'measured_bytes':0,'sent_records':0,'received_records':0,
              'probes_expected':0,'probes_injected':0,'probes_observed':0,'probes_not_injected':0,'unexpected_order':0}
    parser=Parser();payload=random.Random(c['seed']).randbytes(c['payload_bytes'])
    sent_hash=hashlib.sha256();recv_hash=hashlib.sha256()
    last_rx=0;gaps=[];probe_delays=[];scheduled_delays=[];send_block=[]
    received_probes=set();pending={};seq=0;expected_seq=0;next_bulk=0;probe_index=0
    try:
        cls=PlainUDP if c['driver']=='udp-calibration' else Native
        args=[c['bind'],c['port'],c['peer'],c['peer_port']]
        if cls is Native:args.insert(0,c['library'])
        driver=cls(*args)
        print(json.dumps({'type':'ready','pid':os.getpid()}),flush=True)
        control=json.loads(sys.stdin.readline())
        start=control['start_ns'];measure=control['measure_ns'];stop=control['stop_ns'];end=control['end_ns']
        driver.configure_queue_measurement(measure,stop)
        interval=int(1e9/c['probe_hz']) if c['probe_hz'] else 0
        sends_bulk=c['role'] in ('bulk','duplex','bulk_probe');probes=c['role'] in ('probe','mixed_probe','bulk_probe')
        counters['probes_expected']=max(0,(stop-start+interval-1)//interval) if probes else 0
        next_bulk=start;next_sample=start;next_probe=start
        cpu_start=time.process_time_ns();sleep_until(start)
        phases=[('start',start),('measure',measure),('drain',stop)]
        def send(b):
            a=time.monotonic_ns();driver.send(b);btime=time.monotonic_ns()
            if measure<=a<stop:send_block.append((btime-a)/1e6)
        while time.monotonic_ns()<end:
            now=time.monotonic_ns()
            while phases and now>=phases[0][1]:
                name,planned=phases.pop(0)
                emit('phase',name=name,planned_ns=planned)
            if probes and now<stop and now>=next_probe:
                # Do not replay a burst of severely overdue probes. Count each missed deadline.
                while now-next_probe>200_000_000 and next_probe<stop:
                    emit('probe_not_injected',id=probe_index,scheduled_ns=next_probe,reason='driver_backpressure')
                    counters['probes_not_injected']+=1;probe_index+=1;next_probe=start+probe_index*interval
                if next_probe<stop:
                    injected=time.monotonic_ns();pending[probe_index]=(next_probe,injected)
                    emit('probe_injected',id=probe_index,scheduled_ns=next_probe,injected_ns=injected)
                    send(record(1,probe_index,next_probe,injected,b'P'*64));counters['probes_injected']+=1
                    probe_index+=1;next_probe=start+probe_index*interval
            now=time.monotonic_ns()
            if sends_bulk and now<stop and now>=next_bulk:
                send(record(0,seq,now,now,payload));seq+=1;counters['sent_records']+=1
                counters['sent_bytes']+=len(payload);sent_hash.update(payload)
                rate=c.get('offered_mbps',0)
                if rate:
                    next_bulk+=int((H.size+len(payload))*8*1000/rate)
                    # The deficit is observable; do not silently reset next_bulk to now.
            b=driver.read(0 if sends_bulk and time.monotonic_ns()<stop else 5)
            if b:
                read_at=time.monotonic_ns()
                for kind,i,sched,inj,p in parser.feed(b,measure<=read_at<stop):
                    received=time.monotonic_ns()
                    if kind==0:
                        if i!=expected_seq:
                            counters['unexpected_order']+=1
                            if c['driver']!='udp-calibration':raise ValueError(f'order/duplicate: expected {expected_seq}, got {i}')
                        expected_seq=i+1
                        if p!=payload:raise ValueError('payload differs from seeded source')
                        counters['received_bytes']+=len(p);counters['received_records']+=1;recv_hash.update(p)
                        if measure<=received<stop:
                            gaps.append((received-(last_rx or measure))/1e6)
                            last_rx=received
                    elif kind==1:
                        send(record(2,i,sched,inj,p))
                    elif kind==2:
                        if i in received_probes:raise ValueError('duplicate probe delivery')
                        if pending.get(i)!=(sched,inj) or p!=b'P'*64:raise ValueError('unexpected or corrupt probe response')
                        received_probes.add(i);counters['probes_observed']+=1
                        pending.pop(i,None)
                        emit('probe_response',id=i,scheduled_ns=sched,injected_ns=inj,observed_ns=received)
                        if measure<=sched<stop:
                            probe_delays.append((received-inj)/1e6);scheduled_delays.append((received-sched)/1e6)
                    else:raise ValueError('unknown application record')
                counters['measured_bytes']=parser.measured_payload_bytes
            elif not sends_bulk:pass # read() already waited
            if observer_enabled and now>=next_sample:
                out.emit('sample',counters=dict(counters),transport=driver.stats(),process_cpu_ns=time.process_time_ns(),rss_raw=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
                next_sample+=1_000_000_000
            if sends_bulk and c.get('offered_mbps',0) and now<next_bulk and not b:
                time.sleep(min(.001,(next_bulk-now)/1e9))
        emit('phase',name='end',planned_ns=end)
        if last_rx:gaps.append(max(0,stop-last_rx)/1e6) # include final censored stall
        elif c['role'] in ('sink','duplex','mixed_probe'):gaps.append((stop-measure)/1e6)
        if probes:
            while next_probe<stop:
                emit('probe_not_injected',id=probe_index,scheduled_ns=next_probe,reason='never_serviced')
                counters['probes_not_injected']+=1;probe_index+=1;next_probe=start+probe_index*interval
        for i,(s,a) in pending.items():emit('probe_timeout',id=i,scheduled_ns=s,injected_ns=a,deadline_ns=end)
        counters['measured_bytes']=parser.measured_payload_bytes
        result={'version':1,'driver':c['driver'],'scope':'raw-udp2-steady-state' if c['driver']=='native' else 'HARNESS_CALIBRATION_ONLY',
                'role':c['role'],'observer_enabled':observer_enabled,'counters':counters,'incomplete_record_bytes':len(parser.buf),
                'sent_sha256':sent_hash.hexdigest(),'received_sha256':recv_hash.hexdigest(),
                'goodput_mbps':counters['measured_bytes']*8/(stop-measure)*1000,
                'probe_rtt_ms':timing_summary(probe_delays),'probe_scheduled_ms':timing_summary(scheduled_delays),
                'send_call_ms':timing_summary(send_block),'delivery_gap_ms':timing_summary(gaps),
                'probe_timeouts':len(pending),'cpu_total_s':(time.process_time_ns()-cpu_start)/1e9,
                'measurement_s':(stop-measure)/1e9,'native_stats':driver.stats(),'queue_stats':driver.queue_stats(),'completed':True}
    except BaseException as e:
        error=repr(e);out.emit('endpoint_error',error=error)
        result={'version':1,'driver':c['driver'],'completed':False,'error':error,'counters':counters}
    finally:
        if driver:driver.close()
        out.close()
    write_json(c['result'],result)
    return 1 if error else 0

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('config');sys.exit(run(read_json(a.parse_args().config)))
