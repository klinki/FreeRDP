#!/usr/bin/env python3
"""Build CI-only native UDP regressions separately from measured runtimes."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import time

BASE=Path(__file__).resolve().parent
CONTAINER='freerdp-protocol-loss-20261014'

def run(command,log):
    with log.open('x') as stream:
        result=subprocess.run(['docker','exec',CONTAINER,*command],stdout=stream,stderr=subprocess.STDOUT)
    return result.returncode

def main():
    p=argparse.ArgumentParser();p.add_argument('--kind',choices=['trace','fixed'],required=True);args=p.parse_args()
    receipt=json.loads((BASE/('pr-'+args.kind+'-build.json')).read_text())
    flags=[flag for flag in receipt['cmake_flags'] if not flag.startswith(('-DBUILD_TESTING=','-DBUILD_TESTING_INTERNAL='))]
    flags+=['-DBUILD_TESTING=OFF','-DBUILD_TESTING_INTERNAL=ON']
    target='/bench/linux-tests-'+args.kind
    result=dict(scope='CI-only native regression build; separate from measured production runtimes',kind=args.kind,
                source_tree_sha256=receipt['source_tree_sha256'],cmake_flags=flags,started_unix_s=time.time())
    for label,command in [('configure',['cmake','-S',receipt['source_mount'],'-B',target,*flags]),
                           ('build',['cmake','--build',target,'--target','TestCore','-j','4']),
                           ('test',[target+'/Testing/TestCore','TestRdpUdp'])]:
        log=BASE/('native-'+args.kind+'-'+label+'.log');code=run(command,log)
        result[label]=dict(exit_code=code,log=str(log),sha256=hashlib.sha256(log.read_bytes()).hexdigest())
        if code:
            (BASE/('native-'+args.kind+'-validation.json')).write_text(json.dumps(result,indent=2)+'\n')
            raise SystemExit(code)
    result.update(outcome='PASS',completed_unix_s=time.time())
    (BASE/('native-'+args.kind+'-validation.json')).write_text(json.dumps(result,indent=2)+'\n')
    print('Native',args.kind,'UDP regressions PASS',flush=True)

if __name__=='__main__':main()
