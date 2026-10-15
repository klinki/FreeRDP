import sys, os, shutil, json
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root))
import run
from profiles import CaseOptions
os.umask(0o077)
out=Path(sys.argv[1]).resolve(); out.mkdir(mode=0o700)
suite=out/'suite'; shutil.copytree(root/'.cache/suite',suite)

if os.environ.get('PROBE_BIN'):
    shutil.copytree(Path(os.environ['PROBE_BIN']),suite/'Bin',dirs_exist_ok=True)
cert,fp=run.make_certificate(out)
binary=Path(os.environ.get('PROBE_CLIENT',root.parents[1]/'build/protocol-tests/client/SDL/SDL3/sdl-freerdp')).resolve()
# Trace only changes diagnostic verbosity; protocol parameters/assertions remain untouched.
os.environ['WLOG_FILTER']='com.freerdp.channels*:TRACE,com.freerdp.core.gcc:TRACE,com.freerdp.core.rdpeudp:DEBUG,com.freerdp.core.multitransport:DEBUG'
cases=sys.argv[2:] or ['BVT_StaticVirtualChannel_PositiveTest_CompressionNotSupported','BVT_StaticVirtualChannel_PositiveTest_VCChunkSizeNotPresent','S3_DataTransfer_V2_ClientReceiveData']
results=[]
for i,case in enumerate(cases,1):
    result=run.run_case(binary,suite,os.environ.get('PROBE_MODE','standard'),case,out/f"{os.environ.get('PROBE_MODE','standard')}-{i}",cert,fp,80,options=CaseOptions(redirect_drive='StaticVirtualChannel' in case))
    results.append(result)
    run.write_json(out/'summary.json',results)
    print(json.dumps(result),flush=True)
