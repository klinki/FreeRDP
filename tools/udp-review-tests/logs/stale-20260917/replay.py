from pathlib import Path
from datetime import datetime
import ctypes as C
base=Path('/tmp/freerdp-sleep-20260917')
lib=C.CDLL('/Users/david/projects/FreeRDP/build/videotoolbox/libfreerdp/libfreerdp3.3.31.2.dylib')
class State(C.Structure):
 _fields_=[('recvDataBase',C.c_uint16),('lastAckSent',C.c_uint16),('expectedChannelSeq',C.c_uint16),('haveRecvData',C.c_bool),('haveSeenAoa',C.c_bool),('haveRealData',C.c_bool),('recvStreamLen',C.c_size_t)]
lib.rdpeudp_test_new.restype=C.c_void_p
lib.rdpeudp_test_feed.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t]
lib.rdpeudp_test_feed.restype=C.c_bool
lib.rdpeudp_test_recv_state.argtypes=[C.c_void_p,C.POINTER(State)]
lib.rdpeudp_test_recv_state.restype=C.c_bool
lib.rdpeudp_test_free.argtypes=[C.c_void_p]
ctx=lib.rdpeudp_test_new(); assert ctx
st=State(); count=0; last_len=0; last_growth=None
for line in (base/'replay-input.tsv').open():
 f,t,c,raw=line.rstrip('\n').split('\t'); f=int(f)
 raw=bytes.fromhex(raw)
 assert lib.rdpeudp_test_feed(ctx,raw,len(raw))
 assert lib.rdpeudp_test_recv_state(ctx,C.byref(st))
 count+=1
 if st.recvStreamLen!=last_len:
  last_growth=(f,datetime.fromtimestamp(float(t)).isoformat(),c,st.recvStreamLen)
  last_len=st.recvStreamLen
 if f in (272950,272961,569939,569944,783467,867658,867663,867664,867742,868213,868217):
  print('frame',f,'time',datetime.fromtimestamp(float(t)).isoformat(),'channel',c,'expected',hex(st.expectedChannelSeq),'DataSeq base',hex(st.recvDataBase),'delivered bytes',st.recvStreamLen,flush=True)
print('Packets:',count,'final expected:',hex(st.expectedChannelSeq),'final DataSeq base:',hex(st.recvDataBase),'delivered bytes:',st.recvStreamLen)
print('Last stream progress:',last_growth)
lib.rdpeudp_test_free(ctx)
