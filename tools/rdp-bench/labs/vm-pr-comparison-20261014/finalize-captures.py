from pathlib import Path
import json,os,signal,time
BASE=Path('/bench/campaign');all_receipts=[]
for folder in sorted(BASE.iterdir()):
 if not folder.is_dir() or not (folder/'controller.json').exists():continue
 owner=json.loads((folder/'controller.json').read_text())['owner'];rows=[]
 for label in ('offered','received'):
  path=folder/(label+'-capture-owned.json')
  if not path.exists():continue
  r=json.loads(path.read_text());assert r['owner']==owner
  pid=r['pid'];proc=Path('/proc')/str(pid);item={'label':label,'pid':pid,'owner':owner,'scope':'supplementary finalization; original capture cleanup error preserved'}
  if not proc.exists():item['status']='already_exited';rows.append(item);continue
  fd=os.pidfd_open(pid)
  try:
   fields=(proc/'stat').read_text().rsplit(')',1)[1].split();assert int(fields[19])==r['start_ticks'],'birth-time differs'
   argv=[v.decode() for v in (proc/'cmdline').read_bytes().split(b'\0') if v]
   assert argv==[r['executable'],*r['arguments']], 'capture argv identity differs'
   assert (proc/'comm').read_text().strip()=='tcpdump'
   item.update(identity_guard='pidfd + exact birth time + exact argv + comm + owner',cpu_seconds=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK'),process_state=fields[0],arguments=argv)
   if fields[0] in ('Z','X'):item['status']='already_exited'
   else:signal.pidfd_send_signal(fd,signal.SIGINT);item['status']='signalled_owned_capture'
  finally:os.close(fd)
  deadline=time.monotonic()+10
  while proc.exists() and time.monotonic()<deadline:
   try:
    f=(proc/'stat').read_text().rsplit(')',1)[1].split()
    if f[0] in ('Z','X'):break
   except FileNotFoundError:break
   time.sleep(.05)
  item['still_live']=proc.exists() and (proc/'stat').read_text().rsplit(')',1)[1].split()[0] not in ('Z','X')
  log=folder/(label+'-capture.log');item['footer']=log.read_text(errors='replace').splitlines()[-5:] if log.exists() else []
  rows.append(item)
 (folder/'capture-finalization.json').write_text(json.dumps({'scope':'Capture tasks continued beyond intended trial cleanup; restrict any wire analysis to actual measurement UTC bounds','receipts':rows},indent=2)+'\n');all_receipts.extend(rows)
print(json.dumps({'captures':len(all_receipts),'still_live':sum(r.get('still_live',False) for r in all_receipts),'capture_process_cpu_seconds':sum(r.get('cpu_seconds',0) for r in all_receipts)},indent=2))
