#!/usr/bin/env python3
"""Audit high-loss trials and retain failures beside survivor-only latency."""
from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path

from rdpbench.common import audit, digest, read_json, sha256, write_json
from rdpbench.type1 import endpoint_result


def stats(values):
    values=[x for x in values if isinstance(x,(int,float)) and not isinstance(x,bool) and math.isfinite(x)]
    return {'n':len(values),'median':statistics.median(values) if values else None,
            'min':min(values) if values else None,'max':max(values) if values else None}


def collect(root):
    root=Path(root).resolve()
    campaign=read_json(root/'campaign.json')
    if not isinstance(campaign,dict) or not isinstance(campaign.get('trials'),list) or not campaign['trials']:
        raise ValueError('report requires a nonempty completed campaign')
    if any(not isinstance(row,dict) for row in campaign['trials']):raise ValueError('invalid campaign trial declaration')
    if any(row.get('configured_loss') is not None and not isinstance(row['configured_loss'],dict) for row in campaign['trials']):
        raise ValueError('invalid declared directional loss')
    if len({row.get('variant') for row in campaign['trials']})!=1:
        raise ValueError('loss-stress report requires a single candidate variant')
    trials=[]
    for declaration in campaign['trials']:
        ident=declaration.get('trial_id','')
        if not isinstance(ident,str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*',ident):
            raise ValueError('unsafe trial identity')
        folder=root/ident
        row={'trial_id':ident,'case_id':declaration.get('case_id'),'seed':declaration.get('seed'),
             'variant':declaration.get('variant'),'profile_id':declaration.get('profile_id'),
             'configured_loss':declaration.get('configured_loss',{}),
             'loss_probability':declaration.get('configured_loss',{}).get('c2s'),
             'sut_outcome':'UNKNOWN','validity':'INVALID_ARTIFACT','artifact_errors':[],
             'metrics':{},'endpoints':[],'network':{},'reasons':[],'verified_goodput_mbps':None}
        try:
            errors=audit(folder)
            if errors:raise ValueError('; '.join(errors))
            summary=read_json(folder/'summary.json');manifest=read_json(folder/'manifest.json')
            if any(summary.get(key)!=declaration.get(key) for key in ('trial_id','case_id','seed','variant','binary_sha256')):
                raise ValueError('campaign declaration differs from sealed summary')
            if declaration.get('profile_id') is not None and (declaration['profile_id']!=manifest['profile_id'] or
                    declaration['profile_id']!=summary.get('profile_id')):
                raise ValueError('campaign profile differs from sealed condition')
            if declaration.get('configured_loss') is not None and declaration['configured_loss']!={
                    d:manifest['profile'][d]['loss'] for d in ('c2s','s2c')}:
                raise ValueError('campaign loss differs from sealed condition')
            row.update({key:summary.get(key) for key in ('variant','commit','binary_sha256','validity',
                'sut_outcome','integrity_verified','reasons','sut_failures','endpoint_termination','telemetry_notes')})
            row['profile_id']=manifest['profile_id']
            row['loss_probability']=manifest['profile']['c2s']['loss']
            row['configuration']={key:manifest['invariants'][key] for key in
                ('warmup_s','measure_s','drain_s','payload_bytes','probe_hz','peer_sha256','peer_environment')}
            row['configured_loss']={d:manifest['profile'][d]['loss'] for d in ('c2s','s2c')}
            row['metrics']=summary['metrics']
            row['verified_goodput_mbps']=summary['metrics'].get('c2s.verified_goodput_mbps') if (
                summary.get('integrity_verified') is True and summary.get('sut_outcome')=='COMPLETE' and
                summary.get('validity') in ('PROVISIONAL_UNCALIBRATED','VALIDATED_LAB')) else None
            shape=read_json(folder/'shaper.json')
            pending_by_counters=0
            for direction in ('c2s','s2c'):
                values=shape.get(direction,{})
                offers=values.get('received_datagrams',0);random_drops=values.get('random_drops',0)
                candidates=offers-values.get('queue_drops',0)-values.get('blackout_drops',0)
                delivered=values.get('delivered_datagrams',0)
                row['network'][direction]={'offered_datagrams':offers,'random_drops':random_drops,
                    'random_loss_candidates':candidates,
                    'realized_random_loss':random_drops/candidates if candidates else None,
                    'queue_drops':values.get('queue_drops',0),'blackout_drops':values.get('blackout_drops',0),
                    'delivered_datagrams':delivered,'socket_send_errors':values.get('socket_send_errors',0),
                    'dispatch_p99_ms':values.get('lateness_ms',{}).get('p99')}
                if candidates<0 or random_drops>candidates:
                    raise ValueError('impossible relay random-loss accounting: '+direction)
                residual=offers+values.get('duplicates',0)-sum(values.get(key,0) for key in
                    ('random_drops','queue_drops','blackout_drops','delivered_datagrams','socket_send_errors'))
                if residual<0:raise ValueError('inconsistent relay packet accounting: '+direction)
                pending_by_counters+=residual
            if pending_by_counters!=shape.get('pending_on_close',0):
                raise ValueError('relay pending-copy accounting mismatch')
            raw_endpoints=[]
            for index in (0,1):
                endpoint=endpoint_result(folder/f'endpoint-{index}.result.json')
                if endpoint.get('result_read_error'):raise ValueError(endpoint['error'])
                raw_endpoints.append(endpoint)
                row['endpoints'].append({key:endpoint.get(key) for key in ('role','completed','error','terminal',
                    'counters','native_stats','probe_timeouts','probes_pending_at_exit','probe_unobserved',
                    'probe_rtt_ms','probe_scheduled_ms','measurement_observed_s','latency_scope',
                    'incomplete_record_bytes','telemetry_errors','cleanup_errors','benchmark_deadline_reached')})
                row['endpoints'][-1]['queue_configuration']=(endpoint.get('queue_stats') or {}).get('fields',{})
                row['endpoints'][-1]['queue_configuration']={key:row['endpoints'][-1]['queue_configuration'].get(key)
                    for key in ('queue_limit_bytes','own_log_window','peer_log_window','effective_window_packets')}
            terminals=[e['terminal'].get('since_trial_start_ms') for e in row['endpoints'] if isinstance(e.get('terminal'),dict)]
            terminals=[x for x in terminals if isinstance(x,(int,float)) and not isinstance(x,bool) and math.isfinite(x)]
            row['first_failure_ms']=min(terminals) if terminals else None
            if row.get('integrity_verified') is True:
                for sender,receiver in (raw_endpoints,raw_endpoints[::-1]):
                    if sender.get('completed') is not True or receiver.get('completed') is not True:
                        raise ValueError('verified transfer has incomplete endpoint')
                    sent=sender.get('counters',{}).get('sent_bytes');received=receiver.get('counters',{}).get('received_bytes')
                    hashes=(sender.get('sent_sha256'),receiver.get('received_sha256'))
                    if isinstance(sent,bool) or not isinstance(sent,int) or sent<0 or sent!=received or any(
                            not isinstance(h,str) or not re.fullmatch('[0-9a-f]{64}',h) for h in hashes) or hashes[0]!=hashes[1]:
                        raise ValueError('verified transfer byte/hash mismatch')
                    if receiver.get('incomplete_record_bytes'):
                        raise ValueError('verified transfer has partial application record')
        except (ValueError,OSError,KeyError,TypeError,AttributeError) as error:
            row.update(validity='INVALID_ARTIFACT',artifact_errors=[str(error)],metrics={},
                       verified_goodput_mbps=None,endpoints=[],network={})
        trials.append(row)
    groups=[]
    by_profile=defaultdict(list)
    for row in trials:by_profile[row.get('profile_id') or row.get('case_id','unknown')].append(row)
    for profile,rows in sorted(by_profile.items(),key=lambda pair:pair[1][0].get('loss_probability') if pair[1][0].get('loss_probability') is not None else -1):
        intact=[r for r in rows if not r['artifact_errors']]
        identities={digest({key:r.get(key) for key in ('variant','case_id','binary_sha256','configuration','configured_loss')}) for r in intact}
        if len(identities)>1:raise ValueError('mixed case, binary or configuration within profile '+str(profile))
        qualified=[r for r in intact if r['sut_outcome']=='COMPLETE' and r.get('integrity_verified') is True
                   and r['validity'] in ('PROVISIONAL_UNCALIBRATED','VALIDATED_LAB')]
        endpoints=[r['endpoints'][0] for r in intact if r['endpoints']]
        counters=[e.get('counters') or {} for e in endpoints]
        names=('probes_expected','probes_injected','probes_observed','probes_not_injected','probe_send_failures')
        counts={name:sum(c[name] for c in counters) if counters and all(isinstance(c.get(name),int) for c in counters) else None for name in names}
        counts['unobserved']=sum(e['probe_unobserved'] for e in endpoints) if endpoints and all(isinstance(e.get('probe_unobserved'),int) for e in endpoints) else None
        counts['timeouts']=sum(e['probe_timeouts'] for e in endpoints) if endpoints and all(isinstance(e.get('probe_timeouts'),int) for e in endpoints) else None
        groups.append({'profile_id':profile,'configured_loss_probability':rows[0].get('loss_probability'),
            'trials':len(rows),'outcomes':dict(Counter(r['sut_outcome'] for r in rows)),
            'artifact_invalid':len(rows)-len(intact),'timing_valid_complete':len(qualified),'probes':counts,
            'verified_goodput_mbps':stats(r['verified_goodput_mbps'] for r in qualified),
            'observed_trial_p95_ms':stats(r['metrics'].get('client.probe_rtt_ms.p95') for r in intact),
            'observed_trial_p99_ms':stats(r['metrics'].get('client.probe_rtt_ms.p99') for r in intact),
            'first_failure_ms':stats(r.get('first_failure_ms') for r in intact),
            'native_terminal_failures':sum(any(not e.get('benchmark_deadline_reached') and (e.get('native_stats') or {}).get(k) for e in r['endpoints']
                for k in ('send_failed','receive_failed')) for r in intact),
            'benchmark_deadline_stops':sum(any(e.get('benchmark_deadline_reached') for e in r['endpoints']) for r in intact),
            'realized_random_loss':{d:stats(r['network'][d]['realized_random_loss'] for r in intact) for d in ('c2s','s2c')},
            'unexpected_queue_drops':{d:sum(r['network'][d]['queue_drops'] for r in intact) for d in ('c2s','s2c')}})
    return {'schema':'freerdp.loss_stress','version':1,'scope':'raw native UDP2; independent random loss on both directions',
        'reporter_sha256':sha256(Path(__file__)),
        'source_campaign_sha256':sha256(root/'campaign.json'),'counts':{'trials':len(trials),
        'outcomes':dict(Counter(r['sut_outcome'] for r in trials)),'artifact_invalid':sum(bool(r['artifact_errors']) for r in trials)},
        'groups':groups,'trials':trials,'notes':[
            'No accepted calibration receipt or live Windows performance claim is implied.',
            'Goodput summaries require artifact-intact timing-valid COMPLETE integrity-verified transfers; failed transfers are unavailable.',
            'p95/p99 are medians of per-trial observed-response percentiles; failed trials with responses remain diagnostic. Samples are never pooled.',
            'Latency is survivor-only: compare it with unobserved responses and unserved scheduled injections. Missing observations do not become zero.',
            'Probe counts include warmup; percentile samples include measurement-scheduled probes only.',
            'Early endpoint exit leaves responses censored/unobserved, not fabricated drain-deadline timeouts.',
            'Realized random loss uses eligible offers after queue/blackout drops; ACKs and retries also undergo random loss.',
            'Seeds reproduce packet-index impairment decisions; traffic changes do not preserve identical logical losses.']}


def display(value):
    return 'unavailable' if value is None else f'{value:.3f}'


def export(report,output):
    output=Path(output)
    write_json(output.with_suffix('.json'),report)
    lines=['# High-loss native transport campaign','',report['scope'],'',
        '| Configured loss each direction | Complete / failed | Qualified goodput Mbit/s (N) | Observed p95 / p99 ms (N) | Expected / injected / observed | Unserved / unobserved | First failure ms (N) |',
        '|---|---:|---:|---:|---:|---:|---:|']
    for group in report['groups']:
        probes=group['probes'];goodput=group['verified_goodput_mbps'];p95=group['observed_trial_p95_ms'];p99=group['observed_trial_p99_ms']
        loss=group['configured_loss_probability']
        failure=group['first_failure_ms']
        lines.append(f"| {display(loss*100 if loss is not None else None)}% | {group['outcomes'].get('COMPLETE',0)} / {group['outcomes'].get('FAIL',0)} | {display(goodput['median'])} ({goodput['n']}) | {display(p95['median'])} / {display(p99['median'])} ({p99['n']}) | {probes['probes_expected']} / {probes['probes_injected']} / {probes['probes_observed']} | {probes['probes_not_injected']} / {probes['unobserved']} | {display(failure['median'])} ({failure['n']}) |")
    lines+=['','## Limits','']+['- '+note for note in report['notes']]
    lines+=['','## Retained failures','']
    for row in report['trials']:
        if row['sut_outcome']!='COMPLETE' or row['validity'] in ('INVALID_HARNESS','INVALID_ARTIFACT'):
            reasons=row.get('artifact_errors',[])+(row.get('reasons') or [])+(row.get('sut_failures') or [])
            lines.append(f"- {row['trial_id']}: {row['validity']} / {row['sut_outcome']}: "+'; '.join(reasons))
    output.with_suffix('.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    with output.with_suffix('.csv').open('w',encoding='utf-8',newline='') as stream:
        writer=csv.writer(stream,lineterminator='\n');writer.writerow(['trial_id','profile','seed','outcome','validity','configured_loss',
            'verified_goodput_mbps','observed_p95_ms','observed_p99_ms','expected','injected','observed','unserved',
            'unobserved','timeouts','client_send_failed','client_receive_failed','c2s_random_loss','s2c_random_loss',
            'first_failure_ms','client_measurement_observed_s'])
        for row in report['trials']:
            endpoint=row['endpoints'][0] if row['endpoints'] else {};counter=endpoint.get('counters') or {};native=endpoint.get('native_stats') or {}
            writer.writerow([row['trial_id'],row.get('profile_id'),row['seed'],row['sut_outcome'],row['validity'],
                row.get('loss_probability'),row['verified_goodput_mbps'],row['metrics'].get('client.probe_rtt_ms.p95'),
                row['metrics'].get('client.probe_rtt_ms.p99'),*[counter.get(k) for k in ('probes_expected','probes_injected','probes_observed','probes_not_injected')],
                endpoint.get('probe_unobserved'),endpoint.get('probe_timeouts'),native.get('send_failed'),native.get('receive_failed'),
                *[row['network'].get(d,{}).get('realized_random_loss') for d in ('c2s','s2c')],
                row.get('first_failure_ms'),endpoint.get('measurement_observed_s')])


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--root',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path);args=parser.parse_args()
    root=args.root.resolve();output=args.output.resolve()
    targets=[output.with_suffix(s).resolve() for s in ('.json','.csv','.md')]
    sealed=[path.parent for path in root.rglob('SHA256SUMS.json')]
    if any(any(target.is_relative_to(folder) for folder in sealed) or target==root/'campaign.json' for target in targets):
        parser.error('output must not overwrite campaign inputs or sealed trials')
    try:report=collect(root)
    except (ValueError,OSError,KeyError,TypeError) as error:parser.error(str(error))
    export(report,output)
    print(report['counts'])
    return 1 if report['counts']['artifact_invalid'] else 0


if __name__=='__main__':raise SystemExit(main())
