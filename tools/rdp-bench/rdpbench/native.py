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
QUEUE_BINS = 16384
QUEUE_WIDTH_NS = 125000

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
        self.h=self.lib.freerdp_bench_v1_open(bind.encode(),port,peer.encode(),peer_port)
        if not self.h:raise RuntimeError('private FreeRDP endpoint creation failed')
        self.buf=C.create_string_buffer(65536)
    def send(self,data):
        b=C.create_string_buffer(data)
        n=self.lib.freerdp_bench_v1_send(self.h,b,len(data))
        if n!=len(data):raise RuntimeError(f'production send failed: {n}/{len(data)}')
    def read(self,timeout_ms=0):
        n=self.lib.freerdp_bench_v1_read(self.h,self.buf,len(self.buf),timeout_ms)
        if n<0:raise RuntimeError('production receive failed / unhealthy stream')
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
    def close(self):
        if self.h:self.lib.freerdp_bench_v1_close(self.h);self.h=None
