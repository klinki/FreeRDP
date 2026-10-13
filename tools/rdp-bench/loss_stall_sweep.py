#!/usr/bin/env python3
"""Matched native loss-recovery campaign with separately traced causal diagnostics."""
from __future__ import annotations
import argparse,ctypes,json,subprocess,sys
from pathlib import Path
from rdpbench.common import read_json,sha256,write_json

ROOT=Path(__file__).resolve().parent
CAPS=(48,64)
SEEDS=(11,29,47,71,101)
PROFILES=(("clean","ten_mbit_clean"),("impaired","ten_mbit"))
PEER_ENV={'RDP_BENCH_SEND_QUEUE_BYTES':'131072','RDP_BENCH_QUEUE_OBSERVER':'1',
          'RDP_BENCH_COUNTERS':'1','RDP_BENCH_OBSERVER':'1','RDP_BENCH_LOSS_TRACE':'0',
          'RDP_BENCH_LOSS_RECOVERY_MODE':'timer'}

def configuration(profiles,seeds,warmup,measure,drain,trace=False):
    return {'version':1,'warmup_s':warmup,'measure_s':measure,'drain_s':drain,
            'payload_bytes':16384,'probe_hz':20,'seeds':list(seeds),
            'cases':[{'id':label+'-upload-mixed','profile':profile,'workload':'upload_mixed',
                      'loss_trace':trace} for label,profile in profiles]}

def checked_metadata(library,path):
    metadata=read_json(path) if path else {}
    if metadata and metadata.get('binary_sha256')!=sha256(library):
        raise ValueError(f'build metadata does not match {library}')
    return metadata

def prepare(output,candidate,reference,candidate_metadata=None,reference_metadata=None):
    """Pin binaries, peer, treatments and source before any trials start."""
    output=Path(output);candidate=Path(candidate);reference=Path(reference)
    candidate_metadata=candidate_metadata or {};reference_metadata=reference_metadata or {}
    output.mkdir(parents=True,exist_ok=True)
    configs={
        'smoke':configuration(PROFILES,(11,),1,3,3,True),
        'main':configuration(PROFILES,SEEDS,5,30,10),
        'diagnostic-29':configuration(PROFILES[1:],(29,),5,30,10,True),
        'diagnostic-101':configuration(PROFILES[1:],(101,),3,15,10,True),
        'overhead':configuration(PROFILES[:1],(11,29),3,10,5),
    }
    configs['overhead']['cases']=[dict(configs['overhead']['cases'][0],id='clean-trace-'+name,loss_trace=trace)
                                  for name,trace in (('off',False),('on',True))]
    def variant(label,cap,library,metadata,mode=None):
        env=dict(PEER_ENV,RDP_BENCH_SEND_QUEUE_BYTES=str(cap*1024),RDP_BENCH_LOSS_TRACE='0')
        if mode is not None:env['RDP_BENCH_LOSS_RECOVERY_MODE']=mode
        return {'id':f'{label}-q{cap}','library':str(library),'build_metadata':metadata,
                'commit':metadata.get('resolved_native_source_commit',metadata.get('commit')),'build_options':metadata.get('build_options'),
                'env':env}
    main_variants=[variant(label,cap,library,metadata,'timer' if label=='reference' else 'fast') for cap in CAPS
                   for label,library,metadata in (('reference',reference,reference_metadata),
                                                  ('candidate',candidate,candidate_metadata))]
    diagnostic_variants=[variant(mode,cap,candidate,candidate_metadata,mode)
                         for cap in CAPS for mode in ('timer','fast')]
    variants={'main':main_variants,'smoke':[v for v in diagnostic_variants if v['id'].endswith('q48')],
              'diagnostic-29':diagnostic_variants,'diagnostic-101':diagnostic_variants,
              'overhead':[variant('fast',48,candidate,candidate_metadata,'fast')]}
    plan={'version':1,'candidate_library':str(candidate),'candidate_sha256':sha256(candidate),
          'reference_library':str(reference),'reference_sha256':sha256(reference),
          'candidate_metadata':candidate_metadata,'reference_metadata':reference_metadata,
          'fixed_peer_library':str(reference),'fixed_peer_sha256':sha256(reference),
          'fixed_peer_environment':PEER_ENV,'configurations':configs,'variants':variants,
          'source_hashes':{str(p.relative_to(ROOT)):sha256(p) for p in sorted((ROOT/'rdpbench').glob('*.py'))},
          'driver_sha256':sha256(Path(__file__)),'reporter_sha256':sha256(ROOT/'loss_stall_report.py'),
          'profiles_sha256':sha256(ROOT/'configs/profiles.json'),
          'notes':['Main has 40 loaded trials: matched reference/candidate, 48/64 KiB, both 10 Mbit/s profiles, five seeds.',
                   'Variant order is deterministically shuffled by seed within each profile; all failures are retained.',
                   'Traced diagnostics compare timer/fast recovery in the same candidate binary; the old fixed peer lacks native trace ABI.',
                   'Main omits loss tracing. Trace-on/off overhead uses identical mode, queue, seeds and durations in a separate phase.',
                   'Probe miss threshold remains >200 ms and probe injection timestamp precedes native admission.',
                   'Seeds determine packet-index impairment decisions; traffic changes mean logical losses are not identical between versions.',
                   'No TLS, Windows, desktop or validated calibration claim is implied by the raw native transport campaign.']}
    saved=output/'loss-sweep-plan.json'
    if saved.exists() and read_json(saved)!=plan:raise ValueError('Existing sweep provenance changed; use a new output directory')
    write_json(saved,plan);write_json(output/'peer-env.json',PEER_ENV)
    for phase,config in configs.items():
        write_json(output/f'{phase}-suite.json',config)
        write_json(output/f'{phase}-variants.json',{'variants':variants[phase]})
    return configs,variants

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidate',required=True);p.add_argument('--reference',required=True)
    p.add_argument('--candidate-metadata');p.add_argument('--reference-metadata')
    p.add_argument('--output',required=True);p.add_argument('--calibration')
    p.add_argument('--phase',choices=('all','smoke','main','diagnostic-29','diagnostic-101','overhead'),default='all')
    p.add_argument('--prepare-only',action='store_true');args=p.parse_args()
    candidate=Path(args.candidate).expanduser().resolve();reference=Path(args.reference).expanduser().resolve()
    for library in (candidate,reference):
        if not library.is_file():p.error(f'missing native library {library}')
        native=ctypes.CDLL(str(library))
        if any(not hasattr(native,'freerdp_bench_v1_'+name) for name in ('queue_measure','clock_ns','queue_stats','queue_hist')):
            p.error(f'{library} lacks queue observer ABI')
    if any(not hasattr(ctypes.CDLL(str(candidate)),'freerdp_bench_v1_'+name) for name in ('loss_trace_stats','loss_trace_read')):
        p.error('candidate lacks native loss trace ABI')
    try:
        configs,variants=prepare(Path(args.output).expanduser().resolve(),candidate,reference,
                                checked_metadata(candidate,args.candidate_metadata),checked_metadata(reference,args.reference_metadata))
    except ValueError as error:p.error(str(error))
    output=Path(args.output).expanduser().resolve()
    if args.prepare_only:
        print(json.dumps({phase:len(c['cases'])*len(c['seeds'])*len(variants[phase]) for phase,c in configs.items()},sort_keys=True));return 0
    phases=('smoke','diagnostic-29','diagnostic-101','overhead','main') if args.phase=='all' else (args.phase,)
    saved=output/'loss-sweep-status.json';status=read_json(saved) if saved.exists() else {'version':1,'phases':{}}
    failed=False
    for phase in phases:
        target=output/phase
        if target.exists():
            if status['phases'].get(phase,{}).get('complete'):
                failed|=status['phases'][phase].get('returncode',1)!=0;continue
            p.error(f'{target} exists without a completed phase; preserve it and select another output directory')
        expected=len(configs[phase]['cases'])*len(configs[phase]['seeds'])*len(variants[phase])
        command=[sys.executable,'-B',str(ROOT/'bench.py'),'type1','--suite',str(output/f'{phase}-suite.json'),
                 '--variants',str(output/f'{phase}-variants.json'),'--profiles',str(ROOT/'configs/profiles.json'),
                 '--peer-library',str(reference),'--peer-env',str(output/'peer-env.json'),'--output',str(target)]
        if args.calibration:command+=['--calibration',str(Path(args.calibration).expanduser().resolve())]
        print(f'PHASE {phase}: {expected} trials',flush=True)
        result=subprocess.run(command,cwd=ROOT.parent.parent)
        campaign=target/'campaign.json';rows=read_json(campaign).get('trials',[]) if campaign.is_file() else []
        entry={'complete':campaign.is_file() and len(rows)==expected,'expected_trials':expected,
               'observed_trials':len(rows),'returncode':result.returncode,
               'failed_trials':[r['trial_id'] for r in rows if r.get('sut_outcome')!='COMPLETE' or r.get('validity')=='INVALID_HARNESS']}
        status['phases'][phase]=entry;status['pass']=all(x['complete'] and x['returncode']==0 for x in status['phases'].values())
        write_json(saved,status);failed|=result.returncode!=0
        if not entry['complete']:return result.returncode or 1
        report=subprocess.run([sys.executable,'-B',str(ROOT/'loss_stall_report.py'),'--root',str(output),
                               '--output',str(output/'report')],cwd=ROOT.parent.parent)
        if report.returncode:return report.returncode
    return 1 if failed else 0

if __name__=='__main__':raise SystemExit(main())
