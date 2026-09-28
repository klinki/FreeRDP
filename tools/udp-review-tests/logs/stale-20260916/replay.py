import ctypes as C
import argparse
import hashlib
import subprocess
from pathlib import Path
from datetime import datetime

base = Path('/tmp/freerdp-stale-20260916')
parser = argparse.ArgumentParser(description='Offline replay of the stale-session UDP capture')
parser.add_argument('--library', default='/Users/david/projects/FreeRDP/build/videotoolbox/libfreerdp/libfreerdp3.3.31.2.dylib')
parser.add_argument('--capture', type=Path, default=base/'capture-snapshot.pcapng')
parser.add_argument('--expected-stream', type=Path, help='Compare all delivered bytes with an independently reassembled encrypted stream')
args = parser.parse_args()
lib = C.CDLL(args.library)
class State(C.Structure):
    _fields_ = [('recvDataBase', C.c_uint16), ('lastAckSent', C.c_uint16), ('expectedChannelSeq', C.c_uint16), ('haveRecvData', C.c_bool), ('haveSeenAoa', C.c_bool), ('haveRealData', C.c_bool), ('recvStreamLen', C.c_size_t)]
lib.rdpeudp_test_new.restype = C.c_void_p
lib.rdpeudp_test_feed.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t]
lib.rdpeudp_test_feed.restype = C.c_int
lib.rdpeudp_test_recv_state.argtypes = [C.c_void_p, C.POINTER(State)]
lib.rdpeudp_test_recv_state.restype = C.c_int
lib.rdpeudp_test_free.argtypes = [C.c_void_p]
ctx = lib.rdpeudp_test_new()
assert ctx
state = State()
cmd = ['tshark','-n','-r',str(args.capture),'-Y','udp.srcport == 3389 && udp.dstport == 59903 && rdpudp.data.seqnum','-T','fields','-e','frame.number','-e','frame.time_epoch','-e','rdpudp.data.channelseqnumber','-e','udp.payload']
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
count = 0
last_growth = None
old_len = 0
for line in proc.stdout:
    frame,ts,cseq,raw = line.rstrip('\n').split('\t')
    payload = bytes.fromhex(raw)
    assert lib.rdpeudp_test_feed(ctx,payload,len(payload))
    assert lib.rdpeudp_test_recv_state(ctx,C.byref(state))
    count += 1
    if state.recvStreamLen != old_len:
        last_growth = (int(frame),float(ts),cseq,state.recvStreamLen)
        old_len = state.recvStreamLen
    if int(frame) in (313335,313336,313337,316801,319413):
        print('frame',frame,'channel',cseq,'next_expected',hex(state.expectedChannelSeq),'stream_bytes',state.recvStreamLen,flush=True)
err=proc.stderr.read(); rc=proc.wait()
print('Replay packets:',count,'tshark status:',rc,'error:',err.strip())
assert rc == 0 and count > 0
print('Final expected channel:',hex(state.expectedChannelSeq))
print('Final received data base:',hex(state.recvDataBase))
print('Final stream length:',state.recvStreamLen)
print('Last delivery:',last_growth)
if last_growth:
    print('Last delivery local time:',datetime.fromtimestamp(last_growth[1]).isoformat())
if args.expected_stream:
    expected = args.expected_stream.read_bytes()
    assert state.recvStreamLen == len(expected), (state.recvStreamLen, len(expected))
    lib.rdpeudp_test_recv_data.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]
    lib.rdpeudp_test_recv_data.restype = C.c_bool
    actual = C.create_string_buffer(len(expected))
    assert lib.rdpeudp_test_recv_data(ctx, 0, actual, len(expected))
    assert actual.raw == expected, 'Delivered stream differs from authenticated reassembly'
    print('Byte-for-byte stream match:', len(expected), 'bytes; SHA-256:', hashlib.sha256(expected).hexdigest())
lib.rdpeudp_test_free(ctx)
