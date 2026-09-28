"""Offline TLS integrity check. No secrets or decrypted payloads are printed/saved."""
from pathlib import Path
import ctypes as C
import hashlib,hmac,struct
from collections import Counter
base=Path('/tmp/freerdp-sleep-20260917')
chunks={}; highest=0; duplicates=0
for line in (base/'replay-input.tsv').open():
 f,t,cs,raw=line.rstrip('\n').split('\t')
 if not cs:continue
 b=bytearray.fromhex(raw);b[0],b[7]=b[7],b[0]
 flags=int.from_bytes(b[1:3],'little')&0xfff; p=3
 if flags&1:p+=7+(b[p+6]&15)
 if flags&0x40:p+=1
 if flags&0x100:p+=3
 if flags&0x10:p+=2
 assert flags&4;p+=2
 if flags&8:p+=3+(4 if b[p+2]&128 else 0)+(b[p+2]&127)
 c=int.from_bytes(b[p:p+2],'little');assert c==int(cs,0)
 seq=(highest&~65535)|c
 if seq<highest-32768:seq+=65536
 elif seq>highest+32768:seq-=65536
 highest=max(highest,seq);body=bytes(b[p+2:])
 if seq in chunks:assert chunks[seq]==body;duplicates+=1
 else:chunks[seq]=body
missing=[n for n in range(1,highest+1) if n not in chunks]
assert missing==[65536,131072,196608],missing
stall_offset=sum(len(v) for k,v in chunks.items() if k<196608)
assert stall_offset==127664092,stall_offset
stream=b''.join(chunks[k] for k in sorted(chunks))
print('Unique channel chunks:',len(chunks),'identical retransmissions:',duplicates,'missing sequence labels:',missing,flush=True)
print('Reassembled TLS bytes:',len(stream),'bytes held after stalled offset:',len(stream)-stall_offset,flush=True)
records=[];pos=0
while pos<len(stream):
 assert pos+5<=len(stream)
 typ,ver,n=struct.unpack_from('>BHH',stream,pos)
 assert typ in (20,21,22,23) and ver in (0x301,0x302,0x303) and n<=18432
 assert pos+5+n<=len(stream)
 records.append((pos,stream[pos:pos+5],stream[pos+5:pos+5+n]));pos+=5+n
print('Complete TLS records:',len(records),'trailing bytes:',len(stream)-pos,flush=True)
hello=records[0][2];off=39+hello[38];suite=int.from_bytes(hello[off:off+2],'big')
assert suite in (0x1301,0x1302)
keylen=16 if suite==0x1301 else 32;digest=hashlib.sha256 if keylen==16 else hashlib.sha384
lib=C.CDLL('/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib')
lib.EVP_CIPHER_CTX_new.restype=C.c_void_p;lib.EVP_CIPHER_CTX_free.argtypes=[C.c_void_p]
lib.EVP_aes_128_gcm.restype=C.c_void_p;lib.EVP_aes_256_gcm.restype=C.c_void_p
lib.EVP_DecryptInit_ex.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
lib.EVP_CIPHER_CTX_ctrl.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_void_p]
lib.EVP_DecryptUpdate.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int),C.c_void_p,C.c_int]
lib.EVP_DecryptFinal_ex.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
def expand(secret,label,n):
 l=b'tls13 '+label;info=struct.pack('>H',n)+bytes([len(l)])+l+b'\x00'
 return hmac.new(secret,info+b'\x01',digest).digest()[:n]
def decrypt(secret,record,seq):
 _,aad,data=record;key=expand(secret,b'key',keylen);iv=expand(secret,b'iv',12)
 nonce=(int.from_bytes(iv,'big')^seq).to_bytes(12,'big')
 ctx=lib.EVP_CIPHER_CTX_new();out=C.create_string_buffer(len(data)+32);n=C.c_int();tail=C.c_int()
 try:
  cipher=lib.EVP_aes_128_gcm() if keylen==16 else lib.EVP_aes_256_gcm()
  assert lib.EVP_DecryptInit_ex(ctx,cipher,None,None,None)==1
  assert lib.EVP_CIPHER_CTX_ctrl(ctx,9,12,None)==1
  assert lib.EVP_DecryptInit_ex(ctx,None,None,key,nonce)==1
  assert lib.EVP_DecryptUpdate(ctx,None,C.byref(n),aad,len(aad))==1
  assert lib.EVP_DecryptUpdate(ctx,out,C.byref(n),data[:-16],len(data)-16)==1
  assert lib.EVP_CIPHER_CTX_ctrl(ctx,0x11,16,data[-16:])==1
  if lib.EVP_DecryptFinal_ex(ctx,C.byref(out,n.value),C.byref(tail))!=1:return None
  return out.raw[:n.value+tail.value]
 finally:lib.EVP_CIPHER_CTX_free(ctx)
secrets=[]
for line in Path('/Users/david/rdp-debug/20260917-080921-93335/tls-secrets.txt').read_text().splitlines():
 fields=line.split()
 if len(fields)==3 and fields[0]=='SERVER_TRAFFIC_SECRET_0':secrets.append(bytes.fromhex(fields[2]))
match=None
for i,r in enumerate(records[:20]):
 if r[1][0]!=23:continue
 for secret in secrets:
  if decrypt(secret,r,0) is not None:match=(i,secret);break
 if match:break
assert match
start,secret=match
assert all(r[1][0]==23 for r in records[start:])
targets=[i for i,r in enumerate(records) if r[0]+5+len(r[2])>stall_offset]
inner_types=Counter();tunnel_pdus=0;dvc=Counter();payload_bytes=0
for i in targets:
 plain=decrypt(secret,records[i],i-start);assert plain is not None,('TLS authentication failure',i)
 plain=plain.rstrip(b'\x00');typ=plain[-1];data=plain[:-1];inner_types[typ]+=1
 if typ==23:
  p=0
  while p<len(data):
   assert len(data)-p>=4
   action=data[p]&15;plen=int.from_bytes(data[p+1:p+3],'little');hlen=data[p+3]
   assert data[p]>>4==0 and action==2 and hlen>=4 and p+hlen+plen<=len(data)
   body=data[p+hlen:p+hlen+plen];tunnel_pdus+=1;payload_bytes+=len(body)
   if body:
    cmd=body[0]>>4;cb=body[0]&3;assert cb<3
    width=1<<cb;assert len(body)>=1+width
    cid=int.from_bytes(body[1:1+width],'little');dvc[(cmd,cid)]+=1
   p+=hlen+plen
print('PASS: authenticated all',len(targets),'TLS records spanning/following the third wrap without any inserted or removed payload bytes.')
print('TLS inner content types/counts:',dict(inner_types),'Tunnel DATA PDUs:',tunnel_pdus,'higher-layer bytes:',payload_bytes,'DVC (command, channel) counts:',dict(dvc))
print('Encrypted reassembly SHA-256:',hashlib.sha256(stream).hexdigest())
# Identify protocol channel names from early CREATE requests only.
# Do not print application payloads or arbitrary decrypted strings.
channel_names={}
for i in range(start,min(start+200,len(records))):
 plain=decrypt(secret,records[i],i-start)
 assert plain is not None
 plain=plain.rstrip(b'\x00')
 if plain[-1]!=23:continue
 data=plain[:-1];p=0
 while p+4<=len(data):
  act=data[p]&15;plen=int.from_bytes(data[p+1:p+3],'little');hlen=data[p+3]
  if data[p]>>4 or hlen<4 or p+hlen+plen>len(data):break
  body=data[p+hlen:p+hlen+plen]
  if act==2 and body and body[0]>>4==1 and body[0]&3<3:
   width=1<<(body[0]&3);cid=int.from_bytes(body[1:1+width],'little')
   name=body[1+width:].split(b'\x00',1)[0]
   if name.startswith(b'Microsoft::') or name in (b'AUDIO_PLAYBACK_DVC',b'AUDIO_INPUT',b'rdpgfx',b'rdpecam'):
    channel_names[cid]=name.decode('ascii')
  p+=hlen+plen
print('Known protocol channel names:',channel_names)
