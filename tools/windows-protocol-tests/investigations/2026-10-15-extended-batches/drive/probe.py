import hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root))
import run
from profiles import CaseOptions
os.umask(0o077)
out=Path(sys.argv[1]).resolve();out.mkdir(mode=0o700)
suite=out/'suite';shutil.copytree(root/'.cache/suite',suite)
if os.environ.get('PROBE_BIN'):
 shutil.copytree(os.environ['PROBE_BIN'],suite/'Bin',dirs_exist_ok=True)
cert,fp=run.make_certificate(out);binary=Path(os.environ.get('PROBE_CLIENT',root.parents[1]/'build/protocol-tests/client/SDL/SDL3/sdl-freerdp')).resolve()
config=subprocess.check_output([str(binary),'/buildconfig'],text=True,stderr=subprocess.PIPE);mask=run.graphics_compatibility_mask(config)
cases=['BVT_StaticVirtualChannel_PositiveTest_CompressionNotSupported','BVT_StaticVirtualChannel_PositiveTest_VCChunkSizeNotPresent','S7_StaticVirtualChannel_PositiveTest_CompressionSupported','S7_StaticVirtualChannel_PositiveTest_VCChunkSizePresent','S7_StaticVirtualChannel_NegativeTest_InvalidTPKTLength','S7_StaticVirtualChannel_NegativeTest_InvalidMCSLength','RDPEGFX_CapabilityExchange_Negative_IncorrectCapsDataLength','RDPEGFX_CapabilityExchange_Negative_IncorrectVersion']
results=[]
for mode in ['standard','bridge']:
 for i,case in enumerate(cases,1):
  gfx=case.startswith('RDPEGFX_');options=CaseOptions(graphics=gfx,redirect_drive=not gfx)
  result=run.run_case(binary,suite,mode,case,out/f'{mode}-{i}',cert,fp,90,options=options,graphics_mask=mask if gfx else None)
  results.append(result);run.write_json(out/'summary.json',results);print(json.dumps(result),flush=True)
