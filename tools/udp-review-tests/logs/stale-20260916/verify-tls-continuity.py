from pathlib import Path
import ctypes as C
import hashlib,hmac,struct
base=Path('/tmp/freerdp-stale-20260916')
s=(base/'channel-stream-skip-gap.bin').read_bytes(); records=[]; pos=0
while pos<len(s):
 n=int.from_bytes(s[pos+3:pos+5],'big'); records.append((pos,s[pos:pos+5],s[pos+5:pos+5+n]));pos+=5+n
hello=records[0][2]; off=39+hello[38]; suite=int.from_bytes(hello[off:off+2],'big')
print('TLS cipher suite',hex(suite))
assert suite in (0x1301,0x1302)
keylen=16 if suite==0x1301 else 32; digest=hashlib.sha256 if keylen==16 else hashlib.sha384
lib=C.CDLL('/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib')
lib.EVP_CIPHER_CTX_new.restype=C.c_void_p
lib.EVP_CIPHER_CTX_free.argtypes=[C.c_void_p]
lib.EVP_aes_128_gcm.restype=C.c_void_p;lib.EVP_aes_256_gcm.restype=C.c_void_p
lib.EVP_DecryptInit_ex.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
lib.EVP_CIPHER_CTX_ctrl.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_void_p]
lib.EVP_DecryptUpdate.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int),C.c_void_p,C.c_int]
lib.EVP_DecryptFinal_ex.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
def expand(secret,label,n):
 l=b'tls13 '+label; info=struct.pack('>H',n)+bytes([len(l)])+l+b'\x00'
 return hmac.new(secret,info+b'\x01',digest).digest()[:n]
def verify(secret,record,seq):
 _,aad,data=record; key=expand(secret,b'key',keylen); iv=expand(secret,b'iv',12)
 nonce=(int.from_bytes(iv,'big')^seq).to_bytes(12,'big')
 ctx=lib.EVP_CIPHER_CTX_new(); out=C.create_string_buffer(len(data)+32); n=C.c_int();tail=C.c_int()
 try:
  cipher=lib.EVP_aes_128_gcm() if keylen==16 else lib.EVP_aes_256_gcm()
  assert lib.EVP_DecryptInit_ex(ctx,cipher,None,None,None)==1
  assert lib.EVP_CIPHER_CTX_ctrl(ctx,9,12,None)==1
  assert lib.EVP_DecryptInit_ex(ctx,None,None,key,nonce)==1
  assert lib.EVP_DecryptUpdate(ctx,None,C.byref(n),aad,len(aad))==1
  assert lib.EVP_DecryptUpdate(ctx,out,C.byref(n),data[:-16],len(data)-16)==1
  assert lib.EVP_CIPHER_CTX_ctrl(ctx,0x11,16,data[-16:])==1
  return lib.EVP_DecryptFinal_ex(ctx,C.byref(out,n.value),C.byref(tail))==1
 finally: lib.EVP_CIPHER_CTX_free(ctx)
secrets=[]
for line in Path('/Users/david/rdp-debug/20260916-200524-5735/tls-secrets.txt').read_text().splitlines():
 f=line.split()
 if len(f)==3 and f[0]=='SERVER_TRAFFIC_SECRET_0':secrets.append(bytes.fromhex(f[2]))
match=None
for i,r in enumerate(records[:20]):
 if r[1][0]!=23:continue
 for secret in secrets:
  if verify(secret,r,0):match=(i,secret)
assert match, 'Could not identify application-data traffic secret'
start,secret=match; print('Application record start index',start)
assert all(r[1][0]==23 for r in records[start:])
targets=[i for i,r in enumerate(records) if r[0]+len(r[1])+len(r[2])>61857465]
print('Checking records crossing/after gap:',len(targets))
for i in targets:
 assert verify(secret,records[i],i-start), ('TLS authentication failure at record',i)
print('PASS: all',len(targets),'records spanning and following the sequence gap authenticate without inserting or removing any payload bytes.')
