"""ctypes binding to the OPTIONAL private production-code shim; never substitutes a toy stack."""
import ctypes as C
import math
import os
import time

QUEUE_FIELDS = (
    'valid','queue_limit_bytes','hist_bins','hist_width_ns',
    'residence_bytes','residence_min_ns','residence_max_ns','residence_ns_bytes',
    'admission_calls','admission_min_ns','admission_max_ns','admission_total_ns',
    'capacity_blocked_calls','blocked_admission_min_ns','blocked_admission_max_ns','blocked_admission_total_ns',
    'residence_overflow_bytes','admission_overflow_calls','blocked_admission_overflow_calls',
    'peak_owned_bytes','peak_unsent_bytes','peak_inflight_bytes','peak_inflight_packets',
    'accepted_cohort_bytes','first_sent_cohort_bytes','pending_segment_count','pending_segment_bytes',
    'clock_error_count','segment_overflow_count','counter_overflow_count','measure_start_ns','measure_stop_ns',
    'own_log_window','peer_log_window','max_payload_bytes','effective_window_packets','observer_enabled',
    'reserved_0','reserved_1','reserved_2')
QUEUE_CONFIG_FIELDS = ('queue_limit_bytes','own_log_window','peer_log_window','max_payload_bytes',
                       'effective_window_packets','observer_enabled')
FLIGHT_FIELDS = ('abi_version','configured_max_inflight_packets','effective_max_inflight_packets',
    'local_protocol_capacity_packets','peer_protocol_capacity_packets','sent_storage_slots',
    'usable_storage_packets','own_log_window','peer_log_window','queue_limit_bytes',
    'current_inflight_packets','current_active_span_packets','peak_inflight_packets',
    'peak_active_span_packets','measurement_peak_inflight_packets','measurement_peak_valid')
QUEUE_BINS = 16384
QUEUE_WIDTH_NS = 125000
LOSS_TRACE_FIELDS = ('time_ns','event','data_seq','channel_seq','detail','owned_bytes',
                     'unsent_bytes','inflight_packets','active_span','retries')
LOSS_TRACE_STATS = ('enabled','valid','count','capacity','overflow','recovery_timer')
LOSS_TRACE_EVENTS = ('none','enqueue_block','enqueue_accept','data_send','retry_timer',
                     'retry_fast','ack_positive','nack_gap','retire','receive','pump_enter',
                     'pump_exit','admission_begin','admission_end','wait_begin','wait_end')
LOSS_TRACE_MAX_ROWS = 524288
LOSS_TRACE_READ_ROWS = 4096
LOSS_TRACE_DETAILS = {
    'enqueue_block':'requested application write bytes',
    'enqueue_accept':'accepted application write bytes',
    'data_send':'payload bytes in emitted logical chunk',
    'retry_timer':'milliseconds since the latest transmission attempt',
    'retry_fast':'milliseconds since the latest transmission attempt',
    'nack_gap':'distinct higher positive acknowledgements establishing a gap',
    'ack_positive':'latest transmission sequence or cumulative acknowledgement base',
    'retire':'retired logical payload bytes',
    'receive':'received wire flags',
    'pump_enter':'owned payload bytes',
    'pump_exit':'owned payload bytes',
    'admission_begin':'requested application write bytes',
    'admission_end':'requested application write bytes',
    'wait_begin':'requested wait milliseconds',
    'wait_end':'requested wait milliseconds',
}

def histogram_summary(bins, width_ns, maximum_ns, *, overflow=0):
    """Nearest-rank bounds. Scalars are upper bounds, never guessed midpoints."""
    total=sum(bins)
    result={'weight':total,'bin_width_ms':width_ns/1e6,'overflow_weight':overflow,
            'max':maximum_ns/1e6 if total or overflow else None}
    for label,q in (('p50',.5),('p95',.95),('p99',.99)):
        bounds=None
        if total and not overflow:
            target=math.ceil(total*q);cumulative=0
            for i,value in enumerate(bins):
                cumulative+=value
                if cumulative>=target:
                    bounds=[i*width_ns/1e6,(i+1)*width_ns/1e6]
                    break
        result[label]=bounds[1] if bounds else None
        result[label+'_bounds']=bounds
    return result

class Native:
    def __init__(self,path,bind,port,peer,peer_port):
        if not path:raise ValueError('native driver requires a built shared library')
        self.lib=C.CDLL(path)
        required=['open','send','read','close','stats']
        if any(not hasattr(self.lib,'freerdp_bench_v1_'+name) for name in required):
            raise RuntimeError(f'{path}: missing benchmark adapter; rebuild with -DWITH_RDP_BENCH=ON')
        self.lib.freerdp_bench_v1_open.argtypes=[C.c_char_p,C.c_uint16,C.c_char_p,C.c_uint16]
        self.lib.freerdp_bench_v1_open.restype=C.c_void_p
        self.lib.freerdp_bench_v1_send.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t]
        self.lib.freerdp_bench_v1_send.restype=C.c_int64
        self.lib.freerdp_bench_v1_read.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t,C.c_uint32]
        self.lib.freerdp_bench_v1_read.restype=C.c_int64
        self.lib.freerdp_bench_v1_close.argtypes=[C.c_void_p]
        self.lib.freerdp_bench_v1_close.restype=None
        self.lib.freerdp_bench_v1_stats.argtypes=[C.c_void_p,C.POINTER(C.c_uint64),C.c_size_t]
        self.lib.freerdp_bench_v1_stats.restype=C.c_int
        queue_names=('queue_measure','clock_ns','queue_stats','queue_hist')
        self.queue_available=all(hasattr(self.lib,'freerdp_bench_v1_'+name) for name in queue_names)
        self.queue_alignment=None
        if self.queue_available:
            self.lib.freerdp_bench_v1_queue_measure.argtypes=[C.c_void_p,C.c_uint64,C.c_uint64]
            self.lib.freerdp_bench_v1_queue_measure.restype=C.c_int
            self.lib.freerdp_bench_v1_clock_ns.argtypes=[]
            self.lib.freerdp_bench_v1_clock_ns.restype=C.c_uint64
            self.lib.freerdp_bench_v1_queue_stats.argtypes=[C.c_void_p,C.POINTER(C.c_uint64),C.c_size_t]
            self.lib.freerdp_bench_v1_queue_stats.restype=C.c_int
            self.lib.freerdp_bench_v1_queue_hist.argtypes=[C.c_void_p,C.c_uint32,C.POINTER(C.c_uint64),C.c_size_t]
            self.lib.freerdp_bench_v1_queue_hist.restype=C.c_int
        self.loss_trace_available=all(hasattr(self.lib,'freerdp_bench_v1_'+name)
                                      for name in ('loss_trace_stats','loss_trace_read','clock_ns'))
        self.loss_alignment=None
        if self.loss_trace_available:
            self.lib.freerdp_bench_v1_loss_trace_stats.argtypes=[C.c_void_p,C.POINTER(C.c_uint64),C.c_size_t]
            self.lib.freerdp_bench_v1_loss_trace_stats.restype=C.c_int
            self.lib.freerdp_bench_v1_loss_trace_read.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(C.c_uint64),C.c_size_t]
            self.lib.freerdp_bench_v1_loss_trace_read.restype=C.c_size_t
            self.lib.freerdp_bench_v1_clock_ns.argtypes=[]
            self.lib.freerdp_bench_v1_clock_ns.restype=C.c_uint64
        self.flight_available=hasattr(self.lib,'freerdp_bench_v1_flight_config')
        if self.flight_available:
            self.lib.freerdp_bench_v1_flight_config.argtypes=[C.c_void_p,C.POINTER(C.c_uint64),C.c_size_t]
            self.lib.freerdp_bench_v1_flight_config.restype=C.c_int
        self.h=self.lib.freerdp_bench_v1_open(bind.encode(),port,peer.encode(),peer_port)
        if not self.h:raise RuntimeError('private FreeRDP endpoint creation failed')
        self.buf=C.create_string_buffer(65536)
        self.configure_loss_trace()
        self.deadline_available=all(hasattr(self.lib,'freerdp_bench_v1_'+name) for name in ('deadline','deadline_reached','clock_ns'))
        if self.deadline_available:
            self.lib.freerdp_bench_v1_clock_ns.argtypes=[]
            self.lib.freerdp_bench_v1_clock_ns.restype=C.c_uint64
            self.lib.freerdp_bench_v1_deadline.argtypes=[C.c_void_p,C.c_uint64]
            self.lib.freerdp_bench_v1_deadline.restype=C.c_int
            self.lib.freerdp_bench_v1_deadline_reached.argtypes=[C.c_void_p]
            self.lib.freerdp_bench_v1_deadline_reached.restype=C.c_int
    def configure_deadline(self,end_ns):
        if not self.deadline_available:return
        sample=self._queue_clock_sample()
        if not self.lib.freerdp_bench_v1_deadline(self.h,end_ns+sample['offset_ns']):
            raise RuntimeError('native benchmark deadline configuration failed')
    def deadline_reached(self):
        return bool(self.lib.freerdp_bench_v1_deadline_reached(self.h)) if self.deadline_available else False
    def send(self,data):
        b=C.create_string_buffer(data)
        n=self.lib.freerdp_bench_v1_send(self.h,b,len(data))
        if n!=len(data):
            if self.deadline_reached():raise RuntimeError('benchmark deadline reached; reliable drain incomplete')
            raise RuntimeError(f'production send failed: {n}/{len(data)}')
    def read(self,timeout_ms=0):
        n=self.lib.freerdp_bench_v1_read(self.h,self.buf,len(self.buf),timeout_ms)
        if n<0:
            if self.deadline_reached():raise RuntimeError('benchmark deadline reached; reliable drain incomplete')
            raise RuntimeError('production receive failed / unhealthy stream')
        return self.buf.raw[:n]
    def stats(self):
        a=(C.c_uint64*8)()
        if not self.lib.freerdp_bench_v1_stats(self.h,a,8):return None
        return dict(zip(['sent_datagrams','received_datagrams','retransmissions','loss_reports',
                         'sent_window_entries','receive_buffer_bytes','send_failed','receive_failed'],a))
    def _queue_clock_sample(self):
        samples=[]
        for _ in range(5):
            before=time.monotonic_ns();native=self.lib.freerdp_bench_v1_clock_ns();after=time.monotonic_ns()
            samples.append({'python_before_ns':before,'python_after_ns':after,'native_ns':native,
                            'offset_ns':native-(before+after)//2,'uncertainty_ns':(after-before+1)//2})
        return min(samples,key=lambda x:x['python_after_ns']-x['python_before_ns'])
    def configure_queue_measurement(self,measure_ns,stop_ns):
        if not self.queue_available or os.environ.get('RDP_BENCH_QUEUE_OBSERVER')=='0':return
        initial=self._queue_clock_sample()
        self.queue_alignment={'initial':initial,'python_measure_ns':measure_ns,'python_stop_ns':stop_ns,
                              'drift_allowance_ns':250000}
        if getattr(self,'loss_trace_available',False):
            self.loss_alignment=dict(self.queue_alignment)
        if not self.lib.freerdp_bench_v1_queue_measure(self.h,measure_ns+initial['offset_ns'],stop_ns+initial['offset_ns']):
            raise RuntimeError('native queue measurement configuration failed')
    def queue_stats(self):
        if not self.queue_available:return None
        values=(C.c_uint64*len(QUEUE_FIELDS))()
        if not self.lib.freerdp_bench_v1_queue_stats(self.h,values,len(values)):return None
        fields=dict(zip(QUEUE_FIELDS,values));reasons=[]
        if not fields['observer_enabled']:
            # Configuration remains observable; zeroed disabled measurements
            # are unknown and must never become zero-latency observations.
            fields={name:value if name in QUEUE_CONFIG_FIELDS else None for name,value in fields.items()}
            return {'version':1,'scope':'queue observer disabled; configuration only','observer_enabled':False,
                    'valid':True,'reasons':[],'clock_alignment':None,'fields':fields,'summaries':{},'histograms':{}}
        if self.queue_alignment is None:
            return {'version':1,'scope':'queue measurement unavailable','observer_enabled':True,
                    'valid':False,'reasons':['native queue measurement bounds unavailable'],
                    'clock_alignment':None,'fields':fields,'summaries':{},'histograms':{}}
        if not fields['valid']:reasons.append('native queue observer invalid')
        if fields['accepted_cohort_bytes']!=fields['first_sent_cohort_bytes']:
            reasons.append('measurement cohort bytes remain unobserved after drain')
        if fields['residence_bytes']!=fields['first_sent_cohort_bytes']:
            reasons.append('residence weight differs from observed cohort bytes')
        if fields['hist_bins']!=QUEUE_BINS or fields['hist_width_ns']!=QUEUE_WIDTH_NS:
            reasons.append('unsupported queue histogram geometry')
        final=self._queue_clock_sample();alignment=dict(self.queue_alignment,final=final)
        drift=abs(final['offset_ns']-alignment['initial']['offset_ns'])
        uncertainty=final['uncertainty_ns']+alignment['initial']['uncertainty_ns']
        alignment.update(offset_drift_ns=drift,combined_uncertainty_ns=uncertainty)
        if drift>uncertainty+alignment['drift_allowance_ns']:
            reasons.append('native/Python clock alignment drift exceeds uncertainty plus 0.25 ms')
        summaries={};histograms={}
        for kind,name,count,max_field,overflow_field,units in (
                (0,'residence',fields['residence_bytes'],'residence_max_ns','residence_overflow_bytes','bytes'),
                (1,'admission',fields['admission_calls'],'admission_max_ns','admission_overflow_calls','calls'),
                (2,'blocked_admission',fields['capacity_blocked_calls'],'blocked_admission_max_ns','blocked_admission_overflow_calls','calls')):
            bins=(C.c_uint64*QUEUE_BINS)()
            if not self.lib.freerdp_bench_v1_queue_hist(self.h,kind,bins,len(bins)):
                reasons.append(name+' histogram unavailable');continue
            histogram=list(bins);overflow=fields[overflow_field]
            if sum(histogram)+overflow!=count:reasons.append(name+' histogram count mismatch')
            summary=histogram_summary(histogram,QUEUE_WIDTH_NS,fields[max_field],overflow=overflow)
            summary['weight_units']=units;summaries[name+'_ms']=summary;histograms[name]=histogram
        return {'version':1,'scope':'residence accepted during measurement, first transmission through drain; admission calls started during measurement','observer_enabled':True,
                'valid':not reasons,'reasons':reasons,'clock_alignment':alignment,'fields':fields,
                'summaries':summaries,'histograms':histograms}
    def configure_loss_trace(self):
        """Bracket the native clock before workload, including with queue observer off."""
        if not getattr(self,'loss_trace_available',False) or os.environ.get('RDP_BENCH_LOSS_TRACE')!='1':
            return
        self.loss_alignment={'initial':self._queue_clock_sample(),'drift_allowance_ns':250000}
    def loss_trace(self):
        """Export only after endpoint workload/drain stop, before closing its handle.

        Bounded dense rows retain all native evidence. Conversion to JSON/JSONL
        happens outside the transport, and incomplete traces remain invalid.
        """
        result={'version':1,'available':getattr(self,'loss_trace_available',False),
                'enabled':False,'valid':False,'reasons':[],
                'fields':list(LOSS_TRACE_FIELDS),'rows':[],
                'event_names':list(LOSS_TRACE_EVENTS),'event_detail_units':dict(LOSS_TRACE_DETAILS),
                'clock_alignment':None,'metadata':None}
        if not result['available']:
            result['reasons']=['native loss trace API unavailable'];return result
        def snapshot():
            values=(C.c_uint64*len(LOSS_TRACE_STATS))()
            if not self.lib.freerdp_bench_v1_loss_trace_stats(self.h,values,len(values)):
                return None
            return dict(zip(LOSS_TRACE_STATS,map(int,values)))
        stats=snapshot()
        if stats is None:
            result['reasons']=['native loss trace stats unavailable'];return result
        result['enabled']=bool(stats['enabled'])
        result['metadata']=dict(stats,recovery_mode='timer' if stats['recovery_timer'] else 'fast',
                                timestamp_clock='native_monotonic_ns')
        reasons=result['reasons']
        if not stats['enabled']:
            reasons.append('native loss trace disabled; observations unknown');return result
        if not stats['valid']:reasons.append('native loss trace invalid')
        if stats['overflow']:reasons.append('native loss trace buffer overflow; events missing')
        if not 0<stats['capacity']<=LOSS_TRACE_MAX_ROWS or stats['count']>stats['capacity']:
            reasons.append('unsupported native loss trace geometry');return result
        alignment=getattr(self,'loss_alignment',None)
        offset=0
        if alignment is None:
            reasons.append('native loss trace clock alignment unavailable')
        else:
            final=self._queue_clock_sample();alignment=dict(alignment,final=final)
            drift=abs(final['offset_ns']-alignment['initial']['offset_ns'])
            uncertainty=final['uncertainty_ns']+alignment['initial']['uncertainty_ns']
            alignment.update(offset_drift_ns=drift,combined_uncertainty_ns=uncertainty)
            result['clock_alignment']=alignment
            if drift>uncertainty+alignment['drift_allowance_ns']:
                reasons.append('native/Python loss trace clock alignment drift exceeds uncertainty plus 0.25 ms')
            offset=alignment['initial']['offset_ns']
            result['metadata']['timestamp_clock']='python_monotonic_ns'
        start=0;previous_ns=None
        while start<stats['count']:
            requested=min(LOSS_TRACE_READ_ROWS,stats['count']-start)
            values=(C.c_uint64*(requested*len(LOSS_TRACE_FIELDS)))()
            copied=self.lib.freerdp_bench_v1_loss_trace_read(self.h,start,values,requested)
            if copied!=requested:
                reasons.append('native loss trace read incomplete');break
            for row_index in range(copied):
                row=list(values[row_index*len(LOSS_TRACE_FIELDS):(row_index+1)*len(LOSS_TRACE_FIELDS)])
                row[0]-=offset
                if previous_ns is not None and row[0]<previous_ns:
                    if 'native loss trace timestamps moved backwards' not in reasons:
                        reasons.append('native loss trace timestamps moved backwards')
                previous_ns=row[0]
                if row[1]>=len(LOSS_TRACE_EVENTS) and 'unknown native loss trace event' not in reasons:
                    reasons.append('unknown native loss trace event')
                result['rows'].append(row)
            start+=copied
        if snapshot()!=stats:reasons.append('native loss trace changed during export; endpoint still active')
        result['valid']=not reasons
        return result
    def flight_config(self):
        if not self.flight_available:
            return None  # Optional separate ABI; older libraries remain usable.
        values=(C.c_uint64*len(FLIGHT_FIELDS))()
        if self.lib.freerdp_bench_v1_flight_config(self.h,values,len(values))!=1:
            raise RuntimeError('native flight configuration snapshot failed')
        fields=dict(zip(FLIGHT_FIELDS,map(int,values)))
        if fields['abi_version']!=1:
            raise RuntimeError('unsupported native flight configuration ABI')
        configured=fields['configured_max_inflight_packets']
        if configured not in (0,16,32,48,64) or fields['measurement_peak_valid'] not in (0,1):
            raise RuntimeError('invalid native flight configuration')
        for log,capacity in (('own_log_window','local_protocol_capacity_packets'),('peer_log_window','peer_protocol_capacity_packets')):
            if not 2<=fields[log]<=15 or fields[capacity]!=(1<<fields[log])-1:
                raise RuntimeError('invalid native flight protocol capacity')
        if not 0<fields['usable_storage_packets']<fields['sent_storage_slots']:
            raise RuntimeError('invalid native flight storage capacity')
        expected=min(configured or fields['local_protocol_capacity_packets'],fields['local_protocol_capacity_packets'],
                     fields['peer_protocol_capacity_packets'],fields['usable_storage_packets'])
        if fields['effective_max_inflight_packets']!=expected:
            raise RuntimeError('invalid native flight effective capacity')
        hard_limit=configured or fields['usable_storage_packets']
        # Already-sent packets survive a smaller peer advertisement. Only new
        # admissions use the reduced effective limit; the configured cap holds.
        if any(fields[key]>min(hard_limit,fields['usable_storage_packets']) for key in ('current_inflight_packets','peak_inflight_packets')):
            raise RuntimeError('native flight count exceeds configured/storage limit')
        measurement_peak=fields['measurement_peak_inflight_packets'] if fields['measurement_peak_valid']==1 else None
        if measurement_peak is not None and measurement_peak>fields['peak_inflight_packets']:
            raise RuntimeError('native measurement peak exceeds lifetime peak')
        return {'version':1,'fields':fields,'measurement_peak_inflight_packets':measurement_peak,
                'peak_scope':'lifetime of this native endpoint, including warmup and drain',
                'measurement_peak_scope':'measurement window; unavailable unless measurement_peak_valid=1'}
    def close(self):
        if self.h:self.lib.freerdp_bench_v1_close(self.h);self.h=None
