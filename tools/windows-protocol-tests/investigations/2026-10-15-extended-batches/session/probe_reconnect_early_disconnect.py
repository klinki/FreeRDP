"""Negative oracle check: closing during the reconnect observation MUST FAIL.

Requires bridge mode and the strict-disconnect + reconnect-observation driver
patches. This helper is never used by the passing profile.
"""
import sys,os,shutil,json,threading,time,signal
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[3]
os.environ.setdefault('PROBE_MODE','bridge')
if os.environ['PROBE_MODE']!='bridge':raise ValueError('This negative oracle requires bridge state events')
sys.path.insert(0,str(root))
import run
from control import Client
from profiles import CaseOptions
os.umask(0o077)
out=Path(sys.argv[1]);out.mkdir()
suite=out/'suite';shutil.copytree(Path(os.environ.get('PROBE_SUITE',root/'.cache/suite')),suite)
if os.environ.get('PROBE_BIN'): shutil.copytree(Path(os.environ['PROBE_BIN']),suite/'Bin',dirs_exist_ok=True)
cert,fp=run.make_certificate(out)
binary=Path(os.environ.get('PROBE_CLIENT',root.parents[1]/'build/protocol-tests/client/SDL/SDL3/sdl-freerdp'))
results=[]
real_client=run.Client
clients=[]
def get_client(*a,**kw):
 c=real_client(*a,**kw)
 original_start=c.start
 def start_with_early_disconnect(payload):
  original_start(payload)
  def inject():
   deadline=time.monotonic()+30
   while time.monotonic()<deadline:
    if sum(event.get('phase')=='connected' for event in c.events)>=2:
     time.sleep(2)
     if c.process is not None and c.process.poll() is None:
      c.process.send_signal(signal.SIGTERM)
      c.early_disconnect_injected=True
     return
    time.sleep(0.1)
  threading.Thread(target=inject,daemon=True).start()
 c.start=start_with_early_disconnect
 c.early_disconnect_injected=False
 clients.append(c);return c
run.Client=get_client
cases=['S3_AutoReconnection_NegativeTest_AutoReconnectionFailed']
for mode in ([os.environ['PROBE_MODE']] if os.environ.get('PROBE_MODE') else ['standard','bridge']):
 for n,case in enumerate(cases):
  r=run.run_case(binary,suite,mode,case,out/f'{mode}-{n+1}',cert,fp,75,options=CaseOptions(auto_reconnect=("Reconnect" in case or "Reconnection" in case or "HealthMonitoring" in case),redirect_drive='HealthMonitoring' in case,graceful_close='ClientInitiated' in case))
  c=clients[-1];r['early_disconnect_injected']=c.early_disconnect_injected;r['reconnect_requests']=c.reconnects;r['relay_connections']=c.relay_connections
  results.append(r);run.write_json(out/'summary.json',results);print(json.dumps(r),flush=True)
