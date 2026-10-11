"""ctypes binding to the OPTIONAL private production-code shim; never substitutes a toy stack."""
import ctypes as C

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
    def close(self):
        if self.h:self.lib.freerdp_bench_v1_close(self.h);self.h=None
