from __future__ import annotations
import json,math,os,random,re,select,socket,subprocess,sys,time,uuid
from pathlib import Path
from .common import *
from .link import Relay
from .feedback import validate_feedback
ROOT=Path(__file__).resolve().parents[1]

def effective_environment(overlay):
    keys={'RDPEUDP_TRACE','RDP_BENCH_COUNTERS','RDP_BENCH_OBSERVER','RDP_BENCH_QUEUE_OBSERVER',
          'RDP_BENCH_SEND_QUEUE_BYTES','WLOG_LEVEL','WLOG_FILTER','WLOG_APPENDER'}|set(overlay)
    return {key:overlay.get(key,os.environ.get(key)) for key in sorted(keys)}

def free_port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]

def validate_profile(p):
    for direction in ('c2s','s2c'):
        d=p[direction]
        for key in ('one_way_ms','jitter_ms','rate_mbps','queue_ms'):
            if isinstance(d[key],bool) or not isinstance(d[key],(int,float)) or not math.isfinite(d[key]) or not 0<=d[key]<=1e6:raise ValueError(f'bad {direction}.{key}')
        for key in ('loss','duplicate','reorder'):
            value=d.get(key,0)
            if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or not 0<=value<=1:raise ValueError(f'{key} is a probability (0..1), not percent')
        for key in ('overhead_bytes','reorder_extra_ms'):
            value=d.get(key,0)
            if not isinstance(value,(int,float)) or not math.isfinite(value) or not 0<=value<=1e6:raise ValueError(f'bad {direction}.{key}')
        for blackout in d.get('blackouts_ms',[]):
            if len(blackout)!=2 or any(not isinstance(x,(int,float)) or not math.isfinite(x) or x<0 for x in blackout):raise ValueError('invalid blackout interval')

def identifier(value):
    if not isinstance(value,str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*',value):raise ValueError('invalid case/variant identifier')

def one_trial(variant,case,profile,config,seed,folder,driver='native',calibration=None,netns=None,direct=False):
    identifier(case['id']);identifier(variant['id'])
    validate_profile(profile)
    ack_feedback=validate_feedback(case.get('ack_feedback'))
    if ack_feedback is not None and (netns or direct or driver!='native'):
        raise ValueError('ack_feedback requires native endpoints and the Python relay')
    if direct and any(profile[d].get(k,0) for d in ('c2s','s2c') for k in ('one_way_ms','jitter_ms','rate_mbps','loss','reorder','duplicate')):raise ValueError('direct mode requires the unshaped loopback profile')
    if direct and any(profile[d].get('blackouts_ms') for d in ('c2s','s2c')):raise ValueError('direct mode cannot apply blackouts')
    folder=Path(folder).resolve();folder.mkdir(parents=True,exist_ok=False)
    env=environment(); lib=variant.get('library','')
    invariants={'profile':profile,'case':case,'warmup_s':config['warmup_s'],'measure_s':config['measure_s'],
        'drain_s':config['drain_s'],'payload_bytes':config['payload_bytes'],'probe_hz':config['probe_hz'],
        'offered_mbps':config.get('offered_mbps',0),'driver':driver,'ack_feedback':ack_feedback,
        'peer_sha256':sha256(variant['peer_library']) if variant.get('peer_library') and Path(variant['peer_library']).is_file() else None,
        'peer_environment':effective_environment(variant.get('peer_env',{})),
        'engine':'netns' if netns else 'direct' if direct else 'python-relay','python':env['python'],'system':env['system'],
        'host':env['host'],'suite_source_hash':digest({str(p.relative_to(ROOT)):sha256(p) for p in sorted((ROOT/'rdpbench').glob('*.py'))}),
        'native_adapter_hash':digest({name:sha256(ROOT/'../../libfreerdp/core'/name)
                                     for name in ('rdp_bench_private.inc','rdp_bench_queue_private.inc')
                                     if (ROOT/'../../libfreerdp/core'/name).is_file()}),
        'kernel_adapter_hash':sha256(ROOT/'linux/ns_lab.py') if netns else None}
    manifest={'version':1,'suite':'type1','trial_id':folder.name,'case_id':case['id'],'variant':variant['id'],
        'seed':seed,'profile_id':case['profile'],'started_utc':env['utc'],'environment':env,'configuration':config,
        'profile':profile,'source_commit':variant.get('commit'),'build_options':variant.get('build_options'),
        'build_metadata':variant.get('build_metadata'),'variant_environment':variant.get('env',{}),
        'effective_variant_environment':effective_environment(variant.get('env',{})),
        'binary_sha256':sha256(lib) if lib and Path(lib).is_file() else None,'invariants':invariants,
        'comparison_key':digest(invariants),'calibration':calibration,'scope':'raw-udp2-steady-state-no-TLS-no-handshake' if driver=='native' else 'HARNESS_CALIBRATION_ONLY'}
    write_json(folder/'manifest.json',manifest)
    log=Journal(folder/'timeline.jsonl'); processes=[];fds=[];relay=None;shape=None;error=None;interrupted=False
    roles={'upload':('bulk','sink'),'download':('sink','bulk'),'duplex':('duplex','duplex'),
           'probe':('probe','echo'),'mixed':('mixed_probe','bulk'),
           'upload_mixed':('bulk_probe','sink')}
    if case['workload'] not in roles:raise ValueError('unknown workload')
    try:
        left,right=free_port(),free_port()
        while right==left:right=free_port()
        bind=['127.0.0.1','127.0.0.1'];peer=bind[:];ports=[left,right]
        if netns:
            bind=[netns['left_ip'],netns['right_ip']];peer=list(reversed(bind));peer_ports=[right,left]
        elif direct:peer_ports=[right,left];shape={'engine':'direct-loopback'}
        else:
            relay=Relay(left,right,profile,seed,ack_feedback,
                        folder/'ack-feedback.jsonl' if ack_feedback is not None else None);peer_ports=relay.ports
        if netns:
            write_json(folder/'resolved-profiles.json',{'profiles':{case['profile']:profile}})
            applied=subprocess.check_output([sys.executable,str(ROOT/'linux/ns_lab.py'),'profile','--state',netns['state_file'],
                       '--profiles',str(folder/'resolved-profiles.json'),'--profile',case['profile'],'--seed',str(seed)],text=True,stderr=subprocess.STDOUT)
            write_json(folder/'netem-applied.json',json.loads(applied))
        for i,role in enumerate(roles[case['workload']]):
            c={'driver':driver,'library':lib if i==0 else variant.get('peer_library',lib),'bind':bind[i],'port':ports[i],'peer':peer[i],'peer_port':peer_ports[i],
               'role':role,'seed':seed,'payload_bytes':config['payload_bytes'],'probe_hz':config['probe_hz'],
               'offered_mbps':config.get('offered_mbps',0),
               'events':str(folder/f'endpoint-{i}.jsonl'),'result':str(folder/f'endpoint-{i}.result.json')}
            write_json(folder/f'endpoint-{i}.config.json',c)
            args=[sys.executable,'-m','rdpbench.endpoint',str(folder/f'endpoint-{i}.config.json')]
            if netns:args=['ip','netns','exec',netns['left_ns' if i==0 else 'right_ns']]+args
            e=os.environ.copy();e.update(variant.get('env',{}) if i==0 else variant.get('peer_env',{}));e['PYTHONPATH']=str(ROOT)+os.pathsep+e.get('PYTHONPATH','')
            stderr=open(folder/f'endpoint-{i}.stderr','w');fds.append(stderr)
            p=subprocess.Popen(args,cwd=ROOT,env=e,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,text=True)
            processes.append(p)
        deadline=time.monotonic()+15
        for i,p in enumerate(processes):
            ready,_,_=select.select([p.stdout],[],[],max(0,deadline-time.monotonic()))
            if not ready:raise RuntimeError(f'endpoint {i} initialization timed out')
            first=p.stdout.readline()
            if not first or json.loads(first).get('type')!='ready':raise RuntimeError(f'endpoint {i} did not initialize (check its stderr/result)')
        start=time.monotonic_ns()+300_000_000
        measure=start+int(config['warmup_s']*1e9);stop=measure+int(config['measure_s']*1e9);end=stop+int(config['drain_s']*1e9)
        control={'start_ns':start,'measure_ns':measure,'stop_ns':stop,'end_ns':end}
        write_json(folder/'clock.json',control);log.emit('armed',**control)
        if relay:relay.start(start)
        for p in processes:p.stdin.write(json.dumps(control)+'\n');p.stdin.flush()
        sleep_until(end)
        for i,p in enumerate(processes):
            try:p.wait(timeout=max(0,(end+15_000_000_000-time.monotonic_ns())/1e9))
            except subprocess.TimeoutExpired:p.kill();p.wait();error='SUT_OR_DRIVER_HANG'
            if p.returncode:error=f'endpoint {i} exited {p.returncode}'
        if netns:
            shape={'engine':'linux-netem','note':'Read qdisc counters and independent calibration receipt; no Python relay scheduling involved.'}
            for ns,key in [(netns['router_ns'],'router')]:
                shape[key]=subprocess.check_output(['ip','netns','exec',ns,'tc','-j','-s','qdisc','show'],text=True)
    except BaseException as e:
        interrupted=isinstance(e,KeyboardInterrupt)
        error=repr(e);log.emit('runner_error',error=error)
    finally:
        for p in processes:
            if p.poll() is None:p.kill();p.wait()
            if p.stdin:p.stdin.close()
            if p.stdout:p.stdout.close()
        for f in fds:f.close()
        if relay:shape=relay.close()
        write_json(folder/'shaper.json',shape or {})
        log.close()
    results=[]
    for i in range(2):
        p=folder/f'endpoint-{i}.result.json';results.append(read_json(p) if p.exists() else {'completed':False,'error':'missing endpoint result'})
    reasons=[];sut_failures=[];integrity_failures=[]
    if error:reasons.append(error)
    for r in results:
        if not r.get('completed'):integrity_failures.append(r.get('error','incomplete endpoint'))
        if r.get('queue_stats') and not r['queue_stats'].get('valid'):
            reasons.extend('queue telemetry: '+note for note in r['queue_stats'].get('reasons',['invalid observation']))
    for a,b in [(results[0],results[1]),(results[1],results[0])]:
        ac=a.get('counters',{});bc=b.get('counters',{})
        if driver=='native' and a.get('completed') and b.get('completed'):
            if ac['sent_bytes']!=bc['received_bytes'] or a['sent_sha256']!=b['received_sha256']:integrity_failures.append('application data incomplete after drain')
            if a.get('probe_timeouts') or ac.get('probes_not_injected'):sut_failures.append('probe deadlines unserved/unobserved')
            if a.get('incomplete_record_bytes'):integrity_failures.append('partial application record after drain')
    sut_failures.extend(integrity_failures)
    if shape and shape.get('engine')=='python_udp_relay_v1':
        if shape['error']:reasons.append('relay error')
        for d in ['c2s','s2c']:
            x=shape[d];p99=x['lateness_ms']['p99']
            tolerance=max(.5,profile[d]['one_way_ms']*.1)
            if p99 is not None and p99>tolerance:reasons.append(f'{d} relay p99 lateness {p99:.3f}ms exceeds {tolerance:.3f}ms')
            if x.get('socket_send_errors'):reasons.append(f'{d} relay socket send failure')
            feedback=x.get('ack_feedback')
            if feedback is not None:
                if feedback.get('classification',{}).get('unclassified'):
                    reasons.append(f'{d} ACK feedback has unclassified native datagrams')
                if feedback.get('ledger_overflow_datagrams'):
                    reasons.append(f'{d} ACK feedback ledger capacity exceeded')
    from .calibration import accepted
    pps=sum(shape.get(d,{}).get('received_datagrams',0) for d in ('c2s','s2c'))/max(.1,config['warmup_s']+config['measure_s']) if shape else 0
    if driver=='native' and (netns or direct):
        # Packet throughput still needs headroom evidence when no relay is present.
        pps=sum((r.get('native_stats') or {}).get('sent_datagrams',0) for r in results)/max(.1,config['warmup_s']+config['measure_s'])
    proof,calibration_notes=accepted(calibration,env['host'],'netns' if netns else 'direct' if direct else 'python-relay',ROOT,max(profile[d]['rate_mbps'] for d in ('c2s','s2c')),pps,invariants['peer_sha256'])
    if driver=='native' and any(r.get('native_stats') is None for r in results):
        proof=False;calibration_notes.append('native datagram observations unavailable')
    validity='INVALID_HARNESS' if reasons else 'CALIBRATION_ONLY' if driver!='native' else 'VALIDATED_LAB' if proof else 'PROVISIONAL_UNCALIBRATED'
    # VALIDATED_LAB is local operator acceptance, not independent certification.
    metrics={}
    for i,r in enumerate(results):
        prefix='client' if i==0 else 'server'
        metrics[prefix+'.goodput_mbps']=r.get('goodput_mbps')
        metrics[prefix+'.cpu_total_s']=r.get('cpu_total_s')
        metrics[prefix+'.probe_timeouts']=r.get('probe_timeouts')
        for name in ('probes_expected','probes_injected','probes_observed','probes_not_injected','unexpected_order','measured_bytes'):
            metrics[prefix+'.'+name]=r.get('counters',{}).get(name)
        for name in ['probe_rtt_ms','probe_scheduled_ms','send_call_ms','delivery_gap_ms']:
            for q in ['p50','p95','p99','max']:metrics[prefix+'.'+name+'.'+q]=r.get(name,{}).get(q)
            metrics[prefix+'.'+name+'.count']=r.get(name,{}).get('count')
        queue=r.get('queue_stats') or {}
        for name in ('residence_ms','admission_ms','blocked_admission_ms'):
            observed=queue.get('summaries',{}).get(name,{})
            for q in ('p50','p95','p99','max','weight','overflow_weight','bin_width_ms'):
                metrics[prefix+'.queue_'+name+'.'+q]=observed.get(q)
            for q in ('p50','p95','p99'):
                bounds=observed.get(q+'_bounds')
                metrics[prefix+'.queue_'+name+'.'+q+'_lower_ms']=bounds[0] if bounds else None
                metrics[prefix+'.queue_'+name+'.'+q+'_upper_ms']=bounds[1] if bounds else None
        for name in ('queue_limit_bytes','peak_owned_bytes','peak_unsent_bytes','peak_inflight_bytes','peak_inflight_packets',
                     'accepted_cohort_bytes','first_sent_cohort_bytes','pending_segment_count','pending_segment_bytes',
                     'admission_calls','capacity_blocked_calls','clock_error_count','segment_overflow_count','counter_overflow_count',
                     'own_log_window','peer_log_window','max_payload_bytes','effective_window_packets','observer_enabled'):
            metrics[prefix+'.queue_'+name]=queue.get('fields',{}).get(name)
        alignment=queue.get('clock_alignment') or {}
        metrics[prefix+'.queue_clock_offset_drift_ns']=alignment.get('offset_drift_ns')
        metrics[prefix+'.queue_clock_combined_uncertainty_ns']=alignment.get('combined_uncertainty_ns')
    integrity_verified=driver=='native' and not integrity_failures
    for direction in ('c2s','s2c'):
        feedback=shape.get(direction,{}).get('ack_feedback') if shape else None
        if feedback is not None:
            for name in ('requested_delay_ms','classified_datagrams','selected_datagrams','scheduled_datagrams','delivered_datagrams','ledger_events','ledger_overflow_datagrams'):
                metrics[direction+'.ack_feedback.'+name]=feedback.get(name)
            for kind,count in feedback.get('classification',{}).items():
                metrics[direction+'.ack_feedback.classification.'+kind]=count
            for name in ('added_delivery_ms','scheduler_overshoot_ms'):
                for q in ('count','p50','p95','p99','max','sample_count','delivered_count'):
                    metrics[direction+'.ack_feedback.'+name+'.'+q]=feedback.get(name,{}).get(q)
    metrics['c2s.verified_goodput_mbps']=results[1].get('goodput_mbps') if integrity_verified else None
    metrics['s2c.verified_goodput_mbps']=results[0].get('goodput_mbps') if integrity_verified else None
    summary={'version':1,'suite':'type1','trial_id':manifest['trial_id'],'case_id':case['id'],'variant':variant['id'],
             'seed':seed,'commit':manifest['source_commit'],'binary_sha256':manifest['binary_sha256'],'comparison_key':manifest['comparison_key'],'validity':validity,'sut_outcome':'FAIL' if sut_failures else 'COMPLETE',
             'integrity_verified':integrity_verified,'reasons':reasons,'calibration_notes':calibration_notes,'sut_failures':sut_failures,'metrics':metrics,'scope':manifest['scope'],'utc':env['utc']}
    write_json(folder/'summary.json',summary);seal(folder)
    if interrupted:raise KeyboardInterrupt
    return summary

def run_suite(args):
    config=read_json(args.suite)
    if config.get('version')!=1:raise ValueError('unsupported suite version')
    for key in ('measure_s','warmup_s','drain_s','probe_hz','payload_bytes'):
        value=config[key]
        if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value):raise ValueError(f'{key} must be a finite number')
    if not (0<config['measure_s']<=3600 and 0<=config['warmup_s']<=300 and 0<config['drain_s']<=300 and 1<=config['payload_bytes']<=32768 and 0<config['probe_hz']<=1000):raise ValueError('invalid run bounds')
    if not config['seeds'] or any(isinstance(s,bool) or not isinstance(s,int) or not 0<=s<2**31-1000003 for s in config['seeds']):raise ValueError('need nonempty integer seeds in 0..2146483643')
    if len(set(config['seeds']))!=len(config['seeds']):raise ValueError('seeds must be unique within a campaign')
    if args.direct and args.netns:raise ValueError('--direct and --netns are mutually exclusive')
    if args.netns and sys.platform!='linux':raise ValueError('--netns requires Linux')
    if any(not math.isfinite(config[k]) for k in ('measure_s','warmup_s','drain_s','probe_hz')):raise ValueError('run bounds must be finite')
    rate=config.get('offered_mbps',0)
    if not isinstance(rate,(int,float)) or not math.isfinite(rate) or rate<0:raise ValueError('invalid offered_mbps')
    if isinstance(config['payload_bytes'],bool) or not isinstance(config['payload_bytes'],int):raise ValueError('payload_bytes must be an integer')
    profiles=read_json(args.profiles)['profiles']
    variants=read_json(args.variants)['variants'] if args.variants else [{'id':'local','library':args.library,'commit':args.commit,'env':{}}]
    if not variants:raise ValueError('variants must be nonempty')
    peer_env=read_json(args.peer_env) if args.peer_env else {}
    if not isinstance(peer_env,dict) or any(not isinstance(k,str) or not isinstance(v,str) for k,v in peer_env.items()):raise ValueError('peer environment must map strings to strings')
    for collection in (variants,config['cases']):
        for item in collection:identifier(item['id'])
        if len({x['id'] for x in collection})!=len(collection):raise ValueError('duplicate identifiers')
    for v in variants:
        if not isinstance(v.get('env',{}),dict) or any(not isinstance(k,str) or not isinstance(x,str) for k,x in v.get('env',{}).items()):raise ValueError('variant environment must map strings to strings')
        if v.get('library'):v['library']=str(Path(v['library']).expanduser().resolve())
        if v.get('library'):
            metadata=Path(v['library']).parent.parent/'rdp-bench-build.json'
            if metadata.is_file():
                info=read_json(metadata)
                if Path(v['library']).is_file() and info.get('binary_sha256')==sha256(v['library']):
                    v['build_metadata']=info
                    if not v.get('commit'):v['commit']=info.get('commit')
                    if not v.get('build_options'):v['build_options']=info.get('build_options')
        v['peer_library']=str(Path(args.peer_library or variants[0].get('library','')).expanduser().resolve())
        v['peer_env']=peer_env
    calibration=read_json(args.calibration) if args.calibration else None
    topology=read_json(args.netns) if args.netns else None
    if topology:topology['state_file']=str(Path(args.netns).resolve())
    cases=[c for c in config['cases'] if not args.case or c['id']==args.case]
    if not cases:raise ValueError('no matching cases')
    for case in cases:
        validate_profile(profiles[case['profile']])
        treatment=validate_feedback(case.get('ack_feedback'))
        if treatment is not None and (args.netns or args.direct or args.driver!='native'):
            raise ValueError('ack_feedback requires native endpoints and the Python relay')
        if case['workload'] not in ('upload','download','duplex','probe','mixed','upload_mixed'):raise ValueError('unknown workload')
    if args.driver=='udp-calibration':config=dict(config,payload_bytes=1024,offered_mbps=args.offered_mbps)
    if not isinstance(config.get('offered_mbps',0),(int,float)) or not math.isfinite(config.get('offered_mbps',0)) or config.get('offered_mbps',0)<0:raise ValueError('invalid offered rate')
    base=Path(args.output).resolve();base.mkdir(parents=True,exist_ok=True)
    summaries=[]
    for case in cases:
        for seed in config['seeds']:
            ordered=list(variants);random.Random(seed).shuffle(ordered)
            for variant in ordered:
                trial=f"{case['id']}-{variant['id']}-s{seed}-{uuid.uuid4().hex[:8]}"
                print('RUN',trial,flush=True)
                result=one_trial(variant,case,profiles[case['profile']],config,seed,base/trial,args.driver,calibration,topology,args.direct)
                summaries.append(result)
                print(result['validity'],result['sut_outcome'],result['reasons'],flush=True)
    failed=[r['trial_id'] for r in summaries if r['sut_outcome']!='COMPLETE' or r['validity']=='INVALID_HARNESS']
    write_json(base/'campaign.json',{'version':1,'trials':summaries,'failed_trials':failed,'pass':not failed})
    return 1 if failed else 0
