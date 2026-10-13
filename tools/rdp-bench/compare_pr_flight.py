#!/usr/bin/env python3
"""Descriptive, per-seed comparison of the original and full-PR flight matrices.

Consumes the audited JSON exports, without changing or re-running measurements.
Reported tail summaries are medians of per-trial percentiles, never pooled RTTs.
"""
from __future__ import annotations
import argparse
import csv
from datetime import datetime,timezone
import hashlib
import json
import math
from pathlib import Path
import re
import statistics

LOSSES=(0,25,50,70)
WINDOWS=(16,32,48,64)
SEEDS=(11,29,47,71,101)
METRICS=('goodput_mbps','rtt_p95_ms','rtt_p99_ms','scheduled_p99_ms')
EXPECTED={(l,w,s) for l in LOSSES for w in WINDOWS for s in SEEDS}
# Both recognized frozen endpoint implementations construct probe payload P*64.
# Reports predating an explicit meta field establish this through source proof.
PROBE64_ENDPOINT_SHA256={
    'bc9616eee3702b366e8beb86113af2f57157e6387a8192cefca601c82910004f',
    '5d0bd73910e7c6093f5ffc904e1a6bda7da8a2793f743f788ea645e6c21e52a4'}


def finite(value):return isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value) and value>=0

def integer(value):return isinstance(value,int) and not isinstance(value,bool) and value>=0

def stats(values):
    values=[v for v in values if finite(v)]
    return dict(n=len(values),median=statistics.median(values) if values else None,
                min=min(values) if values else None,max=max(values) if values else None)

def total(values):
    values=list(values);known=[v for v in values if integer(v)]
    return dict(total=sum(known) if values and len(known)==len(values) else None,
                known_total=sum(known) if known else None,known_trials=len(known),unknown_trials=len(values)-len(known))

def intact(row):return row.get('validity')!='INVALID_ARTIFACT' and row.get('artifact_errors')==[]

def qualified_eligible(row):
    counters=row.get('probes') or {}
    keys=('expected','injected','observed','missed','unobserved','timeouts','send_failures')
    service=all(integer(counters.get(k)) for k in keys)
    service=bool(service and counters['expected']==counters['injected']==counters['observed']
                 and all(counters[k]==0 for k in ('missed','unobserved','timeouts','send_failures')))
    return bool(intact(row) and row.get('qualified') is True and service)

def flag_counts(rows,key):
    flags=[(qualified_eligible(r) if key=='qualified' else r.get(key)) if intact(r) else None for r in rows]
    yes=sum(v is True for v in flags);no=sum(v is False for v in flags);unknown=len(flags)-yes-no
    return dict(count=yes if flags and not unknown else None,known_true=yes,known_false=no,unknown=unknown)

def read(path):
    def invalid(value):raise ValueError('non-finite JSON token: '+value)
    with Path(path).open(encoding='utf-8-sig') as stream:return json.load(stream,parse_constant=invalid)

def digest_file(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def condition(row):return (row['loss_percent'],row['window_packets'],row['seed'])


def validate(data,label):
    if data.get('schema')!='freerdp.flight_matrix' or data.get('meta',{}).get('status')!='complete':raise ValueError(label+': complete audited matrix required')
    rows=data.get('trials');plan=data.get('plan',{});suite=plan.get('suite',{});meta=data['meta']
    if not isinstance(rows,list) or len(rows)!=80:raise ValueError(label+': exactly80 retained trials required')
    keys=[condition(r) for r in rows]
    if any(not all(integer(v) for v in key) for key in keys) or len(set(keys))!=80 or set(keys)!=EXPECTED:raise ValueError(label+': missing/duplicate/unexpected conditions')
    ids=[r.get('trial_id') for r in rows]
    if len(ids)!=len(set(ids)) or any(not isinstance(i,str) or not re.fullmatch('[A-Za-z0-9][A-Za-z0-9_.-]*',i) for i in ids):raise ValueError(label+': duplicate/unsafe trial identity')
    fixed=dict(warmup_s=5,measure_s=30,drain_s=12,payload_bytes=16384,probe_hz=20)
    for key,value in fixed.items():
        if not finite(suite.get(key)) or suite[key]!=value:raise ValueError(label+': workload/window differs: '+key)
    if suite.get('seeds')!=list(SEEDS) or suite.get('phase')!='main':raise ValueError(label+': seed order/phase differs')
    for key,value in dict(byte_queue=131072,wire_mtu=1232,record_payload_bytes=16384,record_header_bytes=36).items():
        if not integer(meta.get(key)) or meta[key]!=value:raise ValueError(label+': fixed geometry differs: '+key)
    if meta.get('scope')!='native-udp2':raise ValueError(label+': transport scope differs')
    endpoint_hash=plan.get('source_hashes',{}).get('rdpbench/endpoint.py')
    declared_probe=meta.get('probe_payload_bytes')
    if declared_probe is not None:
        if not integer(declared_probe) or declared_probe!=64:raise ValueError(label+': probe payload differs')
        probe_proof=dict(bytes=64,basis='explicit report metadata',endpoint_sha256=endpoint_hash)
    elif endpoint_hash in PROBE64_ENDPOINT_SHA256:
        probe_proof=dict(bytes=64,basis='recognized frozen endpoint source constructs P*64',endpoint_sha256=endpoint_hash)
    else:raise ValueError(label+': 64-byte probe payload not established')
    cases=suite.get('cases',[])
    if len(cases)!=4 or {c.get('profile') for c in cases}!={f'flight_loss{l}' for l in LOSSES}:raise ValueError(label+': fixed cases differ')
    for case in cases:
        if case.get('workload')!='upload_mixed' or case.get('loss_trace') is not False or case.get('ack_feedback') is not None:raise ValueError(label+': workload/tracing treatment differs')
    controls={}
    for loss in LOSSES:
        profile=plan.get('profiles',{}).get(f'flight_loss{loss}',{})
        expected=dict(one_way_ms=15,jitter_ms=1,rate_mbps=10,queue_ms=30,overhead_bytes=28,loss=loss/100,duplicate=0,reorder=0)
        controls[loss]={}
        for direction in ('c2s','s2c'):
            value=profile.get(direction,{})
            if any(not finite(value.get(k)) or value[k]!=v for k,v in expected.items()) or value.get('blackouts_ms'):raise ValueError(label+': network treatment differs')
            controls[loss][direction]=value
    variants=plan.get('variants',[])
    if len(variants)!=4 or {v.get('env',{}).get('RDP_BENCH_MAX_INFLIGHT_PACKETS') for v in variants}!={str(w) for w in WINDOWS}:raise ValueError(label+': DATA limits differ')
    for variant in variants:
        if variant.get('env',{}).get('RDP_BENCH_SEND_QUEUE_BYTES')!='131072' or variant.get('peer_env',{}).get('RDP_BENCH_SEND_QUEUE_BYTES')!='131072' or variant.get('peer_env',{}).get('RDP_BENCH_MAX_INFLIGHT_PACKETS')!='64':raise ValueError(label+': endpoint queue/fixed peer differs')
        if variant.get('env',{}).get('RDP_BENCH_LOSS_TRACE')!='0' or variant.get('peer_env',{}).get('RDP_BENCH_LOSS_TRACE')!='0':raise ValueError(label+': endpoint trace treatment differs')
    for row in rows:
        for metric in METRICS:
            if row.get(metric) is not None and not finite(row[metric]):raise ValueError(label+': invalid metric '+metric)
        if row.get('qualified') is True and not (intact(row) and row.get('integrity_verified') is True and row.get('timing_valid') is True and row.get('outcome')=='COMPLETE' and row.get('native_failure') is False and row.get('deadline') is False and row.get('driver_failure') is not True):raise ValueError(label+': contradictory qualified row')
        if row.get('qualified') is True and (plan.get('native_adapter') or {}).get('backend')==2 and row.get('driver_failure') is not False:raise ValueError(label+': qualified PR row lacks known driver health')
        if row.get('observed_measurement_responses')==0 and any(row.get(k) is not None for k in ('rtt_p95_ms','rtt_p99_ms','scheduled_p99_ms')):raise ValueError(label+': percentile without surviving responses')
    return dict(rows={condition(r):r for r in rows},controls=controls,source_hashes=plan.get('source_hashes',{}),probe_payload=probe_proof)


def metric_value(row,metric,subset):
    if not intact(row):return None
    if subset=='qualified' and not qualified_eligible(row):return None
    if metric=='goodput_mbps' and row.get('integrity_verified') is not True:return None
    return row.get(metric) if finite(row.get(metric)) else None


def partial_record_counts(rows):
    flags=[]
    for row in rows:
        receipts=row.get('endpoint_receipts')
        known=intact(row) and isinstance(receipts,list) and len(receipts)==2 and all(integer(e.get('incomplete_record_bytes')) for e in receipts)
        flags.append(any(e['incomplete_record_bytes']>0 for e in receipts) if known else None)
    true=sum(v is True for v in flags);unknown=sum(v is None for v in flags)
    return dict(count=true if flags and not unknown else None,known_true=true,unknown=unknown)

def cohort(rows):
    usable=[r for r in rows if intact(r)]
    return dict(trials=len(rows),outcomes={name:sum(r.get('outcome')==name for r in rows) for name in ('COMPLETE','FAIL','HARNESS_FAILURE','UNKNOWN')},
        artifact_invalid=sum(not intact(r) for r in rows),harness_invalid=sum(r.get('validity')=='INVALID_HARNESS' for r in rows),
        qualified=flag_counts(rows,'qualified'),full_integrity=flag_counts(rows,'integrity_verified'),timing_valid=flag_counts(rows,'timing_valid'),
        native_failures=flag_counts(rows,'native_failure'),driver_failures=flag_counts(rows,'driver_failure'),deadlines=flag_counts(rows,'deadline'),
        early_exits=flag_counts(rows,'early_exit'),
        incomplete_streams=(flag_counts(rows,'integrity_verified')['known_false'] if not flag_counts(rows,'integrity_verified')['unknown'] else None),
        incomplete_streams_known=flag_counts(rows,'integrity_verified')['known_false'],incomplete_streams_unknown=flag_counts(rows,'integrity_verified')['unknown'],
        partial_record_trials=partial_record_counts(rows),
        metrics={subset:{metric:stats(metric_value(r,metric,subset) for r in rows) for metric in METRICS} for subset in ('qualified','diagnostic')},
        probes={key:total((r.get('probes') or {}).get(key) if intact(r) else None for r in rows) for key in ('expected','injected','observed','missed','unobserved','timeouts','send_failures')},
        observed_measurement_responses=total(r.get('observed_measurement_responses') if intact(r) else None for r in rows),
        lifetime_peak_packets=stats(r.get('peak_inflight') for r in usable),lifetime_peak_span=stats(r.get('peak_span') for r in usable))


def matched(before,after):
    comparisons={}
    for subset in ('qualified','diagnostic'):
        comparisons[subset]={}
        for metric in METRICS:
            pairs=[(seed,metric_value(before[seed],metric,subset),metric_value(after[seed],metric,subset)) for seed in SEEDS]
            pairs=[p for p in pairs if finite(p[1]) and finite(p[2])]
            comparisons[subset][metric]=dict(n=len(pairs),seeds=[p[0] for p in pairs],
                baseline=stats(p[1] for p in pairs),pr=stats(p[2] for p in pairs),delta_pr_minus_baseline=stats_signed(p[2]-p[1] for p in pairs))
    return comparisons


def stats_signed(values):
    values=list(values)
    return dict(n=len(values),median=statistics.median(values) if values else None,min=min(values) if values else None,max=max(values) if values else None)


def intrinsic(data):
    plan=data['plan'];usable=[r for r in data['trials'] if intact(r)]
    geometry={}
    for key in ('own_log_window','peer_log_window','local_protocol_capacity_packets','peer_protocol_capacity_packets','sent_storage_slots','usable_storage_packets','queue_limit_bytes'):
        values=[(r.get('flight_config') or {}).get('fields',{}).get(key) for r in usable]
        geometry[key]=dict(values=sorted({v for v in values if integer(v)}),known_trials=sum(integer(v) for v in values),unknown_trials=sum(not integer(v) for v in values))
    payload=[r.get('wire_max_payload') for r in usable]
    adapter=plan.get('native_adapter') or dict(backend=1,stats_semantics='legacy separate send/receive state; driver fault diagnostic unavailable')
    return dict(commit=plan.get('commit'),binary_sha256=plan.get('binary_sha256'),harness_source_hashes=plan.get('source_hashes'),
                measured_build_metadata=plan.get('build_metadata'),reporter=data['meta'].get('reporter'),
                declared_adapter=adapter,declared_geometry=plan.get('native_geometry'),observed_geometry=geometry,
                observed_data_payload_bytes=sorted({v for v in payload if integer(v)}),
                payload_known_trials=sum(integer(v) for v in payload),payload_unknown_trials=sum(not integer(v) for v in payload),
                network_checks=data.get('network_checks'))


def trial_reference(raw,trial_id):
    raw=Path(raw)
    base=raw/'main' if (raw/'main').is_dir() else raw
    return str(base/trial_id)

def operational_evidence(row,raw):
    if intact(row):return None
    path=Path(trial_reference(raw,row['trial_id']))/'summary.json'
    try:summary=read(path)
    except (OSError,ValueError):return dict(category='unavailable/incomplete evidence',export_validity=row.get('validity'),raw_summary_reference=str(path))
    reasons=summary.get('reasons') or []
    text=' '.join(str(r) for r in reasons).lower()
    startup=('initialization' in text and ('timed out' in text or 'timeout' in text)) or 'did not initialize' in text
    return dict(category='incomplete startup evidence' if startup else 'artifact/evidence qualification failure',
                export_validity=row.get('validity'),raw_summary_validity=summary.get('validity'),raw_summary_outcome=summary.get('sut_outcome'),
                raw_reasons=reasons,raw_summary_reference=str(path),
                interpretation='Initialization failure before measurements; not described as file tampering' if startup else 'Raw cause retained; no independent tampering inference')

def compare(baseline,pr,baseline_reference,pr_reference,baseline_raw,pr_raw):
    left=validate(baseline,'baseline');right=validate(pr,'PR')
    if left['controls']!=right['controls']:raise ValueError('directional network profiles differ between cohorts')
    if not left['source_hashes'].get('rdpbench/link.py') or left['source_hashes'].get('rdpbench/link.py')!=right['source_hashes'].get('rdpbench/link.py'):raise ValueError('Link impairment implementation differs or is unknown')
    cells=[];pairs=[]
    for loss in LOSSES:
        for window in WINDOWS:
            before={s:left['rows'][(loss,window,s)] for s in SEEDS};after={s:right['rows'][(loss,window,s)] for s in SEEDS}
            cell=dict(loss_percent=loss,window_packets=window,baseline=cohort(list(before.values())),pr=cohort(list(after.values())),matched_per_metric=matched(before,after))
            cells.append(cell)
            for seed in SEEDS:
                pair=dict(loss_percent=loss,window_packets=window,seed=seed,condition_key=f'{loss}/{window}/{seed}')
                for name,row,reference,raw in (('baseline',before[seed],baseline_reference,baseline_raw),('pr',after[seed],pr_reference,pr_raw)):
                    pair[name]=dict(report_reference=str(reference),raw_trial_reference=trial_reference(raw,row['trial_id']),
                        trial_id=row['trial_id'],outcome=row.get('outcome'),validity=row.get('validity'),
                        integrity_verified=row.get('integrity_verified'),qualified=qualified_eligible(row),declared_qualified=row.get('qualified'),timing_valid=row.get('timing_valid'),
                        native_failure=row.get('native_failure'),driver_failure=row.get('driver_failure'),deadline=row.get('deadline'),
                        metrics={m:metric_value(row,m,'diagnostic') for m in METRICS},
                        qualified_metrics={m:metric_value(row,m,'qualified') for m in METRICS},
                        probes=row.get('probes'),observed_measurement_responses=row.get('observed_measurement_responses'),
                        flight_config=row.get('flight_config'),peer_flight_config=row.get('peer_flight_config'),wire_max_payload=row.get('wire_max_payload'),
                        endpoint_receipts=row.get('endpoint_receipts'),reasons=row.get('reasons'),artifact_errors=row.get('artifact_errors'),operational_evidence=operational_evidence(row,raw))
                pairs.append(pair)
    return dict(schema='freerdp.pr_flight_comparison',version=1,generated_utc=datetime.now(timezone.utc).isoformat(),
        scope='Descriptive historical comparison of complete retained matrices; transport implementation/geometry differ; no isolated-cause or statistical-significance claim',
        definitions=dict(rate='goodput_mbps requires full CRC/hash/order integrity; full-integrity timing-invalid observations are diagnostic only; no partial-prefix substitution',
                         latency='median of per-trial surviving-response percentiles; N counts trials with a finite metric, not pooled RTT samples',
                         matched='each metric uses exactly the same finite/eligible seed intersection on both sides; PR-minus-baseline delta is descriptive',
                         probes='all scheduled probes from warmup start through measurement stop; missing/artifact-invalid counters remain unknown and prevent complete totals; measurement response count accompanies survivor tails',
                         unknown='missing/unavailable flags and counters stay null with known/unknown counts; driver state is unavailable in the original adapter',
                         qualification='qualified COMPLETE requires stream integrity, timing, health and independently reconciled complete probe service; missing or incomplete probe counters withhold qualified metrics; a failed50/70% trial never yields qualified goodput'),
        external_controls=dict(validated=True,conditions=80,seeds=list(SEEDS),loss_percent=list(LOSSES),window_packets=list(WINDOWS),
                               warmup_s=5,measure_s=30,drain_s=12,byte_queue=131072,wire_mtu=1232,payload_bytes=16384,record_header_bytes=36,probe_payload_bytes=64,probe_hz=20,
                               workload='upload_mixed',fixed_peer_packets=64,profiles=left['controls'],link_sha256=left['source_hashes']['rdpbench/link.py'],
                               probe_payload_validation=dict(baseline=left['probe_payload'],pr=right['probe_payload'])),
        datasets=dict(baseline=dict(report_reference=str(baseline_reference),raw_root=str(baseline_raw),intrinsic=intrinsic(baseline)),
                      pr=dict(report_reference=str(pr_reference),raw_root=str(pr_raw),intrinsic=intrinsic(pr))),cells=cells,per_seed=pairs)


def export(result,prefix):
    prefix=Path(prefix);prefix.parent.mkdir(parents=True,exist_ok=True)
    prefix.with_suffix('.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    names=['loss_percent','window_packets','cohort','trials','qualified_known_true','full_integrity_known_true','timing_valid_known_true','harness_invalid','native_failures','native_failure_unknown','driver_failures','driver_failure_unknown','deadlines','deadline_unknown','incomplete_streams','partial_record_trials','observed_measurement_responses',*['probe_'+k for k in ('expected','injected','observed','missed','unobserved','timeouts')],*[s+'_'+m+'_'+k for s in ('qualified','diagnostic') for m in METRICS for k in ('median','n')]]
    with prefix.with_suffix('.csv').open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=names);writer.writeheader()
        for cell in result['cells']:
            for name in ('baseline','pr'):
                c=cell[name];row=dict(loss_percent=cell['loss_percent'],window_packets=cell['window_packets'],cohort=name,trials=c['trials'],
                    qualified_known_true=c['qualified']['known_true'],full_integrity_known_true=c['full_integrity']['known_true'],timing_valid_known_true=c['timing_valid']['known_true'],harness_invalid=c['harness_invalid'],
                    native_failures=c['native_failures']['count'],native_failure_unknown=c['native_failures']['unknown'],driver_failures=c['driver_failures']['count'],driver_failure_unknown=c['driver_failures']['unknown'],
                    deadlines=c['deadlines']['count'],deadline_unknown=c['deadlines']['unknown'],incomplete_streams=c['incomplete_streams'],partial_record_trials=c['partial_record_trials']['count'],observed_measurement_responses=c['observed_measurement_responses']['total'])
                for key in ('expected','injected','observed','missed','unobserved','timeouts'):row['probe_'+key]=c['probes'][key]['total']
                for subset in ('qualified','diagnostic'):
                    for metric in METRICS:
                        for field in ('median','n'):row[subset+'_'+metric+'_'+field]=c['metrics'][subset][metric][field]
                writer.writerow(row)
    def value(x):return 'unknown' if x is None else f'{x:.3f}'
    def metric(c,name):
        s=c['metrics']['diagnostic'][name];return value(s['median'])+f" (N={s['n']})"
    lines=['# Original versus full PR flight matrix','',result['scope'], '',
        f"Original report: [{result['datasets']['baseline']['report_reference']}]({result['datasets']['baseline']['report_reference']}) · [raw trials]({result['datasets']['baseline']['raw_root']})",'',
        f"PR report: [{result['datasets']['pr']['report_reference']}]({result['datasets']['pr']['report_reference']}) · [raw trials]({result['datasets']['pr']['raw_root']})",'',
        'Native geometry, payload ceiling, source/library hashes and separate versus unified health semantics are preserved in JSON. Fixed external controls match; changing implementation and wire geometry prevents attribution to a single fix.', '',
        '| Loss | DATA cap | Original / PR full-integrity rate Mbit/s (N) | Original / PR survivor p95 ms (N) | Original / PR survivor p99 ms (N) | Matched full-rate seeds N |',
        '|---:|---:|---:|---:|---:|---:|']
    for cell in result['cells']:
        b=cell['baseline'];p=cell['pr'];lines.append(f"| {cell['loss_percent']}% | {cell['window_packets']} | {metric(b,'goodput_mbps')} / {metric(p,'goodput_mbps')} | {metric(b,'rtt_p95_ms')} / {metric(p,'rtt_p95_ms')} | {metric(b,'rtt_p99_ms')} / {metric(p,'rtt_p99_ms')} | {cell['matched_per_metric']['diagnostic']['goodput_mbps']['n']} |")
    lines+=['','Rates above require full stream integrity but are diagnostic if timing/service qualification fails. Survivor tails include failed-trial responses and do not represent unobserved probes. Each side has its own contributing N; same-seed metric intersections and qualified-only summaries are explicit in JSON. No percentile pooling or statistical significance claim is made.','',
        '| Loss / cap | Original / PR qualified | Full integrity | Timing valid | Native failures | Driver failures | Deadlines | Incomplete streams | Missed / unobserved / timeout (original → PR) |',
        '|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
    for cell in result['cells']:
        b=cell['baseline'];p=cell['pr']
        counts=lambda key:f"{b[key]['known_true']} / {p[key]['known_true']}"
        flags=lambda key:f"{b[key]['count'] if b[key]['count'] is not None else 'unknown'} / {p[key]['count'] if p[key]['count'] is not None else 'unknown'}"
        probes=lambda c:'/'.join(str(c['probes'][k]['total']) if c['probes'][k]['total'] is not None else 'unknown' for k in ('missed','unobserved','timeouts'))
        lines.append(f"| {cell['loss_percent']}% / {cell['window_packets']} | {counts('qualified')} | {counts('full_integrity')} | {counts('timing_valid')} | {flags('native_failures')} | {flags('driver_failures')} | {flags('deadlines')} | {b['incomplete_streams'] if b['incomplete_streams'] is not None else 'unknown'} / {p['incomplete_streams'] if p['incomplete_streams'] is not None else 'unknown'} | {probes(b)} → {probes(p)} |")
    lines+=['','The CSV/JSON also retain measurement-response counts, expected/injected/observed probes, artifact/harness invalidity, partial-record evidence and unknown-state counts. All80 per-seed pairs include both raw trial references and failure reasons. No failed50/70% observation is promoted to qualified goodput.','']
    operational=[(pair,name,pair[name]['operational_evidence']) for pair in result['per_seed'] for name in ('baseline','pr') if pair[name].get('operational_evidence')]
    for pair,name,evidence in operational:
        lines += [f"Evidence note: {name} {pair['condition_key']} — {evidence['category']}. {evidence['interpretation']}. Raw summary: [{evidence['raw_summary_reference']}]({evidence['raw_summary_reference']}). Export validity and null metric exclusion are preserved.",'']
    prefix.with_suffix('.md').write_text('\n'.join(lines))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-json','pr-json','baseline-raw-root','pr-raw-root','output-prefix'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    for source in (args.baseline_json,args.pr_json):
        if source.resolve() in {args.output_prefix.with_suffix(s).resolve() for s in ('.json','.csv','.md')}:parser.error('output would overwrite an input')
    result=compare(read(args.baseline_json),read(args.pr_json),args.baseline_json.resolve(),args.pr_json.resolve(),args.baseline_raw_root.resolve(),args.pr_raw_root.resolve())
    for name,path in (('baseline',args.baseline_json),('pr',args.pr_json)):result['datasets'][name]['report_sha256']=digest_file(path)
    export(result,args.output_prefix)
    print(json.dumps(dict(cells=len(result['cells']),per_seed_pairs=len(result['per_seed']),scope=result['scope'])))

if __name__=='__main__':main()
