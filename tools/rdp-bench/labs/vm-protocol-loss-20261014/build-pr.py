#!/usr/bin/env python3
"""Freeze a PR diagnostic/fixed source tree before building the owned runner."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import time

ROOT=Path('/Users/david/projects/FreeRDP')
BASE=Path(__file__).resolve().parent
WORKTREE=Path('/Users/david/.codex/worktrees/pr13608-flight-bench/FreeRDP')
ORIGINAL=ROOT/'build/pr13608-reference-20261013/original-source'
CONTAINER='freerdp-protocol-loss-20261014'

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,data):path.write_text(json.dumps(data,indent=2)+'\n')
def docker(args,log=None):
    if log:
        with log.open('x') as stream:r=subprocess.run(['docker','exec',CONTAINER,*args],stdout=stream,stderr=subprocess.STDOUT)
    else:r=subprocess.run(['docker','exec',CONTAINER,*args],capture_output=True,text=True)
    if r.returncode:raise RuntimeError('owned build command failed: '+str(log))
    return r

def main():
    p=argparse.ArgumentParser();p.add_argument('--kind',choices=['trace','fixed'],required=True);p.add_argument('--source-commit',required=True);p.add_argument('--resume-composition',action='store_true');p.add_argument('--resume-pointer-repair',action='store_true')
    args=p.parse_args();source=BASE/('pr-'+args.kind+'-source');build=BASE/('linux-pr-'+args.kind)
    suffix=''
    if args.resume_composition and args.resume_pointer_repair:raise RuntimeError('choose one recorded repair')
    if args.resume_pointer_repair:
        if args.kind != 'trace':raise RuntimeError('only the trace pointer compile correction may resume')
        current=BASE/'pr-trace-build.json';backup=BASE/'pr-trace-build-attempt2.json'
        if backup.exists():raise RuntimeError('pointer correction already resumed')
        if sha(source/'libfreerdp/core/rdpudp.c')!='d2051365cb9df881d22626a3b5b6767d684b410a6846f81f62dea6bc70ad9481':raise RuntimeError('failed source identity differs')
        if sha(WORKTREE/'libfreerdp/core/rdpudp.c')!='9884526390aba0142b236e8f781ad85e7f2746a58d2c32bf0cca49f4e0aabeef':raise RuntimeError('reviewed pointer correction identity differs')
        previous=json.loads(current.read_text());previous.update(outcome='FAILED_TRACE_COMPILE',error='void pointer indexed in diagnostic header peek; no timed trial launched')
        save(backup,previous)
        shutil.copy2(BASE/'linux-pr-trace-source-hashes.json',BASE/'linux-pr-trace-source-hashes-attempt2.json')
        suffix='-resume2'
    elif args.resume_composition:
        if args.kind != 'trace':raise RuntimeError('only the recorded initial trace composition failure may resume')
        current=BASE/'pr-trace-build.json';backup=BASE/'pr-trace-build-attempt1.json'
        if backup.exists():raise RuntimeError('composition already resumed')
        previous=json.loads(current.read_text());previous.update(outcome='FAILED_COMPOSITION',error='missing private benchmark include files; no timed trial launched')
        save(backup,previous)
        shutil.copy2(BASE/'linux-pr-trace-source-hashes.json',BASE/'linux-pr-trace-source-hashes-attempt1.json')
        suffix='-resume1'
    else:
        if (source/'CMakeLists.txt').exists() or build.exists():raise RuntimeError('frozen source/build already exists')
        shutil.copytree(ORIGINAL,source,dirs_exist_ok=True)
    overlays=['libfreerdp/core/rdpudp.c','libfreerdp/core/rdpudp.h',
              'libfreerdp/core/rdpudp_bench_types.inc','libfreerdp/core/rdpudp_bench_observer.inc','libfreerdp/core/rdpudp_bench_private.inc',
              'libfreerdp/core/test/TestRdpUdp.c','libfreerdp/core/test/CMakeLists.txt']
    for name in overlays:shutil.copy2(WORKTREE/name,source/name)
    files={str(path.relative_to(source)):sha(path) for path in sorted(source.rglob('*')) if path.is_file()}
    save(BASE/('linux-pr-'+args.kind+'-source-hashes.json'),files)
    originals={name:sha(ORIGINAL/name) if (ORIGINAL/name).is_file() else None for name in overlays}
    changed={name:sha(source/name) for name in overlays if sha(source/name)!=originals[name]}
    old=json.loads((ROOT/'build/vm-pr-comparison-20261014/linux-setup.json').read_text())['builds']['pr']
    flags=[f for f in old['cmake_flags'] if not f.startswith('-DBUILD_TESTING=')]
    flags += ['-DBUILD_TESTING=ON','-DBUILD_TESTING_INTERNAL=ON','-DWITH_RDP_BENCH=OFF']
    receipt=dict(kind=args.kind,source_commit=args.source_commit,base_pr_commit='a306a67c198f4493d8d2000bc10efd75b2432020',
                 source_path=str(source),source_mount='/src-'+args.kind,build_root='/bench/linux-pr-'+args.kind,
                 overlays_sha256=changed,originals_sha256=originals,
                 source_tree_sha256=hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
                 cmake_flags=flags,tracing='opt-in only; disabled in performance trials',outcome='RUNNING',started_unix_s=time.time())
    save(BASE/('pr-'+args.kind+'-build.json'),receipt)
    docker(['cmake','-S','/src-'+args.kind,'-B',receipt['build_root'],*flags],BASE/('pr-'+args.kind+'-configure'+suffix+'.log'))
    docker(['cmake','--build',receipt['build_root'],'--target','xfreerdp','TestCore','-j','4'],BASE/('pr-'+args.kind+'-build'+suffix+'.log'))
    binary=build/'client/X11/xfreerdp';receipt.update(outcome='COMPLETE',binary_path=receipt['build_root']+'/client/X11/xfreerdp',binary_sha256=sha(binary),
       libraries_sha256={str(path.relative_to(BASE)):sha(path) for path in build.rglob('*.so*') if path.is_file()},
       version=docker([receipt['build_root']+'/client/X11/xfreerdp','/version']).stdout.strip(),
       buildconfig=docker([receipt['build_root']+'/client/X11/xfreerdp','/buildconfig']).stdout.strip(),completed_unix_s=time.time())
    symbols=docker(['nm','-D',receipt['build_root']+'/libfreerdp/libfreerdp3.so']).stdout
    assert 'freerdp_bench_v1_' not in symbols
    receipt['private_benchmark_exports']=[]
    save(BASE/('pr-'+args.kind+'-build.json'),receipt)
    print('Frozen PR',args.kind,'build complete',receipt['binary_sha256'],flush=True)

if __name__=='__main__':main()
