from pathlib import Path
import struct
base=Path('/tmp/freerdp-stale-20260916')
chunks={}
for line in (base/'channel-payloads.tsv').open():
 frame,cs,raw=line.rstrip('\n').split('\t')
 b=bytearray.fromhex(raw); b[0],b[7]=b[7],b[0]
 flags=int.from_bytes(b[1:3],'little')&0xfff
 pos=3
 if flags&1: pos+=7+(b[pos+6]&15)
 if flags&0x40: pos+=1
 if flags&0x100: pos+=3
 if flags&0x10: pos+=2
 assert flags&4
 pos+=2
 if flags&8: pos+=3+(4 if b[pos+2]&128 else 0)+(b[pos+2]&127)
 cseq=int.from_bytes(b[pos:pos+2],'little'); assert cseq==int(cs,16)
 n=cseq+(65536 if int(frame)>=313337 and cseq<32768 else 0)
 body=bytes(b[pos+2:])
 if n in chunks: assert chunks[n]==body, (frame,cs,'different retransmission')
 else: chunks[n]=body
missing=[i for i in range(min(chunks),max(chunks)+1) if i not in chunks]
print('unique channel packets',len(chunks),'range',min(chunks),max(chunks),'missing',missing)
stream=b''.join(chunks[k] for k in sorted(chunks))
(base/'channel-stream-skip-gap.bin').write_bytes(stream)
print('stream length',len(stream),'first record header',stream[:5].hex())
pos=count=0; around=[]
while pos+5<=len(stream):
 typ,ver,n=struct.unpack_from('>BHH',stream,pos)
 if typ not in (20,21,22,23) or ver not in (0x301,0x302,0x303) or n>18432:
  print('INVALID RECORD HEADER at',pos,'after',count,'records'); break
 if count<3: print('record',count,'type',typ,'version',hex(ver),'length',n)
 if pos+n+5>len(stream):
  print('Incomplete final record at',pos,'need',n,'have',len(stream)-pos-5); break
 if pos+n+5>61857465 and len(around)<4: around.append((count,pos,typ,n))
 pos+=5+n; count+=1
print('complete records',count,'bytes parsed',pos,'remaining',len(stream)-pos)
print('records across/after wrap',around)
