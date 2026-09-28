"""Replay captured server datagrams through the fixed native receiver; no network I/O."""
import argparse
import hashlib
from pathlib import Path
from datetime import datetime
import ctypes as C
parser=argparse.ArgumentParser()
parser.add_argument('--library', required=True)
parser.add_argument('--input', type=Path, required=True)
args=parser.parse_args()
lib=C.CDLL(args.library)
class State(C.Structure):
 _fields_=[('recvDataBase',C.c_uint16),('lastAckSent',C.c_uint16),('expectedChannelSeq',C.c_uint16),('haveRecvData',C.c_bool),('haveSeenAoa',C.c_bool),('haveRealData',C.c_bool),('recvStreamLen',C.c_size_t)]
lib.rdpeudp_test_new.restype=C.c_void_p
lib.rdpeudp_test_feed.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t]
lib.rdpeudp_test_feed.restype=C.c_bool
lib.rdpeudp_test_recv_state.argtypes=[C.c_void_p,C.POINTER(State)]
lib.rdpeudp_test_recv_state.restype=C.c_bool
lib.rdpeudp_test_free.argtypes=[C.c_void_p]
lib.rdpeudp_test_set_time.argtypes=[C.c_void_p,C.c_uint64]
lib.rdpeudp_test_check_health.argtypes=[C.c_void_p]
lib.rdpeudp_test_check_health.restype=C.c_bool
lib.rdpeudp_test_recv_data.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
lib.rdpeudp_test_recv_data.restype=C.c_bool
ctx=lib.rdpeudp_test_new(); assert ctx
st=State(); count=0; last_len=0; last_growth=None
for line in args.input.open():
 f,t,c,raw=line.rstrip('\n').split('\t'); f=int(f)
 raw=bytes.fromhex(raw)
 lib.rdpeudp_test_set_time(ctx,round(float(t)*1000))
 assert lib.rdpeudp_test_feed(ctx,raw,len(raw))
 assert lib.rdpeudp_test_check_health(ctx), f
 assert lib.rdpeudp_test_recv_state(ctx,C.byref(st))
 count+=1
 if st.recvStreamLen!=last_len:
  last_growth=(f,datetime.fromtimestamp(float(t)).isoformat(),c,st.recvStreamLen)
  last_len=st.recvStreamLen
 if f in (272950,272961,569939,569944,783467,867658,867663,867664,867742,868213,868217):
  print('frame',f,'time',datetime.fromtimestamp(float(t)).isoformat(),'channel',c,'expected',hex(st.expectedChannelSeq),'DataSeq base',hex(st.recvDataBase),'delivered bytes',st.recvStreamLen,flush=True)
print('Packets:',count,'final expected:',hex(st.expectedChannelSeq),'final DataSeq base:',hex(st.recvDataBase),'delivered bytes:',st.recvStreamLen)
print('Last stream progress:',last_growth)
assert count==198590 and st.expectedChannelSeq==15
assert st.recvStreamLen==127669048
sha=hashlib.sha256()
for offset in range(0,st.recvStreamLen,1024*1024):
 n=min(1024*1024,st.recvStreamLen-offset)
 out=C.create_string_buffer(n)
 assert lib.rdpeudp_test_recv_data(ctx,offset,out,n)
 sha.update(out.raw)
assert sha.hexdigest()=='362775d852643ece44d620db1e1f3ee28cabc91a254f76a399b743de5bdea4bb'
print('PASS: all three wraps, no watchdog failure, exact independently reconstructed stream SHA-256',sha.hexdigest())
lib.rdpeudp_test_free(ctx)
