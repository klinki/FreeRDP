#!/usr/bin/env python3
"""Freeze and demonstrate the old watchdog rejecting a compliant live peer."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import time

BASE=Path(__file__).resolve().parent
WORKTREE=Path('/Users/david/.codex/worktrees/pr13608-flight-bench/FreeRDP')
CONTAINER='freerdp-protocol-loss-20261014'

def save(name,value):(BASE/name).write_text(json.dumps(value,indent=2)+'\n')
def run(command,name):
    log=BASE/name
    with log.open('x') as stream:
        result=subprocess.run(['docker','exec',CONTAINER,*command],stdout=stream,stderr=subprocess.STDOUT)
    return dict(exit_code=result.returncode,log=str(log),sha256=hashlib.sha256(log.read_bytes()).hexdigest())

def main():
    source=BASE/'native-red-source'
    if source.exists():raise RuntimeError('red source already frozen')
    shutil.copytree(BASE/'pr-trace-source',source)
    test=WORKTREE/'libfreerdp/core/test/TestRdpUdp.c'
    assert hashlib.sha256(test.read_bytes()).hexdigest()=='d9a16c9fe0e6ef2538f68cfd7c39b09fb710f8a49ec18cc02481048227db9ca9'
    shutil.copy2(test,source/'libfreerdp/core/test/TestRdpUdp.c')
    assert hashlib.sha256((source/'libfreerdp/core/rdpudp.c').read_bytes()).hexdigest()=='9884526390aba0142b236e8f781ad85e7f2746a58d2c32bf0cca49f4e0aabeef'
    flags=json.loads((BASE/'native-trace-validation.json').read_text())['cmake_flags']
    target='/bench/linux-tests-red'
    result=dict(scope='unchanged recovery policy; new regression expected RED',started_unix_s=time.time(),
                source=str(source),source_rdpudp_sha256=hashlib.sha256((source/'libfreerdp/core/rdpudp.c').read_bytes()).hexdigest(),
                source_test_sha256=hashlib.sha256(test.read_bytes()).hexdigest(),cmake_flags=flags)
    save('native-red-validation.json',result)
    for label,command in [('configure',['cmake','-S','/bench/native-red-source','-B',target,*flags]),
                           ('build',['cmake','--build',target,'--target','TestCore','-j','4'])]:
        result[label]=run(command,'native-red-'+label+'.log');save('native-red-validation.json',result)
        if result[label]['exit_code']:raise SystemExit(result[label]['exit_code'])
    case='live_peer_can_repair_an_ordered_gap_after_16_seconds'
    result['test']=run(['env','RDPEUDP_RECOVERY_TRACE=/bench/native-red-live-peer-trace.jsonl','RDPEUDP_RECOVERY_TRACE_LIMIT=2048',
                        target+'/Testing/TestCore','TestRdpUdp','--case',case],'native-red-test.log')
    text=(BASE/'native-red-test.log').read_text()
    result['expected_red_observed']=result['test']['exit_code']!=0 and 'the stream is stuck behind a chunk the server no longer resends' in text
    result['completed_unix_s']=time.time();save('native-red-validation.json',result)
    if not result['expected_red_observed']:raise SystemExit('expected live-peer watchdog failure not observed')
    print('RED reproduced: live peer was rejected by16s ordered-progress watchdog',flush=True)

if __name__=='__main__':main()
