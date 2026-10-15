import json,os,shutil,sys
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root))
import run
from profiles import CaseOptions
os.umask(0o077)
out=Path(sys.argv[1]);out.mkdir();suite=out/'suite';shutil.copytree(root/'.cache/suite',suite)
shutil.copy2(os.environ['PROBE_ADAPTER'],suite/'Bin/RDP_ClientAdapter.dll')
cert,fp=run.make_certificate(out);binary=Path(os.environ.get('PROBE_CLIENT',str(root.parents[1]/'build/protocol-tests/client/SDL/SDL3/sdl-freerdp')))
cases=['S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidMCSLength','S1_ConnectionTest_CapabilityExchange_NegativeTest_DemandActive_InvalidPDULength','BVT_ConnectionTest_CapabilityExchange_PositiveTest']
results=[]
for mode in ['standard','bridge']:
 for i,case in enumerate(cases,1):
  os.environ['DEMAND_PACKET_OUTPUT']=str(out/f'{mode}-{i}.hex')
  result=run.run_case(binary,suite,mode,case,out/f'{mode}-{i}',cert,fp,90,options=CaseOptions())
  results.append(result);run.write_json(out/'summary.json',results);print(json.dumps(result),flush=True)
