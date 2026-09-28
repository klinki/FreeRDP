"""Inspect only protocol metadata; never print UDP payloads."""
from pathlib import Path
from datetime import datetime
from collections import Counter
base=Path('/tmp/freerdp-sleep-20260917')
acks=[]
for line in (base/'stall-udp-wire.tsv').open():
 frame,t,src,raw=line.rstrip('\n').split('\t')
 b=bytearray.fromhex(raw)
 if len(b)<8:continue
 b[0],b[7]=b[7],b[0]
 flags=int.from_bytes(b[1:3],'little')&0xfff
 if src=='100.118.217.48' and flags&1:
  acks.append((float(t),int.from_bytes(b[3:5],'little'),int(frame)+844661))
stall=1789629485.802162
before=[a for a in acks if a[0]<stall];after=[a for a in acks if a[0]>=stall]
for label,(t,seq,frame) in [('last ACK before stall',before[-1]),('first ACK after stall',after[0]),('final ACK',after[-1])]:
 print(label,datetime.fromtimestamp(t).isoformat(),'DataSeq',hex(seq),'merged frame',frame)
print('First ACK delay after blocked arrival:',round(after[0][0]-stall,6),'seconds')
print('ACKs during stalled interval:',len(after))
attempts=Counter()
for line in (base/'inbound-sequences.tsv').open():
 frame,t,port,seq,channel=line.rstrip('\n').split('\t')
 if int(frame)>=867663 and channel:attempts[int(channel,0)]+=1
print('Post-stall channel attempts:',{hex(k):v for k,v in attempts.items()})
print('Unique channel chunks:',len(attempts),'Retransmissions:',sum(attempts.values())-len(attempts))
print('Stall until disconnect:',round(1789629757.698742-stall,6),'seconds')
