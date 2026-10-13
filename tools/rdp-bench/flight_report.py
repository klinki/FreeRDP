#!/usr/bin/env python3
"""Audited native UDP2 inflight-count matrix and portable dashboard data."""
from __future__ import annotations
import argparse
import csv
from datetime import datetime,timezone
import math
from pathlib import Path
import re
import statistics

from rdpbench.common import audit,digest,read_json,write_json
from rdpbench.native import FLIGHT_FIELDS
from rdpbench.type1 import endpoint_result

LOSSES=(0,25,50,70)
WINDOWS=(16,32,48,64)
METRICS=('goodput_mbps','rtt_p95_ms','rtt_p99_ms','scheduled_p99_ms')
VALID=('PROVISIONAL_UNCALIBRATED','VALIDATED_LAB')


def finite(value):
    return isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value)


def stats(values):
    values=[v for v in values if finite(v)]
    return dict(n=len(values),median=statistics.median(values) if values else None,
                min=min(values) if values else None,max=max(values) if values else None)


def known_sum(values):
    values=list(values)
    return sum(values) if values and all(isinstance(x,int) and not isinstance(x,bool) and x>=0 for x in values) else None


def count(value):
    return isinstance(value,int) and not isinstance(value,bool) and value>=0


def flight_fields(config):
    """Audit the separate private ABI without assuming span equals live count."""
    if config is None:return {}
    fields=config.get('fields',{})
    if not count(config.get('version')) or config.get('version')!=1 or set(fields)!=set(FLIGHT_FIELDS) or not all(count(v) for v in fields.values()):
        raise ValueError('invalid flight configuration schema')
    if fields['abi_version']!=1 or fields['configured_max_inflight_packets'] not in (0,*WINDOWS) or fields['measurement_peak_valid'] not in (0,1):
        raise ValueError('invalid flight configuration values')
    for log,capacity in (('own_log_window','local_protocol_capacity_packets'),('peer_log_window','peer_protocol_capacity_packets')):
        if not 2<=fields[log]<=15 or fields[capacity]!=(1<<fields[log])-1:raise ValueError('invalid flight protocol capacity')
    if not 0<fields['usable_storage_packets']<fields['sent_storage_slots']:raise ValueError('invalid flight storage capacity')
    configured=fields['configured_max_inflight_packets'];storage=fields['usable_storage_packets']
    if fields['effective_max_inflight_packets']!=min(configured or fields['local_protocol_capacity_packets'],fields['local_protocol_capacity_packets'],fields['peer_protocol_capacity_packets'],storage):
        raise ValueError('invalid flight effective capacity')
    # A real smaller peer advertisement can grandfather existing packets above
    # effective capacity. The configured/storage hard bound still applies.
    if any(fields[k]>min(configured or storage,storage) for k in ('current_inflight_packets','peak_inflight_packets')):
        raise ValueError('flight count exceeds configured/storage bound')
    if fields['current_inflight_packets']>fields['peak_inflight_packets'] or fields['current_active_span_packets']>fields['peak_active_span_packets']:
        raise ValueError('flight current count/span exceeds lifetime peak')
    if fields['measurement_peak_valid'] and fields['measurement_peak_inflight_packets']>fields['peak_inflight_packets']:
        raise ValueError('measurement flight peak exceeds lifetime peak')
    return fields


def object_at(path):
    value=read_json(path)
    if not isinstance(value,dict):raise ValueError(str(path)+': expected object')
    return value


def inspect_trial(folder,declared,plan):
    variant=next((v for v in plan['variants'] if v['id']==declared.get('variant')),None)
    case=next((c for c in plan['suite']['cases'] if c['id']==declared.get('case_id')),None)
    if not variant or not case:raise ValueError('trial is outside the frozen matrix')
    window=int(variant['env']['RDP_BENCH_MAX_INFLIGHT_PACKETS'])
    loss=round(plan['profiles'][case['profile']]['c2s']['loss']*100)
    row=dict(trial_id=declared['trial_id'],loss_percent=loss,window_packets=window,
        seed=declared.get('seed'),outcome=declared.get('sut_outcome','UNKNOWN'),validity='INVALID_ARTIFACT',
        integrity_verified=False,qualified=False,integrity_valid=False,timing_valid=False,
        goodput_mbps=None,rtt_p95_ms=None,rtt_p99_ms=None,scheduled_p99_ms=None,
        partial_crc_payload_mbps=None,missed=None,timeouts=None,unobserved=None,
        peak_inflight=None,peak_span=None,measurement_peak_inflight=None,
        flight_config=None,peer_flight_config=None,wire_max_payload=None,
        native_failure=None,deadline=None,first_failure_ms=None,measurement_observed_s=None,
        early_exit=None,observed_measurement_responses=None,
        probes=dict(expected=None,injected=None,observed=None,missed=None,unobserved=None,timeouts=None,send_failures=None),
        network={},reasons=[],artifact_errors=[],qualification_notes=[],endpoint_receipts=[],
        goodput_scope='unavailable',latency_scope='surviving responses to measurement-scheduled probes')
    try:
        errors=audit(folder)
        if errors:raise ValueError('; '.join(errors))
        summary=object_at(folder/'summary.json');manifest=object_at(folder/'manifest.json')
        for key in ('trial_id','variant','case_id','seed','binary_sha256'):
            if summary.get(key)!=declared.get(key) or manifest.get(key)!=declared.get(key):raise ValueError('sealed identity differs: '+key)
        if summary.get('profile_id')!=case['profile'] or manifest.get('profile_id')!=case['profile']:raise ValueError('profile identity differs')
        if manifest['profile']!=plan['profiles'][case['profile']] or manifest['invariants']['profile']!=manifest['profile']:raise ValueError('profile parameters differ')
        inv=manifest['invariants']
        if inv.get('case')!=case or inv.get('driver')!='native' or inv.get('engine')!='python-relay' or inv.get('loss_trace') is not False or inv.get('ack_feedback') is not None or inv.get('offered_mbps')!=0:
            raise ValueError('workload/driver/relay treatment differs')
        if inv.get('suite_source_hash')!=digest(plan['source_hashes']):raise ValueError('frozen harness source differs')
        for key in ('warmup_s','measure_s','drain_s','payload_bytes','probe_hz'):
            if inv[key]!=plan['suite'][key]:raise ValueError('condition differs: '+key)
        if inv['peer_sha256']!=plan['binary_sha256']:raise ValueError('fixed peer differs')
        if manifest['binary_sha256']!=plan['binary_sha256']:raise ValueError('DUT binary differs')
        if manifest['effective_variant_environment'].get('RDP_BENCH_MAX_INFLIGHT_PACKETS')!=str(window):raise ValueError('declared inflight cap differs')
        if inv['peer_environment'].get('RDP_BENCH_MAX_INFLIGHT_PACKETS')!='64':raise ValueError('fixed peer cap differs')
        for actual,expected in ((manifest['effective_variant_environment'],variant['effective_environment']),(inv['peer_environment'],plan['fixed_peer']['effective_environment'])):
            if any(actual.get(k)!=v for k,v in expected.items()):raise ValueError('frozen endpoint environment differs')
        endpoints=[endpoint_result(folder/f'endpoint-{i}.result.json') for i in (0,1)]
        if any(e.get('result_read_error') for e in endpoints):raise ValueError('endpoint schema invalid')
        shape=object_at(folder/'shaper.json');timing=summary['validity'] in VALID and not shape.get('error');pending=0
        for direction in ('c2s','s2c'):
            v=shape[direction]
            if not all(count(v.get(k)) for k in ('received_datagrams','random_drops','queue_drops','blackout_drops','delivered_datagrams','socket_send_errors','duplicates')):raise ValueError('relay counters unavailable')
            offers=v['received_datagrams'];random=v['random_drops'];eligible=offers-v['queue_drops']-v['blackout_drops']
            residual=offers+v.get('duplicates',0)-sum(v.get(k,0) for k in ('random_drops','queue_drops','blackout_drops','delivered_datagrams','socket_send_errors'))
            if eligible<0 or not 0<=random<=eligible or residual<0:raise ValueError('relay accounting inconsistent')
            if loss==0 and random:raise ValueError('zero-loss control has random drops')
            pending+=residual;p99=v.get('lateness_ms',{}).get('p99')
            timing=bool(timing and finite(p99) and 0<=p99<=max(.5,manifest['profile'][direction]['one_way_ms']*.1) and v.get('socket_send_errors')==0)
            row['network'][direction]=dict(offered=offers,random_drops=random,random_candidates=eligible,
                actual_loss=random/eligible if eligible else None,queue_drops=v['queue_drops'],blackout_drops=v['blackout_drops'],
                delivered=v['delivered_datagrams'],socket_errors=v.get('socket_send_errors'),pending=residual,dispatch_p99_ms=p99)
        if pending!=shape.get('pending_on_close',0):raise ValueError('pending relay copy count differs')
        integrity=summary.get('integrity_verified') is True and all(e.get('completed') is True and not e.get('incomplete_record_bytes') and e.get('counters',{}).get('unexpected_order')==0 for e in endpoints)
        for sent,received in (endpoints,endpoints[::-1]):
            sent_count=sent.get('counters',{}).get('sent_bytes');other=received.get('counters',{}).get('received_bytes')
            hashes=(sent.get('sent_sha256'),received.get('received_sha256'))
            integrity=bool(integrity and count(sent_count) and sent_count==other and all(isinstance(h,str) and re.fullmatch('[0-9a-f]{64}',h) for h in hashes) and hashes[0]==hashes[1])
        health=True;flight_known=True;terminal=[];native_failures=[];deadlines=[]
        for index,e in enumerate(endpoints):
            native=e.get('native_stats') or {};flags_known=all(isinstance(native.get(k),int) and not isinstance(native.get(k),bool) for k in ('send_failed','receive_failed'))
            unhealthy=bool(flags_known and (native['send_failed'] or native['receive_failed']))
            native_failures.append(unhealthy if flags_known else None)
            deadlines.append(e.get('benchmark_deadline_reached'))
            if not flags_known:row['qualification_notes'].append(f'endpoint{index}: native failure flags unavailable')
            if not isinstance(e.get('benchmark_deadline_reached'),bool):row['qualification_notes'].append(f'endpoint{index}: deadline receipt unavailable')
            health=bool(health and flags_known and not unhealthy and not e.get('error') and not e.get('benchmark_deadline_reached') and not e.get('telemetry_errors') and not e.get('cleanup_errors'))
            health=bool(health and isinstance(e.get('benchmark_deadline_reached'),bool) and e.get('counters',{}).get('probe_send_failures')==0)
            cfg=e.get('flight_config');fields=flight_fields(cfg)
            wanted=window if index==0 else 64
            if not fields:
                flight_known=False;row['qualification_notes'].append(f'endpoint{index}: flight configuration unavailable')
            elif fields.get('abi_version')!=1 or fields.get('configured_max_inflight_packets')!=wanted or fields.get('queue_limit_bytes')!=131072 or fields.get('own_log_window')!=7:
                flight_known=False;row['qualification_notes'].append(f'endpoint{index}: requested flight/byte configuration not established')
            if index==0:
                row['flight_config']=cfg;row['peak_inflight']=fields.get('peak_inflight_packets');row['peak_span']=fields.get('peak_active_span_packets')
                row['measurement_peak_inflight']=fields.get('measurement_peak_inflight_packets') if fields.get('measurement_peak_valid')==1 else None
                row['wire_max_payload']=(e.get('queue_stats') or {}).get('fields',{}).get('max_payload_bytes')
                if not count(row['wire_max_payload']) or not 0<row['wire_max_payload']<=1232:raise ValueError('native DATA payload ceiling unavailable/invalid')
                row['minimum_data_chunks_per_record']=math.ceil((plan['suite']['payload_bytes']+36)/row['wire_max_payload'])
            else:row['peer_flight_config']=cfg
            t=e.get('terminal') or {}
            if finite(t.get('since_trial_start_ms')):terminal.append(t['since_trial_start_ms'])
            row['endpoint_receipts'].append({k:e.get(k) for k in ('role','completed','error','terminal','native_stats','benchmark_deadline_reached','measurement_observed_s','incomplete_record_bytes','telemetry_errors','cleanup_errors')})
        row['native_failure']=True if True in native_failures else False if all(v is False for v in native_failures) else None
        row['deadline']=True if True in deadlines else False if all(v is False for v in deadlines) else None
        client=endpoints[0];counts=client.get('counters',{})
        row['probes']={key:(client.get(source) if key in ('timeouts','unobserved') else counts.get(source)) for key,source in dict(expected='probes_expected',injected='probes_injected',observed='probes_observed',missed='probes_not_injected',unobserved='probe_unobserved',timeouts='probe_timeouts',send_failures='probe_send_failures').items()}
        accounting_known=all(isinstance(x,int) and not isinstance(x,bool) and x>=0 for x in row['probes'].values())
        if accounting_known and (row['probes']['expected']!=row['probes']['injected']+row['probes']['missed'] or row['probes']['observed']>row['probes']['injected']):raise ValueError('probe counters inconsistent')
        if accounting_known and row['probes']['unobserved']!=row['probes']['injected']-row['probes']['observed']:raise ValueError('unobserved probes differ from injected-minus-observed')
        if not accounting_known:row['qualification_notes'].append('probe accounting unavailable')
        failures=summary.get('sut_failures') or [];only_probes=bool(summary.get('sut_outcome')=='FAIL' and failures and all(x=='probe deadlines unserved/unobserved' for x in failures))
        qualified=bool(summary['sut_outcome']=='COMPLETE' and not failures and integrity and timing and health and flight_known and accounting_known and row['probes']['expected']==row['probes']['observed'] and row['probes']['timeouts']==0 and row['probes']['send_failures']==0)
        payload_valid=bool(integrity and timing and health and flight_known and accounting_known and (qualified or only_probes))
        metrics=summary.get('metrics') or {};goodput=metrics.get('c2s.verified_goodput_mbps') if integrity else None
        if integrity:
            calculated=endpoints[1]['counters']['measured_bytes']*8/plan['suite']['measure_s']/1e6
            if not finite(goodput) or not math.isclose(calculated,goodput,rel_tol=1e-12,abs_tol=1e-12):raise ValueError('verified rate differs from byte counter')
        for key,kind,q in (('rtt_p95_ms','probe_rtt_ms','p95'),('rtt_p99_ms','probe_rtt_ms','p99'),('scheduled_p99_ms','probe_scheduled_ms','p99')):
            value=metrics.get('client.'+kind+'.'+q)
            if value!=(client.get(kind) or {}).get(q):raise ValueError('latency differs from endpoint record')
            row[key]=value if finite(value) else None
        observed=endpoints[1].get('measurement_observed_s')
        prefix=endpoints[1].get('goodput_mbps')
        row.update(outcome=summary['sut_outcome'],validity=summary['validity'],integrity_verified=integrity,
            qualified=qualified,integrity_valid=payload_valid,timing_valid=timing,goodput_mbps=goodput,
            partial_crc_payload_mbps=prefix if finite(observed) and observed>0 and finite(prefix) else None,
            missed=row['probes']['missed'],timeouts=row['probes']['timeouts'],unobserved=row['probes']['unobserved'],
            first_failure_ms=min(terminal) if terminal else None,measurement_observed_s=client.get('measurement_observed_s'),
            early_exit=any(not e.get('completed') for e in endpoints),observed_measurement_responses=(client.get('probe_rtt_ms') or {}).get('count'),
            reasons=list(summary.get('reasons') or [])+failures+row['qualification_notes'],
            goodput_scope='COMPLETE-qualified' if qualified else 'full-integrity timing-qualified probe-failure diagnostic' if payload_valid else 'full-integrity timing-unqualified diagnostic' if integrity else 'unavailable; partial CRC-valid prefix is separate')
    except (OSError,ValueError,KeyError,TypeError,AttributeError) as error:
        row.update(validity='INVALID_ARTIFACT',artifact_errors=[str(error)],reasons=row['reasons']+[str(error)],integrity_verified=False,
                   qualified=False,integrity_valid=False,goodput_mbps=None,partial_crc_payload_mbps=None,
                   rtt_p95_ms=None,rtt_p99_ms=None,scheduled_p99_ms=None,peak_inflight=None,peak_span=None,measurement_peak_inflight=None)
    return row


def dashboard(plan,rows,status='preparing',network_checks=None):
    groups=[]
    for loss in LOSSES:
        for window in WINDOWS:
            cell=[r for r in rows if r['loss_percent']==loss and r['window_packets']==window]
            intact=[r for r in cell if not r['artifact_errors']]
            subsets={'qualified':[r for r in intact if r['qualified']],
                     'integrity_valid':[r for r in intact if r['integrity_valid']],
                     'diagnostic':intact}
            actual={}
            for direction in ('c2s','s2c'):
                offered=known_sum(r['network'].get(direction,{}).get('random_candidates') for r in intact)
                drops=known_sum(r['network'].get(direction,{}).get('random_drops') for r in intact)
                actual[direction]=drops/offered if offered is not None and offered>0 and drops is not None else None
            groups.append(dict(loss_percent=loss,window_packets=window,planned_count=len(plan['suite']['seeds']),run_count=len(cell),
                qualified_count=len(subsets['qualified']),integrity_count=sum(r['integrity_verified'] for r in intact),
                fail_count=sum(r['outcome']=='FAIL' for r in cell),harness_invalid_count=sum(r['validity']=='INVALID_HARNESS' for r in cell),
                artifact_invalid_count=sum(bool(r['artifact_errors']) for r in cell),native_failure_count=sum(r['native_failure'] is True for r in intact),
                native_failure_unknown_count=sum(r['native_failure'] is None for r in intact),
                deadline_count=sum(r['deadline'] is True for r in intact),deadline_unknown_count=sum(r['deadline'] is None for r in intact),
                metrics={name:{metric:stats(r[metric] for r in selected) for metric in METRICS} for name,selected in subsets.items()},
                probes={key:known_sum(r['probes'][key] for r in intact) for key in ('expected','injected','observed','missed','unobserved','timeouts')},
                actual_loss=actual,peak_inflight=max((r['peak_inflight'] for r in intact if r['peak_inflight'] is not None),default=None),
                peak_span=max((r['peak_span'] for r in intact if r['peak_span'] is not None),default=None),
                queue_drops={d:known_sum(r['network'].get(d,{}).get('queue_drops') for r in intact) for d in ('c2s','s2c')},
                socket_errors={d:known_sum(r['network'].get(d,{}).get('socket_errors') for r in intact) for d in ('c2s','s2c')},
                pending={d:known_sum(r['network'].get(d,{}).get('pending') for r in intact) for d in ('c2s','s2c')},
                first_failure_ms=stats(r['first_failure_ms'] for r in intact),
                incomplete_count=sum(r['outcome'] not in ('COMPLETE','FAIL') for r in cell)))
    meta=dict(status=status,generated_utc=datetime.now(timezone.utc).isoformat(),commit=plan.get('commit'),planned_trials=80,
        seeds=plan['suite']['seeds'],loss_percent=list(LOSSES),window_packets=list(WINDOWS),
        network=dict(rate_mbps=10,one_way_ms=15,jitter_ms=1,queue_ms=30),byte_queue=131072,wire_mtu=1232,scope='native-udp2',
        record_payload_bytes=16384,record_header_bytes=36,parameter='configured active reliable DATA packet limit',
        peak_scope='whole endpoint lifetime including warmup/drain; span is distinct from active packet count',
        actual_loss_scope='whole trial random drops / offers eligible after queue/blackout drops, independently per direction; includes ACKs/retries',
        partial_crc_payload_scope='CRC-valid complete-record prefix payload / configured measurement duration; excludes incomplete records and is not whole-stream verification',
        qualification='Provisional; no accepted calibration receipt. Latency is observed transport probe RTT, not desktop latency.',
        metric_subsets={'qualified':'COMPLETE, full CRC/hash/order integrity, valid timing/endpoint/flight evidence',
                        'integrity_valid':'full integrity plus valid timing/health; probe-only FAIL remains a failure',
                        'diagnostic':'all intact observations; timing-invalid full-integrity rates are diagnostics; partial CRC-prefix rate is separate'})
    return dict(meta=meta,groups=groups,trials=rows,network_checks=network_checks or dict(status='pending',samples=[],summary='Independent raw UDP CRC checks have not been supplied'))


def collect(root,status=None):
    root=Path(root);plan=object_at(root/'flight-plan.json');campaign=root/'main/campaign.json'
    receipt=object_at(campaign) if campaign.exists() else {}
    declarations=list(receipt.get('trials',[]))
    attempts=receipt.get('attempts',[])
    for declaration in [*declarations,*attempts]:
        if not isinstance(declaration,dict) or not isinstance(declaration.get('trial_id'),str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*',declaration['trial_id']):
            raise ValueError('unsafe trial identity')
    if len(attempts)!=len({a['trial_id'] for a in attempts}):raise ValueError('duplicate attempt identity')
    declared_ids={r.get('trial_id') for r in declarations}
    # A killed controller cannot run its finally block. Its write-before-launch
    # attempt receipt still makes an unfinished condition visible on re-audit.
    for attempt in attempts:
        if attempt.get('trial_id') in declared_ids:continue
        path=root/'main'/attempt['trial_id']/'summary.json'
        declarations.append(object_at(path) if path.exists() else dict(attempt,sut_outcome='UNKNOWN',validity='INCOMPLETE_ARTIFACT'))
        declared_ids.add(attempt['trial_id'])
    ids=[r.get('trial_id') for r in declarations]
    if len(ids)!=len(set(ids)) or any(not isinstance(i,str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*',i) for i in ids):raise ValueError('duplicate or unsafe trial identity')
    rows=[inspect_trial(root/'main'/r['trial_id'],r,plan) for r in declarations]
    keys=[(r['loss_percent'],r['window_packets'],r['seed']) for r in rows]
    if len(keys)!=len(set(keys)):raise ValueError('duplicate matrix condition')
    expected={(l,w,s) for l in LOSSES for w in WINDOWS for s in plan['suite']['seeds']}
    if not set(keys)<=expected:raise ValueError('unexpected matrix condition')
    checks=object_at(root/'network-checks.json') if (root/'network-checks.json').exists() else None
    final=status or ('complete' if set(keys)==expected else 'running' if rows else 'preparing')
    result=dashboard(plan,rows,final,checks)
    result['meta']['interrupted']=receipt.get('interrupted') is True
    result.update(schema='freerdp.flight_matrix',version=1,plan=plan,
        condition_coverage=dict(planned=len(expected),attempted=len(rows),unrun=[dict(loss_percent=l,window_packets=w,seed=s) for l,w,s in sorted(expected-set(keys))]))
    return result


def export(result,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    write_json(output/'dashboard.json',result)
    with (output/'trials.csv').open('w',newline='',encoding='utf-8') as stream:
        names=['trial_id','loss_percent','window_packets','seed','outcome','validity','integrity_verified','qualified','integrity_valid',*METRICS,'missed','timeouts','unobserved','peak_inflight','peak_span','measurement_peak_inflight','native_failure','deadline','first_failure_ms','measurement_observed_s','goodput_scope','reasons']
        writer=csv.DictWriter(stream,fieldnames=names);writer.writeheader()
        for row in result['trials']:writer.writerow({key:('; '.join(row['reasons']) if key=='reasons' else row.get(key)) for key in names})
    lines=['# Native UDP2 loss and inflight-count matrix','',result['meta']['qualification'],'',
        '| Loss | DATA limit | Runs / planned | Qualified | Full integrity | Failures | Goodput Mbit/s | RTT p95 / p99 ms | Missed / timeouts |',
        '|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
    show=lambda value:'unavailable' if value is None else f'{value:.3f}'
    for g in result['groups']:
        m=g['metrics']['qualified'];p=g['probes'];lines.append(f"| {g['loss_percent']}% | {g['window_packets']} | {g['run_count']}/{g['planned_count']} | {g['qualified_count']} | {g['integrity_count']} | {g['fail_count']} | {show(m['goodput_mbps']['median'])} | {show(m['rtt_p95_ms']['median'])} / {show(m['rtt_p99_ms']['median'])} | {p['missed']} / {p['timeouts']} |")
    lines += ['', '16/32/48/64 are active DATA packet limits. Byte queues stay 128KiB, records 16KiB, and wire MTU 1232. All failures remain in JSON/CSV. Partial-prefix payload rates and survivor tails are diagnostic; they do not establish complete verified goodput.', '']
    (output/'report.md').write_text('\n'.join(lines),encoding='utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--root',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();result=collect(args.root);export(result,args.output)
    print(dict(status=result['meta']['status'],trials=len(result['trials']),qualified=sum(r['qualified'] for r in result['trials'])))


if __name__=='__main__':main()
