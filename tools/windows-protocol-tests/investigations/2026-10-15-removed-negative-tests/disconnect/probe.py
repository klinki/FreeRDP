#!/usr/bin/env python3
"""Isolated diagnostic matrix; never change the shared suite or claim EOF as client rejection."""
import argparse,json,os,shutil,sys
from pathlib import Path
sys.dont_write_bytecode=True
parser=argparse.ArgumentParser()
parser.add_argument('--harness',type=Path,default=Path(__file__).resolve().parents[3])
parser.add_argument('--bin',type=Path)
parser.add_argument('--variant',choices=['baseline','wire-controls','generator-fixed'],default='baseline')
parser.add_argument('--client',type=Path)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--scenarios',nargs='+',default=['original','short','correct','long-open','long-filled','original-eof','truncated-eof','fragmented'])
args=parser.parse_args()
root=args.harness.resolve();sys.path.insert(0,str(root))
import run
from profiles import CaseOptions
os.umask(0o077)
out=args.output.resolve();out.mkdir(mode=0o700)
suite=out/'suite';shutil.copytree(root/'.cache/suite',suite)
if args.bin: shutil.copytree(args.bin,suite/'Bin',dirs_exist_ok=True)
certificate,fingerprint=run.make_certificate(out)
client=(args.client or root.parents[1]/'build/protocol-tests/client/SDL/SDL3/sdl-freerdp').resolve()
case='S1_ConnectionTest_Disconnection_NegativeTest_MCSDisconnectProviderUltimatum_InvalidTPKTLength'
results=[]
for scenario in args.scenarios:
 os.environ['WPTS_DISCONNECT_SCENARIO']=scenario
 for mode in ['standard','bridge']:
  result=run.run_case(client,suite,mode,case,out/f'{scenario}-{mode}',certificate,fingerprint,70,options=CaseOptions())
  result.update(scenario=scenario,variant=args.variant,diagnostic_only=args.variant!='baseline',server_eof_control=scenario.endswith('eof'))
  results.append(result)
  run.write_json(out/'summary.json',results)
  print(json.dumps(result),flush=True)
