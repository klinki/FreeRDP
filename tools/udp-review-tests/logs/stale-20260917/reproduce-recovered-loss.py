"""Socketless regression using production receiver and tiny synthetic bodies.
Only difference between control and failure is one absent original datagram;
the same channel bytes are subsequently delivered by retransmission.
"""
from pathlib import Path
import ctypes as C
import struct
import argparse
parser=argparse.ArgumentParser()
parser.add_argument("--library", default="/Users/david/projects/FreeRDP/build/videotoolbox/libfreerdp/libfreerdp3.3.31.2.dylib")
parser.add_argument("--expect-stall", action="store_true", help="Verify the pre-fix failure instead")
args=parser.parse_args()
lib=C.CDLL(args.library)
class State(C.Structure):
 _fields_=[('recvDataBase',C.c_uint16),('lastAckSent',C.c_uint16),('expectedChannelSeq',C.c_uint16),('haveRecvData',C.c_bool),('haveSeenAoa',C.c_bool),('haveRealData',C.c_bool),('recvStreamLen',C.c_size_t)]
lib.rdpeudp_test_new.restype=C.c_void_p
lib.rdpeudp_test_feed.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t];lib.rdpeudp_test_feed.restype=C.c_bool
lib.rdpeudp_test_recv_state.argtypes=[C.c_void_p,C.POINTER(State)];lib.rdpeudp_test_recv_state.restype=C.c_bool
lib.rdpeudp_test_free.argtypes=[C.c_void_p]
def feed(ctx,d,c):
 b=bytearray(b'\xe0'+struct.pack('<HHH',0x5004,d,c)+b'X')
 b[0],b[7]=b[7],b[0]
 assert lib.rdpeudp_test_feed(ctx,bytes(b),len(b))
def snapshot(ctx,label):
 s=State();assert lib.rdpeudp_test_recv_state(ctx,C.byref(s))
 print(label,'expected',hex(s.expectedChannelSeq),'DataSeq base',hex(s.recvDataBase),'delivered',s.recvStreamLen,flush=True)
 return s
for lose_original in (False,True):
 ctx=lib.rdpeudp_test_new();assert ctx
 try:
  print('CASE:', 'one original lost then channel recovered' if lose_original else 'all originals received',flush=True)
  for c in range(1,65536):feed(ctx,c,c)
  feed(ctx,0,1)
  assert snapshot(ctx,'first wrap').expectedChannelSeq==2
  if not lose_original:feed(ctx,1,2)
  feed(ctx,2,3)
  feed(ctx,3,2) # identical retransmission in both cases
  s=snapshot(ctx,'after recovery');assert s.expectedChannelSeq==4 and s.recvStreamLen==65538
  for c in range(4,65536):feed(ctx,c,c)
  feed(ctx,0,1)
  s=snapshot(ctx,'second wrap')
  stalled=lose_original and args.expect_stall
  assert s.expectedChannelSeq==(0 if stalled else 2)
  assert s.recvStreamLen==(131070 if stalled else 131071)
 finally:lib.rdpeudp_test_free(ctx)
print('PASS: reproduced the previous stall.' if args.expect_stall else 'PASS: recovered packet loss no longer blocks the next wrap; both cases deliver identical bytes.')
