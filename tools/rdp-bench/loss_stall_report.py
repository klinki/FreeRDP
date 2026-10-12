#!/usr/bin/env python3
"""Audit loss campaigns, preserve failures and attribute traced admission stalls."""
from __future__ import annotations
import argparse,bisect,csv,json,statistics
from collections import Counter,defaultdict
from pathlib import Path
from baseline_report import aggregate_campaign,trial_counts
from queue_report import enrich,metric_names,pair,summarize,SUBSETS,eligible,read as safe_read
from rdpbench.common import read_json,rows as json_rows,write_json

METRICS=('c2s.verified_goodput_mbps','client.probe_rtt_ms.p95','client.probe_rtt_ms.p99',
         'client.probe_scheduled_ms.p95','client.probe_scheduled_ms.p99',
         'client.send_call_ms.max','client.queue_blocked_admission_ms.max',
         'client.probes_not_injected','client.probe_timeouts',
         'shaper.c2s.lateness_ms.p99','shaper.s2c.lateness_ms.p99')

def ledger(folder,name):
    if not isinstance(name,str) or Path(name).name!=name:raise ValueError('unsafe trace ledger path')
    return list(json_rows(folder/name))

def native_recoveries(native,relay):
    """Match chronological visible sequence evidence; unknown drops stay unknown."""
    sends={};gaps={};drops=defaultdict(list);retirements=defaultdict(list);result=[]
    for item in relay:
        if item.get('type')=='drop' and item.get('direction')=='c2s' and 'data_seq' in item:
            drops[(item['data_seq'],item.get('channel_seq'))].append(item)
    for event in native:
        if event.get('event_name')=='retire':retirements[event.get('channel_seq')].append(event['time_ns'])
    for event in native:
        kind=event.get('event_name');key=(event.get('data_seq'),event.get('channel_seq'))
        if kind=='data_send':sends[key]=event;gaps.pop(key,None)
        elif kind=='nack_gap':
            evidence=gaps.setdefault(key,{'first':event,'ready':None})
            if event.get('detail',0)>=3 and evidence['ready'] is None:evidence['ready']=event
        elif kind in ('retry_timer','retry_fast'):
            sent=sends.get(key);evidence=gaps.get(key);gap=evidence['first'] if evidence else None
            ready=evidence['ready'] if evidence else None;when=event['time_ns']
            drop=next((r for r in reversed(drops[key]) if sent and sent['time_ns']-1_000_000<=r['offered_ns']<=when),None)
            retires=retirements[event.get('channel_seq')];index=bisect.bisect_left(retires,when)
            retired=retires[index] if index<len(retires) else None
            result.append({'kind':kind,'time_ns':when,'data_seq':key[0],'channel_seq':key[1],
                           'native_attempt_age_ms':event.get('detail'),
                           'gap_ns':gap['time_ns'] if gap else None,
                           'gap_to_retry_ms':(when-gap['time_ns'])/1e6 if gap else None,
                           'ready_feedback_ns':ready['time_ns'] if ready else None,
                           'ready_feedback_to_retry_ms':(when-ready['time_ns'])/1e6 if ready else None,
                           'native_pump_events_after_feedback':sum(e.get('event_name')=='pump_enter' and ready['time_ns']<=e['time_ns']<=when for e in native) if ready else None,
                           'attempt_to_retry_ms':(when-sent['time_ns'])/1e6 if sent else None,
                           'native_send_to_relay_drop_offer_ms':(drop['offered_ns']-sent['time_ns'])/1e6 if drop and sent else None,
                           'relay_drop_reason':drop.get('reason') if drop else None,
                           'relay_drop_ns':drop.get('offered_ns') if drop else None,
                           'retired_ns':retired,'retry_to_retire_ms':(retired-when)/1e6 if retired else None})
    return result

def attribute(calls,native,relay,tolerance_ms=1.5):
    """Report observed native timer waits and overlapping relay confounds separately."""
    recoveries=native_recoveries(native,relay);stalls=[];misses=[]
    native_times=[event['time_ns'] for event in native]
    dispatches=[r for r in relay if r.get('type')=='dispatch']
    for call in calls:
        if call.get('type')!='native_call' or call.get('duration_ns',0)<100_000_000:continue
        start,end=call['start_ns'],call['end_ns']
        inside=native[bisect.bisect_left(native_times,start):bisect.bisect_right(native_times,end)]
        due=[r for r in dispatches if r['due_ns']<=end and r['actual_dispatch_ns']>=start]
        maximum=max((r.get('lateness_ns',0) for r in due),default=None)
        local=[r for r in recoveries if start<=r['time_ns']<=end]
        blocked=any(e.get('event_name')=='enqueue_block' for e in inside)
        known_loss=[r for r in local if r['kind']=='retry_timer' and r['ready_feedback_ns'] is not None
                    and r['relay_drop_reason'] is not None]
        known_timer=[r for r in local if r['kind']=='retry_timer' and r['ready_feedback_ns'] is not None]
        if maximum is not None and maximum>tolerance_ms*1e6:classification='relay_timing_confounded'
        elif maximum is None:classification='insufficient_relay_evidence'
        elif blocked and known_loss:classification='native_admission_timer_recovery'
        elif blocked and known_timer:classification='native_admission_timer_wait_unattributed'
        elif blocked:classification='native_admission_wait'
        else:classification='unexplained_native_call'
        stalls.append(dict(call,classification=classification,relay_dispatch_count=len(due),
                           relay_max_lateness_ms=maximum/1e6 if maximum is not None else None,
                           native_timer_recovery_evidence=known_loss if blocked else [],
                           relay_lateness_confound=maximum is not None and maximum>tolerance_ms*1e6,
                           unknown_intervals=['fixed peer ACK generation to relay offer; peer native ABI unavailable',
                                              'source socket send to relay offer except matched DATA drop',
                                              'host scheduling outside recorded calls and relay dispatches'],
                           native_event_counts=dict(Counter(e.get('event_name') for e in inside)),
                           recoveries=local))
    for item in calls:
        if item.get('type')!='probe_not_injected':continue
        deadline=item['scheduled_ns'];observed=item.get('observed_ns',deadline)
        involved=[s for s in stalls if s['start_ns']<observed and s['end_ns']>deadline]
        if any(s['classification']=='relay_timing_confounded' for s in involved):cause='relay_timing_confounded'
        elif any(s['classification']=='native_admission_timer_recovery' for s in involved):cause='native_admission_timer_recovery'
        elif any(s['classification']=='native_admission_wait' for s in involved):cause='native_admission_wait'
        elif any(r.get('type')=='owner_loop_gap' and r['start_ns']<observed and r['end_ns']>deadline for r in calls):cause='owner_loop_or_host_schedule_gap'
        else:cause='insufficient_evidence'
        misses.append(dict(item,classification=cause,overlapping_call_spans=[{'start_ns':s['start_ns'],'end_ns':s['end_ns'],
                          'call_kind':s.get('call_kind'),'record_id':s.get('record_id')} for s in involved]))
    return {'stall_threshold_ms':100,'relay_tolerance_ms':tolerance_ms,'stalls':stalls,'misses':misses,
            'recoveries':recoveries,'stall_classes':dict(Counter(s['classification'] for s in stalls)),
            'miss_classes':dict(Counter(m['classification'] for m in misses)),
            'recovery_counts':dict(Counter(r['kind'] for r in recoveries))}

def trace_trial(row):
    if row['validity']=='INVALID_ARTIFACT':
        return {'enabled':row.get('invariants',{}).get('case',{}).get('loss_trace',False),
                'available':False,'valid':False,'reasons':['sealed artifacts changed/missing; trace withheld']}
    folder=Path(row['artifact_directory']);endpoint=safe_read(folder/'endpoint-0.result.json')
    trace=endpoint.get('loss_trace');shape=safe_read(folder/'shaper.json');relay_meta=shape.get('loss_trace')
    if not trace:return {'enabled':False,'available':False,'valid':None,'reasons':['loss trace disabled; no causal attribution']}
    native=trace.get('native',{});reasons=[]
    if not trace.get('valid'):reasons.append('application trace overflow or invalid')
    if not native.get('valid'):reasons.extend(native.get('reasons',['native trace invalid/unavailable']))
    if not relay_meta or not relay_meta.get('valid'):reasons.append('relay trace invalid/unavailable')
    try:
        calls=ledger(folder,trace.get('call_ledger'));events=ledger(folder,trace.get('native_ledger'))
        relay=ledger(folder,(relay_meta or {}).get('ledger'))
        if len(calls)!=trace.get('count'):reasons.append('application ledger count mismatch')
        if len(events)!=(native.get('metadata') or {}).get('count'):reasons.append('native ledger count mismatch')
        if len(relay)!=(relay_meta or {}).get('count'):reasons.append('relay ledger count mismatch')
        missed=[r for r in calls if r.get('type')=='probe_not_injected']
        if len(missed)!=endpoint.get('counters',{}).get('probes_not_injected'):reasons.append('missed-probe trace count mismatch')
        for call in calls:
            if call.get('type')=='native_call':
                start,end=call.get('start_ns'),call.get('end_ns')
                if not isinstance(start,int) or not isinstance(end,int) or not 0<=start<=end or call.get('duration_ns')!=end-start:
                    reasons.append('invalid native call span');break
        if any(not isinstance(e.get('time_ns'),int) for e in events):reasons.append('native timestamps unavailable')
        elif any(a['time_ns']>b['time_ns'] for a,b in zip(events,events[1:])):reasons.append('native timestamps moved backwards')
        offers=sum(r.get('type')=='offer' for r in relay);dispatches=sum(r.get('type')=='dispatch' for r in relay)
        if offers!=sum(shape[d].get('received_datagrams',0) for d in ('c2s','s2c')):reasons.append('relay offer count mismatch')
        if dispatches!=sum(shape[d].get('delivered_datagrams',0) for d in ('c2s','s2c')):reasons.append('relay dispatch count mismatch')
        seen_offers={};copies=set();counts=Counter()
        for event in relay:
            kind=event.get('type');direction=event.get('direction');key=(direction,event.get('packet_id'))
            if direction not in ('c2s','s2c'):reasons.append('invalid relay direction');break
            if kind=='offer':
                if key in seen_offers:reasons.append('duplicate relay offer identity');break
                seen_offers[key]=event
            elif kind in ('drop','dispatch'):
                offer=seen_offers.get(key)
                if not offer or any(event.get(field)!=offer.get(field) for field in ('offered_ns','data_seq','channel_seq','kind','udp_bytes')):
                    reasons.append('relay event differs from its offered packet');break
                if kind=='drop':
                    if offer.get('scheduled_copies')!=0:reasons.append('dropped relay offer has scheduled copies');break
                    counts[(direction,event.get('reason'))]+=1
                else:
                    serial=event.get('copy_id');due=event.get('due_ns');actual=event.get('actual_dispatch_ns')
                    if serial in copies or not isinstance(serial,int) or not isinstance(due,int) or not isinstance(actual,int) or event.get('lateness_ns')!=max(0,actual-due):
                        reasons.append('invalid relay dispatch identity/deadline');break
                    copies.add(serial)
        for direction in ('c2s','s2c'):
            for reason in ('random_drops','queue_drops','blackout_drops'):
                if counts[(direction,reason)]!=shape[direction].get(reason,0):reasons.append('relay drop reason count mismatch')
        scheduled=sum(e.get('scheduled_copies',0) for e in seen_offers.values())
        send_errors=sum(shape[d].get('socket_send_errors',0) for d in ('c2s','s2c'))
        if scheduled!=dispatches+send_errors+shape.get('pending_on_close',0):reasons.append('relay scheduled copy accounting mismatch')
    except (OSError,ValueError,KeyError,TypeError) as error:
        reasons.append(str(error));calls=[];events=[];relay=[]
    summary={'enabled':True,'available':native.get('available',False),'valid':not reasons,'reasons':reasons,
             'native_clock_alignment':native.get('clock_alignment'),'native_metadata':native.get('metadata'),
             'native_event_counts':dict(Counter(e.get('event_name') for e in events)),
             'fixed_peer_native_trace':safe_read(folder/'endpoint-1.result.json').get('loss_trace',{}).get('native',{}),
             'call_ledger':str(folder/trace['call_ledger']) if isinstance(trace.get('call_ledger'),str) else None,
             'native_ledger':str(folder/trace['native_ledger']) if isinstance(trace.get('native_ledger'),str) else None,
             'relay_ledger':str(folder/relay_meta['ledger']) if relay_meta and isinstance(relay_meta.get('ledger'),str) else None}
    if not reasons:summary['attribution']=attribute(calls,events,relay)
    return summary

def apply_trace_eligibility(row):
    """A requested diagnostic with unaudited trace evidence cannot form a strict pair."""
    trace=row.get('loss_trace',{})
    requested=row.get('invariants',{}).get('case',{}).get('loss_trace',False)
    if (requested or trace.get('enabled')) and trace.get('valid') is not True:
        row['artifact_intact_timing_valid_complete']=False
        row['validated_lab_complete']=False
        row['trace_audit_reasons']=list(trace.get('reasons') or ['requested loss trace unavailable or invalid'])
    return row

def build(root):
    root=Path(root);plan=read_json(root/'loss-sweep-plan.json');all_rows=[];phases=[];comparisons=[];unpaired=[];groups=[]
    expected={phase:len(c['cases'])*len(c['seeds'])*len(plan['variants'][phase]) for phase,c in plan['configurations'].items()}
    for phase in plan['configurations']:
        campaign=root/phase/'campaign.json'
        if not campaign.is_file():
            phases.append({'phase':phase,'complete':False,'expected_trials':expected[phase],'observed_trials':0});continue
        report=aggregate_campaign(root/phase);rows=[dict(enrich(r),phase=phase) for r in report['trials']]
        for row in rows:
            row['loss_trace']=trace_trial(row)
            apply_trace_eligibility(row)
            if row['validity']=='INVALID_ARTIFACT':row['metrics']={name:None for name in row['metrics']}
        all_rows.extend(rows);phases.append(dict(trial_counts(rows,expected[phase]),phase=phase,
                                                complete=len(rows)==expected[phase]))
        names=metric_names(rows)
        for (case,variant,key),members in _groups(rows).items():
            groups.append({'phase':phase,'case_id':case,'variant':variant,'comparison_key':key,
                           'subsets':{subset:summarize([r for r in members if eligible(r,subset)],names) for subset in SUBSETS}})
        pairs=[(f'reference-q{cap}',f'candidate-q{cap}') for cap in (48,64)] if phase=='main' else [(f'timer-q{cap}',f'fast-q{cap}') for cap in (48,64)] if phase.startswith('diagnostic') or phase=='smoke' else []
        for baseline,candidate in pairs:
            c,u=pair(rows,baseline,candidate,names)
            comparisons.extend(dict(item,phase=phase) for item in c);unpaired.extend(dict(item,phase=phase) for item in u)
    return {'version':1,'scope':'raw native UDP2 transport; trace diagnostics separate from untraced comparison',
            'provenance':plan,'phases':phases,'groups':groups,'comparisons':comparisons,'unpaired':unpaired,
            'trials':all_rows,'notes':['All failures remain visible. Strict pairs require both timing-valid COMPLETE artifact-intact trials.',
                                     'Per-trial percentiles are summarized by medians; no percentile samples are pooled.',
                                     'Trace-disabled trials and old fixed peers have unknown causal evidence, not zero native events.',
                                     'Misses include warmup; p95/p99 probe samples use measurement-scheduled probes.',
                                     'Native timer recovery evidence requires admission blocking, a matched C→S DATA drop, ready gap feedback and timer retry; relay intervals overlapping that call are checked separately.',
                                     'This establishes an observed native wait after known feedback, not exclusive causation: old-peer ACK generation and unmeasured host/socket intervals remain unknown.',
                                     'Trace-on/off overhead is descriptive; no independent calibration receipt is implied.']}

def _groups(rows):
    result=defaultdict(list)
    for row in rows:result[(row['case_id'],row['variant'],row['comparison_key'])].append(row)
    return result

def write(report,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=True);write_json(output/'aggregate.json',report)
    with (output/'trials.csv').open('w',newline='',encoding='utf-8') as stream:
        fields=['phase','trial_id','case_id','variant','seed','validity','sut_outcome','integrity_verified']+list(METRICS)+['trace_valid','trace_stall_classes','trace_miss_classes']
        writer=csv.DictWriter(stream,fieldnames=fields,lineterminator='\n');writer.writeheader()
        for row in report['trials']:
            values={name:row.get(name) for name in fields[:8]};values.update({name:row['metrics'].get(name) for name in METRICS})
            trace=row['loss_trace'];attribution=trace.get('attribution',{})
            values.update(trace_valid=trace.get('valid'),trace_stall_classes=json.dumps(attribution.get('stall_classes',{})),
                          trace_miss_classes=json.dumps(attribution.get('miss_classes',{})))
            writer.writerow(values)
    lines=['# Loss-stall campaign','',report['scope'],'','| Phase | Trials | Integrity passed | Service failures | Timing invalid | Artifact invalid |',
           '|---|---:|---:|---:|---:|---:|']
    for phase in report['phases']:
        values=[r for r in report['trials'] if r['phase']==phase['phase']]
        lines.append(f"| {phase['phase']} | {len(values)}/{phase['expected_trials']} | {sum(r['integrity_verified'] is True for r in values)} | {sum(r['sut_outcome']=='FAIL' for r in values)} | {sum(r['original_validity']=='INVALID_HARNESS' for r in values)} | {sum(r['validity']=='INVALID_ARTIFACT' for r in values)} |")
    lines+=['','## Matched main results','','| Profile | Cap | Metric | Paired seeds | Reference | Candidate |',
            '|---|---|---|---|---:|---:|']
    for c in report['comparisons']:
        if c['phase']=='main' and c['subset']=='artifact_intact_timing_valid_complete' and c['metric'] in METRICS:
            def fmt(v):return '—' if v is None else f'{v:.3f}'
            lines.append(f"| {c['case_id']} | {c['candidate_variant']} | {c['metric']} | {','.join(str(p['seed']) for p in c['pairs'])} | {fmt(c['baseline']['median'])} | {fmt(c['candidate']['median'])} |")
    lines+=['','## Trace attribution','']
    for r in report['trials']:
        trace=r['loss_trace']
        if not trace.get('enabled'):continue
        if not trace.get('valid'):lines.append(f"- {r['trial_id']}: trace invalid: {'; '.join(trace.get('reasons',[]))}");continue
        a=trace['attribution'];lines.append(f"- {r['trial_id']}: stalls {json.dumps(a['stall_classes'])}; misses {json.dumps(a['miss_classes'])}; retries {json.dumps(a['recovery_counts'])}.")
    lines+=['','## Failures retained','']
    for r in report['trials']:
        if r['sut_outcome']!='COMPLETE' or r['original_validity']=='INVALID_HARNESS' or r['validity']=='INVALID_ARTIFACT':
            lines.append(f"- {r['trial_id']}: {r['validity']} / {r['sut_outcome']}; {'; '.join(r.get('reasons',[])+r.get('sut_failures',[]))}")
    lines+=['','## Limits','']+['- '+note for note in report['notes']]
    (output/'report.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--root',required=True,type=Path);parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();root=args.root.expanduser().resolve();output=args.output.expanduser().resolve()
    # Never modify source, binaries, existing sealed trials or campaign metadata.
    plan=read_json(root/'loss-sweep-plan.json')
    protected=[root/phase for phase in plan['configurations']]
    protected += [Path(plan[key]).parent for key in ('candidate_library','reference_library')]
    if output==root or output==Path(__file__).parent or any(output.is_relative_to(p.resolve()) for p in protected):
        parser.error('report output would overwrite campaign provenance, binary or source inputs')
    write(build(root),output);return 0

if __name__=='__main__':raise SystemExit(main())
