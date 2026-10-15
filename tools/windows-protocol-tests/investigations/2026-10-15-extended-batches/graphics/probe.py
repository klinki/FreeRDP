import hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root))
import run
from profiles import CaseOptions
os.umask(0o077)
out=Path(sys.argv[1]).resolve(); out.mkdir(mode=0o700)
suite=out/'suite'; shutil.copytree(root/'.cache/suite',suite)
source=Path(os.environ['WPTS_SOURCE'])/'TestSuites/RDP/Client/src/TestSuite/RDPEGFX'
existing=set(run.profile_cases('extended'))
cases=[]
for name in ('RdpegfxCapabilityExchangeTest.cs','RDPegfxCompressionTest.cs','RdpegfxFrameSegmentationTest.cs','RdpegfxH264CodecTest.cs'):
    cases += [c for c in re.findall(r'public void (\w+)\(', (source/name).read_text()) if c not in existing]
cert,fp=run.make_certificate(out)
binary=root.parents[1]/'build/protocol-tests/client/SDL/SDL3/sdl-freerdp'
config=subprocess.check_output([str(binary),'/buildconfig'],text=True,stderr=subprocess.PIPE)
mask=run.graphics_compatibility_mask(config)
run.write_json(out/'provenance.json',dict(microsoft=json.loads((suite/'provenance.json').read_text()),client_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),client_buildconfig=config,graphics_mask=mask,cases=cases))
results=[]
for mode in ('standard','bridge'):
    for i,case in enumerate(cases,1):
        print(f'[{mode} {i}/{len(cases)}] {case}',flush=True)
        result=run.run_case(binary,suite,mode,case,out/f'{mode}-{i}',cert,fp,90,options=CaseOptions(graphics=True),graphics_mask=mask)
        results.append(result); run.write_json(out/'summary.json',results)
        print(json.dumps(result),flush=True)

raise SystemExit(0 if all(result["passed"] for result in results) else 1)
