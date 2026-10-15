"""Negative oracle check only: abrupt owned-client exit MUST FAIL the close test.

Run with PROBE_BIN pointing at the driver with session-graceful-disconnect.patch.
This helper is not used by the passing suite or normal probe.py.
"""
import sys,os,shutil,json
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[3]
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
 def transport_only_exit():
  if c.process is not None and c.process.poll() is None and not c.graceful_closes:
   c.process.kill();c.graceful_closes+=1
 c.close_connection=transport_only_exit
 clients.append(c);return c
run.Client=get_client
cases=['S1_ConnectionTest_Disconnection_PositiveTest_ClientInitiated']
for mode in ([os.environ['PROBE_MODE']] if os.environ.get('PROBE_MODE') else ['standard','bridge']):
 for n,case in enumerate(cases):
  r=run.run_case(binary,suite,mode,case,out/f'{mode}-{n+1}',cert,fp,75,options=CaseOptions(auto_reconnect=("Reconnect" in case or "Reconnection" in case or "HealthMonitoring" in case),redirect_drive='HealthMonitoring' in case,graceful_close='ClientInitiated' in case))
  c=clients[-1];r['reconnect_requests']=c.reconnects;r['relay_connections']=c.relay_connections
  results.append(r);run.write_json(out/'summary.json',results);print(json.dumps(r),flush=True)
