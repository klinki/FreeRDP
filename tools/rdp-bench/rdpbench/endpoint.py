"""Single-owner application driver around the actual production reliable byte stream.
Payload generation is open-loop where specified. No application stop-and-wait is added.
"""
from __future__ import annotations
import argparse,hashlib,json,os,random,resource,socket,struct,sys,time,zlib
from .common import Journal,read_json,sleep_until,write_json,timing_summary
from .native import Native
H=struct.Struct('!4sB3xIQQII') # magic,kind,id,scheduled_ns,injected_ns,length,crc
MAGIC=b'RB01';MAX_RECORD=32768
# Cancellation interrupts a stuck native call. The ordinary final 5 ms read
# may cross end_ns; this grace must not extend the workload or drain window.
NATIVE_DRAIN_CANCEL_GRACE_NS=100_000_000

class CallTrace:
    """Optional bounded memory ledger; no disk writes inside measured calls."""
    CAPACITY=200000
    def __init__(self):
        self.records=[];self.overflow=0;self.last_call=None;self.last_slow_call=None
    def add(self,event_type,**fields):
        if len(self.records)<self.CAPACITY:self.records.append(dict(type=event_type,**fields))
        else:self.overflow+=1
    def call(self,kind,record_id,size,start,end):
        value=dict(call_kind=kind,record_id=record_id,bytes=size,start_ns=start,end_ns=end,duration_ns=end-start)
        self.last_call=value
        if end-start>=5_000_000:self.last_slow_call=value
        if kind!='read' or end-start>=5_000_000:self.add('native_call',**value)
    def summary(self):
        return {'version':1,'enabled':True,'valid':self.overflow==0,'count':len(self.records),
                'capacity':self.CAPACITY,'overflow':self.overflow,
                'scope':'all send calls, reads >=5 ms, missed deadlines and owner-loop gaps >=5 ms'}

def write_ledger(path,records):
    with open(path,'w',encoding='utf-8') as stream:
        for value in records:stream.write(json.dumps(value,separators=(',',':'),allow_nan=False)+'\n')

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
    start=measure=stop=end=cpu_start=None;exit_ns=failure_ns=None;bounds_ready=False
    cancel_deadline_ns=None
    operation={'operation':'initialization'};telemetry_errors=[];cleanup_errors=[]
    observer_enabled=os.environ.get('RDP_BENCH_OBSERVER','1')!='0'
    trace=CallTrace() if os.environ.get('RDP_BENCH_LOSS_TRACE')=='1' else None
    def emit(kind,**data):
        if observer_enabled or kind in ('phase','endpoint_error','probe_timeout','probe_not_injected'):
            out.emit(kind,**data)
    counters={'sent_bytes':0,'received_bytes':0,'measured_bytes':0,'sent_records':0,'received_records':0,
              'probes_expected':0,'probes_injected':0,'probes_observed':0,'probes_not_injected':0,'unexpected_order':0,
              'probes_admitted':0,'probe_send_failures':0,'probes_unserved_after_failure':0}
    parser=Parser();payload=random.Random(c['seed']).randbytes(c['payload_bytes'])
    sent_hash=hashlib.sha256();recv_hash=hashlib.sha256()
    last_rx=0;gaps=[];probe_delays=[];scheduled_delays=[];send_block=[]
    received_probes=set();pending={};seq=0;expected_seq=0;next_bulk=0;probe_index=0
    probes=c['role'] in ('probe','mixed_probe','bulk_probe');next_probe=None;interval=0
    def snapshot(name,method):
        try:return method()
        except Exception as e:
            telemetry_errors.append({'operation':name,'error':repr(e),'observed_ns':time.monotonic_ns()})
            return None
    try:
        cls=PlainUDP if c['driver']=='udp-calibration' else Native
        args=[c['bind'],c['port'],c['peer'],c['peer_port']]
        if cls is Native:args.insert(0,c['library'])
        driver=cls(*args)
        print(json.dumps({'type':'ready','pid':os.getpid()}),flush=True)
        operation={'operation':'control'}
        control=json.loads(sys.stdin.readline())
        start=control['start_ns'];measure=control['measure_ns'];stop=control['stop_ns'];end=control['end_ns']
        if not all(isinstance(x,int) and not isinstance(x,bool) for x in (start,measure,stop,end)) or not start<=measure<stop<end:
            raise ValueError('invalid endpoint clock bounds')
        interval=int(1e9/c['probe_hz']) if c['probe_hz'] else 0
        sends_bulk=c['role'] in ('bulk','duplex','bulk_probe')
        counters['probes_expected']=max(0,(stop-start+interval-1)//interval) if probes else 0
        next_bulk=start;next_sample=start;next_probe=start;bounds_ready=True
        operation={'operation':'configure_queue_measurement'}
        driver.configure_queue_measurement(measure,stop)
        if hasattr(driver,'configure_deadline'):
            operation={'operation':'configure_deadline'}
            cancel_deadline_ns=end+NATIVE_DRAIN_CANCEL_GRACE_NS
            driver.configure_deadline(cancel_deadline_ns)
        if trace and hasattr(driver,'configure_loss_trace'):
            operation={'operation':'configure_loss_trace'};driver.configure_loss_trace()
        cpu_start=time.process_time_ns();sleep_until(start)
        phases=[('start',start),('measure',measure),('drain',stop)]
        def send(b,kind,record_id):
            nonlocal operation
            operation={'operation':'send','call_kind':kind,'record_id':record_id,'bytes':len(b)}
            a=time.monotonic_ns()
            try:driver.send(b)
            except BaseException:
                operation['admission']='unknown_after_failed_call'
                raise
            finally:
                btime=time.monotonic_ns()
                if trace:trace.call(kind,record_id,len(b),a,btime)
                if measure<=a<stop:send_block.append((btime-a)/1e6)
        previous_loop_end=time.monotonic_ns()
        while time.monotonic_ns()<end:
            now=time.monotonic_ns()
            loop_start=now
            if trace and now-previous_loop_end>=5_000_000:
                trace.add('owner_loop_gap',start_ns=previous_loop_end,end_ns=now,duration_ns=now-previous_loop_end)
            while phases and now>=phases[0][1]:
                name,planned=phases.pop(0)
                emit('phase',name=name,planned_ns=planned)
            if probes and now<stop and now>=next_probe:
                # Do not replay a burst of severely overdue probes. Count each missed deadline.
                while now-next_probe>200_000_000 and next_probe<stop:
                    emit('probe_not_injected',id=probe_index,scheduled_ns=next_probe,reason='driver_backpressure')
                    if trace:trace.add('probe_not_injected',id=probe_index,scheduled_ns=next_probe,
                                       observed_ns=now,lateness_ns=now-next_probe,last_call=trace.last_call,
                                       last_slow_call=trace.last_slow_call)
                    counters['probes_not_injected']+=1;probe_index+=1;next_probe=start+probe_index*interval
                if next_probe<stop:
                    injected=time.monotonic_ns();probe_id=probe_index;scheduled=next_probe
                    pending[probe_id]=(scheduled,injected)
                    # Injection means a call was attempted, not admission or delivery.
                    # Advance before calling: a terminal send can fail after accepting bytes.
                    counters['probes_injected']+=1;probe_index+=1;next_probe=start+probe_index*interval
                    emit('probe_injected',id=probe_id,scheduled_ns=scheduled,injected_ns=injected)
                    if trace:trace.add('probe_injected',id=probe_id,scheduled_ns=scheduled,injected_ns=injected)
                    try:send(record(1,probe_id,scheduled,injected,b'P'*64),'probe',probe_id)
                    except BaseException:
                        counters['probe_send_failures']+=1
                        emit('probe_send_failed',id=probe_id,admission='unknown_after_failed_call')
                        raise
                    counters['probes_admitted']+=1
            now=time.monotonic_ns()
            if sends_bulk and now<stop and now>=next_bulk:
                send(record(0,seq,now,now,payload),'bulk',seq);seq+=1;counters['sent_records']+=1
                counters['sent_bytes']+=len(payload);sent_hash.update(payload)
                rate=c.get('offered_mbps',0)
                if rate:
                    next_bulk+=int((H.size+len(payload))*8*1000/rate)
                    # The deficit is observable; do not silently reset next_bulk to now.
            operation={'operation':'read'};read_start=time.monotonic_ns();b=b''
            try:b=driver.read(0 if sends_bulk and time.monotonic_ns()<stop else 5)
            finally:
                if trace:trace.call('read',None,len(b),read_start,time.monotonic_ns())
            if b:
                operation={'operation':'parse'};read_at=time.monotonic_ns()
                for kind,i,sched,inj,p in parser.feed(b,measure<=read_at<stop):
                    operation={'operation':'parse','record_kind':kind,'record_id':i}
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
                        send(record(2,i,sched,inj,p),'echo',i)
                    elif kind==2:
                        if i in received_probes:raise ValueError('duplicate probe delivery')
                        if pending.get(i)!=(sched,inj) or p!=b'P'*64:raise ValueError('unexpected or corrupt probe response')
                        received_probes.add(i);counters['probes_observed']+=1
                        pending.pop(i,None)
                        emit('probe_response',id=i,scheduled_ns=sched,injected_ns=inj,observed_ns=received)
                        if trace:trace.add('probe_response',id=i,scheduled_ns=sched,injected_ns=inj,observed_ns=received)
                        if measure<=sched<stop:
                            probe_delays.append((received-inj)/1e6);scheduled_delays.append((received-sched)/1e6)
                    else:raise ValueError('unknown application record')
                counters['measured_bytes']=parser.measured_payload_bytes
            elif not sends_bulk:pass # read() already waited
            if observer_enabled and now>=next_sample:
                out.emit('sample',counters=dict(counters),transport=snapshot('native_stats',driver.stats),process_cpu_ns=time.process_time_ns(),rss_raw=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
                next_sample+=1_000_000_000
            if sends_bulk and c.get('offered_mbps',0) and now<next_bulk and not b:
                time.sleep(min(.001,(next_bulk-now)/1e9))
            previous_loop_end=time.monotonic_ns()
            if trace and previous_loop_end-loop_start>=5_000_000:
                trace.add('owner_iteration',start_ns=loop_start,end_ns=previous_loop_end,
                          duration_ns=previous_loop_end-loop_start)
        emit('phase',name='end',planned_ns=end)
    except BaseException as e:
        error=repr(e);failure_ns=time.monotonic_ns()
        out.emit('endpoint_error',error=error,operation=operation,failure_ns=failure_ns)
    finally:
        exit_ns=time.monotonic_ns()
        if bounds_ready:
            observed_stop=min(stop,max(measure,exit_ns))
            if observed_stop>measure:
                if last_rx:gaps.append(max(0,observed_stop-last_rx)/1e6)
                elif c['role'] in ('sink','duplex','mixed_probe'):gaps.append((observed_stop-measure)/1e6)
            if probes:
                while next_probe<stop:
                    reason='endpoint_failure' if error else 'never_serviced'
                    emit('probe_not_injected',id=probe_index,scheduled_ns=next_probe,reason=reason)
                    if trace:trace.add('probe_not_injected',id=probe_index,scheduled_ns=next_probe,
                                       observed_ns=exit_ns,reason=reason,last_call=trace.last_call)
                    counters['probes_not_injected']+=1
                    if error:counters['probes_unserved_after_failure']+=1
                    probe_index+=1;next_probe=start+probe_index*interval
        # Early termination leaves attempted calls unobserved; it does not invent
        # deadline expirations for probes whose drain deadline has not arrived.
        deadline_reached=bounds_ready and exit_ns>=end
        for i,(s,a) in pending.items():
            emit('probe_timeout' if deadline_reached else 'probe_unobserved',id=i,
                 scheduled_ns=s,injected_ns=a,deadline_ns=end,observed_ns=exit_ns,
                 reason='drain_deadline' if deadline_reached else 'endpoint_terminated')
        counters['measured_bytes']=parser.measured_payload_bytes
        result={'version':1,'driver':c['driver'],'scope':'raw-udp2-steady-state' if c['driver']=='native' else 'HARNESS_CALIBRATION_ONLY',
                'role':c['role'],'observer_enabled':observer_enabled,'counters':counters,'incomplete_record_bytes':len(parser.buf),
                'sent_sha256':sent_hash.hexdigest(),'received_sha256':recv_hash.hexdigest(),
                'goodput_mbps':counters['measured_bytes']*8/(stop-measure)*1000 if bounds_ready else None,
                'goodput_scope':'CRC-validated record payload observed before exit; planned measurement denominator; complete-stream integrity requires peer audit',
                'probe_rtt_ms':timing_summary(probe_delays),'probe_scheduled_ms':timing_summary(scheduled_delays),
                'latency_scope':'observed responses to probes scheduled during measurement; survivor-only, excludes unobserved probes',
                'probe_count_scope':'all probes scheduled from warmup start through measurement stop',
                'send_call_ms':timing_summary(send_block),'delivery_gap_ms':timing_summary(gaps),
                'probe_timeouts':len(pending) if deadline_reached else 0,'probes_pending_at_exit':len(pending),
                'probe_unobserved':len(pending),'cpu_total_s':(time.process_time_ns()-cpu_start)/1e9 if cpu_start is not None else None,
                'measurement_s':(stop-measure)/1e9 if bounds_ready else None,
                'measurement_observed_s':(min(stop,max(measure,exit_ns))-measure)/1e9 if bounds_ready else None,
                'native_stats':snapshot('native_stats',driver.stats) if driver else None,
                'native_backend':getattr(driver,'backend',None) if driver else None,
                'native_driver_error':snapshot('driver_error',driver.driver_error) if driver and hasattr(driver,'driver_error') else None,
                'native_driver_error_available':getattr(driver,'driver_error_available',False) if driver else False,
                'queue_stats':snapshot('queue_stats',driver.queue_stats) if driver else None,
                'flight_config':snapshot('flight_config',driver.flight_config) if driver and hasattr(driver,'flight_config') else None,
                'telemetry_errors':telemetry_errors,'cleanup_errors':cleanup_errors,
                'benchmark_deadline_reached':snapshot('deadline_reached',driver.deadline_reached) if driver and hasattr(driver,'deadline_reached') else False,
                'native_cancel_deadline_ns':cancel_deadline_ns,
                'native_cancel_grace_ns':NATIVE_DRAIN_CANCEL_GRACE_NS if cancel_deadline_ns is not None else None,
                'completed':error is None}
        if error:
            result['error']=error
            result['terminal']={**operation,'error':error,'failure_ns':failure_ns,
                                'since_trial_start_ms':(failure_ns-start)/1e6 if bounds_ready else None,
                                'since_measurement_start_ms':(failure_ns-measure)/1e6 if bounds_ready else None}
        if trace:
            try:
                app_path=c['events']+'.loss-calls.jsonl';write_ledger(app_path,trace.records)
                try:
                    native=driver.loss_trace() if hasattr(driver,'loss_trace') else {
                        'available':False,'valid':False,'reasons':['native loss trace ABI unavailable'],'rows':[]}
                except Exception as e:
                    native={'available':False,'valid':False,'reasons':['native loss trace export failed: '+repr(e)],'rows':[]}
                native_path=c['events']+'.loss-native.jsonl'
                fields=native.get('fields',[]);names=native.get('event_names',{})
                def native_records():
                    for row in native.get('rows',[]):
                        value=dict(zip(fields,row));event=value.get('event')
                        value['event_name']=(names[event] if isinstance(names,list) and isinstance(event,int) and 0<=event<len(names)
                                             else names.get(str(event),names.get(event,str(event))) if isinstance(names,dict) else str(event))
                        yield value
                write_ledger(native_path,native_records())
                result['loss_trace']=dict(trace.summary(),call_ledger=os.path.basename(app_path),
                                          native_ledger=os.path.basename(native_path),
                                          native={key:value for key,value in native.items() if key!='rows'})
            except Exception as e:
                telemetry_errors.append({'operation':'loss_trace_export','error':repr(e),'observed_ns':time.monotonic_ns()})
                result['loss_trace']=dict(trace.summary(),valid=False,reasons=['loss trace export failed: '+repr(e)])
        if driver:
            try:driver.close()
            except Exception as e:cleanup_errors.append({'operation':'driver_close','error':repr(e)})
        try:out.close()
        except Exception as e:cleanup_errors.append({'operation':'journal_close','error':repr(e)})
    write_json(c['result'],result)
    return 1 if error else 0

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('config');sys.exit(run(read_json(a.parse_args().config)))
