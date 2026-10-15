#!/usr/bin/env python3
"""Build an isolated suite with wire diagnostics and optionally the TPKT fixture repair."""
import argparse,json,subprocess,sys
from pathlib import Path
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
sys.path.insert(0,str(ROOT))
from profiles import PIN,suite_patches
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,required=True,help='Clean pinned Microsoft Git checkout')
parser.add_argument('--output',type=Path,required=True,help='New temporary build directory')
parser.add_argument('--fix',action='store_true',help='Also repair the adapter generator')
args=parser.parse_args()
source=args.source.resolve();out=args.output.resolve()
if subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()!=PIN['revision']:
 raise SystemExit('Source revision does not match suite.json')
if subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=source):
 raise SystemExit('Source checkout has tracked changes')
out.mkdir(parents=True);build=out/'source';build.mkdir()
archive=out/'source.tar'
subprocess.run(['git','archive','--format=tar','-o',str(archive),PIN['revision'],'AssemblyInfo','ProtoSDK','TestSuites/RDP/Client','LICENSE.txt'],cwd=source,check=True)
subprocess.run(['tar','-xf',str(archive),'-C',str(build)],check=True)
subprocess.run(['git','init','--quiet'],cwd=build,check=True)
patches=[ROOT/p['path'] for p in suite_patches()]+[HERE/'diagnostic-wire.patch']
if args.fix: patches.append(HERE/'tpkt-short-length.patch')
for patch in patches: subprocess.run(['git','apply',str(patch)],cwd=build,check=True)
(out/'global.json').write_text(json.dumps({'sdk':{'version':'8.0.100','rollForward':'latestFeature','allowPrerelease':False}}))
with (out/'build.log').open('w') as log:
 subprocess.run(['dotnet','publish',str(build/'TestSuites/RDP/Client/src/TestSuite/RDP_ClientTestSuite.csproj'),'-c','Release','-o',str(out/'Bin'),f'-p:TestSuiteRoot={build}','-p:NuGetAudit=false','--disable-build-servers','-m:1'],cwd=out,stdout=log,stderr=subprocess.STDOUT,check=True)
print(out/'Bin')
