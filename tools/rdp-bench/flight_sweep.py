#!/usr/bin/env python3
"""Prepare or serially execute a frozen 80-trial DATA inflight-count matrix."""
from __future__ import annotations
import argparse
import ctypes
from datetime import datetime,timezone
import json
from pathlib import Path
import random
import uuid

from flight_report import collect,dashboard,export,inspect_trial
from rdpbench.common import read_json,sha256,write_json
from rdpbench.type1 import effective_environment,one_trial,validate_profile

ROOT=Path(__file__).resolve().parent
WINDOWS=(16,32,48,64)
ENV={'RDP_BENCH_SEND_QUEUE_BYTES':'131072','RDP_BENCH_COUNTERS':'1','RDP_BENCH_OBSERVER':'1',
     'RDP_BENCH_QUEUE_OBSERVER':'1','RDP_BENCH_LOSS_TRACE':'0','RDP_BENCH_LOSS_RECOVERY_MODE':'fast',
     'RDPEUDP_RECOVERY_TRACE':'','RDPEUDP_RECOVERY_TRACE_ARM':'','RDPEUDP_TRACE':'0'}


def validate_matrix(suite,profiles):
    """The runner has one declared experiment, not arbitrary eighty trials."""
    expected_suite=read_json(ROOT/'configs/type1.flight-matrix.json')
    for key in ('version','warmup_s','measure_s','drain_s','payload_bytes','probe_hz','seeds','cases'):
        if suite.get(key)!=expected_suite[key]:raise ValueError('fixed matrix differs: '+key)
    expected_profiles=read_json(ROOT/'configs/profiles.flight.json')['profiles']
    if set(profiles)!=set(expected_profiles):raise ValueError('fixed loss profiles differ')
    for name,profile in profiles.items():
        validate_profile(profile)
        for direction in ('c2s','s2c'):
            if profile[direction]!=expected_profiles[name][direction]:raise ValueError('fixed network treatment differs: '+name+'.'+direction)


def prepare(output,library,metadata,suite=None,profiles=None):
    output=Path(output).resolve();library=Path(library).resolve()
    suite=suite or read_json(ROOT/'configs/type1.flight-matrix.json')
    profiles=profiles or read_json(ROOT/'configs/profiles.flight.json')['profiles']
    if not library.is_file():raise ValueError('native library missing')
    binary=sha256(library)
    if not isinstance(metadata,dict) or metadata.get('binary_sha256')!=binary:raise ValueError('build metadata does not match the frozen native binary')
    validate_matrix(suite,profiles)
    peer_env=dict(ENV,RDP_BENCH_MAX_INFLIGHT_PACKETS='64')
    variants=[dict(id=f'flight{window}',library=str(library),commit=metadata.get('commit'),
                   build_metadata=metadata,build_options=metadata.get('build_options'),
                   peer_library=str(library),peer_env=peer_env,
                   env=dict(ENV,RDP_BENCH_MAX_INFLIGHT_PACKETS=str(window))) for window in WINDOWS]
    for variant in variants:variant['effective_environment']=effective_environment(variant['env'])
    plan=dict(schema='freerdp.flight_matrix_plan',version=1,commit=metadata.get('commit'),
        binary_sha256=binary,library=str(library),build_metadata=metadata,suite=suite,profiles=profiles,
        variants=variants,fixed_peer=dict(library=str(library),binary_sha256=binary,environment=peer_env,effective_environment=effective_environment(peer_env)),
        expected_trials=len(suite['cases'])*len(suite['seeds'])*len(variants),
        source_hashes={str(p.relative_to(ROOT)):sha256(p) for p in sorted((ROOT/'rdpbench').glob('*.py'))},
        runner_sha256=sha256(Path(__file__)),reporter_sha256=sha256(ROOT/'flight_report.py'),
        notes=['Configured inflight DATA packet counts are 16/32/48/64; byte queue 128KiB fixed on both endpoints.',
               'Each endpoint opts into protocol LogWindow 7; fixed peer always uses configured 64.',
               'Records 16KiB, probe payload 64bytes, protocol wire MTU 1232; actual max DATA payload comes from endpoint telemetry.',
               'Loss applies independently in both directions to all datagrams, including ACKs and retransmissions.',
               'The zero-loss control keeps the same 15ms one-way delay, ±1ms jitter, 10Mbit/s cap and 30ms queue.',
               'Native reliable UDP2 transport only; no TLS/RDP setup/Windows/rendering.',
               'Every attempt retained; no failed condition is retried to obtain a passing sample.',
               'No accepted calibration receipt is created by this runner; independent raw UDP checks are calibration-only.',
               'Serial timing; report progress is updated only after a completed trial, between measured windows.'])
    if plan['expected_trials']!=80:raise ValueError('matrix must declare80 main trials')
    output.mkdir(parents=True,exist_ok=True)
    saved=output/'flight-plan.json'
    if saved.exists() and read_json(saved)!=plan:raise ValueError('frozen matrix provenance changed; select a new output directory')
    write_json(saved,plan);write_json(output/'suite.json',suite);write_json(output/'profiles.json',{'profiles':profiles})
    write_json(output/'variants.json',{'variants':variants});write_json(output/'peer-env.json',peer_env)
    if not (output/'dashboard/dashboard.json').exists():export(dashboard(plan,[]),output/'dashboard')
    return plan


def execute(output,plan):
    def frozen():
        if sha256(plan['library'])!=plan['binary_sha256']:raise ValueError('frozen native binary changed')
        if any(sha256(ROOT/name)!=value for name,value in plan['source_hashes'].items()):raise ValueError('frozen harness source changed')
        if sha256(Path(__file__))!=plan['runner_sha256'] or sha256(ROOT/'flight_report.py')!=plan['reporter_sha256']:raise ValueError('frozen runner/reporter source changed')
        for variant in plan['variants']:
            if effective_environment(variant['env'])!=variant['effective_environment']:raise ValueError('frozen endpoint environment changed')
        if effective_environment(plan['fixed_peer']['environment'])!=plan['fixed_peer']['effective_environment']:raise ValueError('frozen peer environment changed')
    frozen()
    output=Path(output);base=output/'main'
    if base.exists():raise ValueError('main output already exists; preserve completed/unfinished trials and choose a new output')
    base.mkdir();summaries=[];audited=[]
    campaign=base/'campaign.json'
    attempts=[];interrupted=False
    def save():
        failed=[r['trial_id'] for r in summaries if r['sut_outcome']!='COMPLETE' or r['validity']=='INVALID_HARNESS']
        write_json(campaign,{'version':1,'trials':summaries,'attempts':attempts,'expected_trials':80,'complete':len(summaries)==80,
                            'failed_trials':failed,'pass':len(summaries)==80 and not failed,'interrupted':interrupted})
    save()
    checks=read_json(output/'network-checks.json') if (output/'network-checks.json').exists() else None
    try:
        for case in plan['suite']['cases']:
            for seed in plan['suite']['seeds']:
                variants=list(plan['variants']);random.Random(seed).shuffle(variants)
                for variant in variants:
                    frozen()
                    ident=f"{case['id']}-{variant['id']}-s{seed}-{uuid.uuid4().hex[:8]}"
                    print('RUN',ident,flush=True)
                    attempt=dict(trial_id=ident,case_id=case['id'],variant=variant['id'],seed=seed,binary_sha256=plan['binary_sha256'],
                                 started_utc=datetime.now(timezone.utc).isoformat(),status='started')
                    attempts.append(attempt);save()
                    try:
                        summary=one_trial(variant,case,plan['profiles'][case['profile']],plan['suite'],seed,base/ident)
                    except BaseException as error:
                        interrupted=True;attempt.update(status='interrupted' if isinstance(error,KeyboardInterrupt) else 'runner_error',error=repr(error))
                        # one_trial seals even interrupted trials before re-raising.
                        # If it failed earlier, retain the unfinished declaration;
                        # report audit marks missing/unsealed artifacts explicitly.
                        path=base/ident/'summary.json'
                        summary=read_json(path) if path.exists() else dict(attempt,sut_outcome='UNKNOWN',validity='INCOMPLETE_ARTIFACT')
                        summaries.append(summary);save();raise
                    summaries.append(summary);attempt['status']='closed';save()
                    audited.append(inspect_trial(base/ident,summary,plan))
                    export(dashboard(plan,audited,'running' if len(summaries)<80 else 'complete',checks),output/'dashboard')
                    print(summary['validity'],summary['sut_outcome'],summary['reasons'],flush=True)
    except BaseException:
        interrupted=True
        raise
    finally:
        # Exhaustive final audits are outside all measured intervals. On an
        # interruption, successful closed attempts and remaining cells persist.
        save();result=collect(output,'complete' if len(summaries)==80 else 'running')
        result['meta']['interrupted']=interrupted
        export(result,output/'dashboard')
    return 0 if len(summaries)==80 and all(r['qualified'] for r in result['trials']) else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library',type=Path,required=True);parser.add_argument('--build-metadata',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args();library=args.library.expanduser().resolve()
    native=ctypes.CDLL(str(library))
    if not hasattr(native,'freerdp_bench_v1_flight_config'):parser.error('library lacks the private flight-config ABI')
    plan=prepare(args.output,library,read_json(args.build_metadata))
    if args.prepare_only:
        print(json.dumps(dict(planned_trials=80,loss_percent=[0,25,50,70],window_packets=list(WINDOWS),commit=plan['commit'])));return 0
    return execute(args.output,plan)


if __name__=='__main__':raise SystemExit(main())
