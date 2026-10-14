from pathlib import Path
import getpass,json,subprocess,warnings
BASE=Path(__file__).resolve().parent;s=json.loads((BASE/'settings.json').read_text())
with warnings.catch_warnings():
 warnings.simplefilter('error',getpass.GetPassWarning);password=getpass.getpass('Windows benchmark account password: ')
results=[]
for name,v in s['variants'].items():
 args=['docker','exec','-i','freerdp-protocol-loss-20261014','env','DISPLAY=:99',v['binary'],'/auth-only','/v:'+s['vm_ip'],'/u:'+s['username'],'/d:'+s['domain'],'/from-stdin:force','/cert:fingerprint:sha256:'+s['certificate_sha256'],'/timeout:20000','/log-level:INFO']
 with (BASE/('auth-'+name+'.log')).open('wb') as log:
  p=subprocess.run(args,input=(password+'\n').encode(),stdout=log,stderr=subprocess.STDOUT,timeout=45)
 results.append({'variant':name,'exit_code':p.returncode,'log':'auth-'+name+'.log','args':args});print(name,'authentication preflight exit',p.returncode,flush=True)
(BASE/'authentication-preflight.json').write_text(json.dumps(results,indent=2)+'\n');del password
raise SystemExit(0 if all(r['exit_code']==0 for r in results) else 1)
