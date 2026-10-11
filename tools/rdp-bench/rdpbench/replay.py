"""Independent wire fixtures exercising the actual production RECEIVER.
These are correctness tests, not CPU/throughput benchmarks or a complete conformance suite.
"""
import ctypes as C,json,struct,time
from .common import write_json

def wire(ds,cs,payload=b'x',dummy=False):
    layout=struct.pack('<HHH',0x5004,ds&65535,cs&65535)+payload
    short=min(7,len(layout));b=bytearray([short<<5 | (16 if dummy else 0)])+bytearray(layout)
    b.extend(b'\0'*max(0,8-len(b)));b[0],b[7]=b[7],b[0];return bytes(b)

class Receiver:
    def __init__(self,lib):
        self.l=lib;self.h=lib.rdpeudp_test_new()
        if not self.h:raise RuntimeError('test_new failed')
    def feed(self,ds,cs,p=b'x',dummy=False):
        data=wire(ds,cs,p,dummy);buf=C.create_string_buffer(data)
        if not self.l.rdpeudp_test_feed(self.h,buf,len(data)):raise AssertionError('feed failed')
    def check(self,expected):
        # A failed read is evidence against completeness, never interpreted as an empty successful read.
        b=C.create_string_buffer(max(1,len(expected)))
        if not self.l.rdpeudp_test_recv_data(self.h,0,b,len(expected)):raise AssertionError('missing in-order bytes')
        if b.raw[:len(expected)]!=expected:raise AssertionError('corrupt/reordered/duplicate delivery')
        extra=C.create_string_buffer(1)
        if self.l.rdpeudp_test_recv_data(self.h,len(expected),extra,1):raise AssertionError('unexpected extra delivery')
    def close(self):self.l.rdpeudp_test_free(self.h)

def run(path,out,wrap=False):
    lib=C.CDLL(path)
    lib.rdpeudp_test_new.restype=C.c_void_p
    lib.rdpeudp_test_free.argtypes=[C.c_void_p]
    lib.rdpeudp_test_free.restype=None
    lib.rdpeudp_test_feed.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t];lib.rdpeudp_test_feed.restype=C.c_int
    lib.rdpeudp_test_recv_data.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t];lib.rdpeudp_test_recv_data.restype=C.c_int
    lib.rdpeudp_test_set_time.argtypes=[C.c_void_p,C.c_uint64]
    lib.rdpeudp_test_check_health.argtypes=[C.c_void_p];lib.rdpeudp_test_check_health.restype=C.c_int
    cases=[]
    def case(name,fn):
        r=Receiver(lib);start=time.monotonic_ns()
        try:fn(r);cases.append({'id':name,'pass':True})
        except Exception as e:cases.append({'id':name,'pass':False,'error':repr(e)})
        finally:r.close()
    def ordered(r):
        for i,c in enumerate(b'abcdef',1):r.feed(i,i,bytes([c]))
        r.check(b'abcdef')
    def reordered(r):
        r.feed(3,3,b'c');r.check(b'');r.feed(1,1,b'a');r.check(b'a');r.feed(2,2,b'b');r.check(b'abc')
    def duplicate(r):
        r.feed(1,1,b'a');r.feed(1,1,b'a');r.feed(2,1,b'a');r.feed(3,2,b'b');r.check(b'ab')
    def lost_original(r):
        r.feed(1,1,b'a');r.feed(3,3,b'c');r.check(b'a');r.feed(4,2,b'b');r.check(b'abc')
    def dummy(r):
        r.feed(1,1,b'a');r.feed(2,0,b'ignored',True);r.feed(3,2,b'b');r.check(b'ab')
    def watchdog(r):
        lib.rdpeudp_test_set_time(r.h,100);r.feed(2,2,b'b')
        lib.rdpeudp_test_set_time(r.h,10099)
        if not lib.rdpeudp_test_check_health(r.h):raise AssertionError('watchdog fired too early')
        lib.rdpeudp_test_set_time(r.h,10101)
        if lib.rdpeudp_test_check_health(r.h):raise AssertionError('watchdog failed to fire')
    def wrap_case(r):
        expected=bytearray()
        for i in range(1,65541):
            v=bytes([i%251]);expected+=v;r.feed(i,i,v)
        r.check(bytes(expected))
    for name,fn in [('ordered',ordered),('reorder',reordered),('duplicate',duplicate),('retransmission-new-dseq',lost_original),('dummy-data-not-delivered',dummy),('gap-watchdog-current-policy',watchdog)]:case(name,fn)
    if wrap:case('natural-sequence-wrap',wrap_case)
    result={'scope':'production-receiver-correctness-only','cases':cases,'pass':all(x['pass'] for x in cases),
            'excludes':['full sender virtual-time scheduler','TLS','SYN','DVC','Windows interoperability','CPU performance']}
    write_json(out,result);print(json.dumps(result,indent=2));return 0 if result['pass'] else 1
