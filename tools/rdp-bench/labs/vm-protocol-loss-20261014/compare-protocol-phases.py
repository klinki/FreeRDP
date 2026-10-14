#!/usr/bin/env python3
"""Combine closed VM phase reports without pooling trials or inventing metrics.

This is analysis only. It does not contact the VM, Docker or a network endpoint.
Run after every timed campaign has closed. The optional earlier all-IPv4 report
is a separately labelled historical comparison, with different collector code.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

LOSSES = (0, 25, 50, 70)
ENGINES = ('local', 'pr')
LOCAL = '79ee244a4d5b2d5a280e165be4c643478484bf9e'
OLD_PR = 'a306a67c198f4493d8d2000bc10efd75b2432020'
FIXED_PR = '49fcbb72c0b6fbfdb00822f98a9ff3697af6a1ab'
UNKNOWN = ('verified_goodput_mbps', 'physical_client_fps', 'transport_probe_rtt_p95_ms',
           'transport_probe_rtt_p99_ms', 'input_to_display_p95_ms',
           'input_to_display_p99_ms', 'crc_integrity_verified')


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def count(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def read(path):
    value = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict):
        raise ValueError('report must be a JSON object: ' + str(path))
    return value


def phase_trials(report, protocols, commits, *, historical=False):
    if not historical:
        if report.get('trial_kind') != 'performance':
            raise ValueError('diagnostics cannot enter production comparisons')
        if not report.get('campaign_execution', {}).get('finished_utc'):
            raise ValueError('closed campaign receipt required')
        mode = report.get('run_configuration') or {}
        if mode.get('packet_capture_enabled') is not False or mode.get('trace_enabled') is not False:
            raise ValueError('production comparison must have no capture or tracing')
    expected = {(p, e, loss) for p in protocols for e in ENGINES for loss in LOSSES}
    trials = report.get('trials')
    if not isinstance(trials, list) or len(trials) != len(expected):
        raise ValueError('all planned conditions must be retained')
    result = {}
    for trial in trials:
        protocol = trial.get('protocol', 'all' if historical else None)
        key = (protocol, trial.get('engine'), trial.get('loss_percent'))
        if key not in expected or key in result:
            raise ValueError('unexpected or duplicate condition')
        source = trial.get('source_identity') or {}
        if source.get('expected_commit') != commits[key[1]]:
            raise ValueError('unexpected source identity at ' + str(key))
        if any(trial.get(name) is not None for name in UNKNOWN):
            raise ValueError('this VM method does not establish CRC goodput or latency tails')
        if not historical and trial.get('performance_eligible') is not True:
            raise ValueError('production row marked observer diagnostic')
        if (trial.get('capture_status') or {}).get('performance_confounded'):
            raise ValueError('capture-confounded report cannot enter matched comparison')
        result[key] = trial
    if set(result) != expected:
        raise ValueError('condition set mismatch')
    return result


def qualified_source(trial):
    source = trial.get('source_identity') or {}
    return (trial.get('source_comparison_eligible') is True
            and source.get('commit_matches') is True and source.get('binary_matches') is True
            and source.get('source_hashes_match_phase', True) is True
            and not trial.get('artifact_issues'))


def lifecycle(trial):
    receipt = trial.get('trial_result_receipt') or {}
    warmup = (trial.get('trial_provenance') or {}).get('warmup') or {}
    errors = receipt.get('errors'); cleanup = receipt.get('cleanup_errors')
    owned=trial.get('owned_windows_cleanup_receipt') or {}
    stop=owned.get('collector_stop') or {}
    geometry = (trial.get('requested_s') == 120 and warmup.get('minimum_requested_s') == 45
                and finite(warmup.get('observed_host_monotonic_s'))
                and warmup['observed_host_monotonic_s'] >= 45)
    return dict(execution_errors=errors,cleanup_errors=cleanup,
                cleanup_qualified=cleanup == [],
                external_duration_geometry_qualified=geometry,
                execution_receipt_present=bool(receipt),
                owned_windows_cleanup_receipt=trial.get('owned_windows_cleanup_receipt'),
                collector_stop_status=stop.get('status'),
                collector_stop_elapsed_s=stop.get('elapsed_s'),
                fallback_owned_cleanup_required=stop.get('fallback_owned_cleanup_required'),
                qualification='Native/session failures are retained. Setup/lifecycle errors and absent cleanup receipts withhold comparison admission.')


def sampled_stalls(samples):
    """Describe linked sampled points; never infer activity between captures."""
    pairs=[(a,b) for a,b in zip(samples,samples[1:])
           if b.get('compared_previous_capture_end_ns')==a.get('capture_end_ns')]
    changes=[];longest=0.;run=0.;previous=None;max_gap=None;unknown=0
    for a,b in pairs:
        gap=(b['capture_end_ns']-a['capture_end_ns'])/1e9
        if gap<0:
            raise ValueError('image timestamps decreased')
        max_gap=gap if max_gap is None else max(max_gap,gap)
        changed=b.get('changed_pixels')
        if not count(changed):
            unknown+=1;run=0.;previous=None;continue
        changes.append(changed>0)
        if changed==0:
            run=(run if previous==a['capture_end_ns'] else 0.)+gap
            longest=max(longest,run);previous=b['capture_end_ns']
        else:
            run=0.;previous=None
    known=unknown==0 and bool(pairs)
    return dict(samples=len(samples),comparison_intervals=len(pairs),
                changed_intervals=sum(changes) if known else None,
                changed_fraction=sum(changes)/len(pairs) if known else None,
                longest_consecutive_unchanged_s=longest if known else None,
                trailing_consecutive_unchanged_s=run if known and pairs[-1][1] is samples[-1] else None,
                maximum_sample_gap_s=max_gap,unknown_change_intervals=unknown,
                qualification='Consecutive linked1Hz pixel samples. Identical sampled frames do not prove no activity between captures; this is not physical FPS or input latency.')


def add_visual_stalls(trial):
    visual=trial.get('visual_progress') or {}
    progress=visual.get('actual_phase_progress') or {}
    path=progress.get('sample_file')
    raw=trial.get('raw_campaign') or {};steps=raw.get('steps') or []
    step=steps[0] if len(steps)==1 else {}
    start=step.get('start_monotonic_s');end=step.get('end_monotonic_s')
    result=None
    if isinstance(path,str) and Path(path).is_file() and finite(start) and finite(end):
        cutoff=(visual.get('before_logged_terminal_progress') or {}).get('cutoff_utc_ns')
        admitted=[];parse_errors=0
        for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
            if not line.strip():continue
            try:
                row=json.loads(line)
                if not isinstance(row,dict):raise ValueError('image row must be object')
            except ValueError:
                parse_errors+=1;continue
            if row.get('type')!='sample':continue
            a=row.get('capture_start_ns');b=row.get('capture_end_ns')
            if not count(a) or not count(b) or not start*1e9<=a<=b<end*1e9:continue
            if cutoff is not None:
                utc=row.get('capture_end_utc_ns_estimate')
                if not count(utc) or utc>=cutoff:continue
            admitted.append(row)
        result=sampled_stalls(admitted)
        result['terminal_prefix_used']=cutoff is not None
        if progress.get('parse_errors') or parse_errors:
            result.update(changed_fraction=None,longest_consecutive_unchanged_s=None,
                          trailing_consecutive_unchanged_s=None,
                          qualification='Image sample parse errors withhold stall interpretation.')
    visual['sampled_stalls']=result
    longest=result.get('longest_consecutive_unchanged_s') if result else None
    changed=result.get('changed_intervals') if result else None
    visual['surviving_visual_attention']=(changed==0 or longest>=10) if trial.get('survived_requested_interval') is True and count(changed) and finite(longest) else None
    visual['attention_threshold_s']=10
    visual['attention_qualification']='Ten seconds or zero changed comparisons flags a surviving sampled visual stall for inspection, not an automatic transport failure.'
    trial['visual_progress']=visual


def brief(trial):
    visual = trial.get('visual_progress') or {}
    healthy = visual.get('before_logged_terminal_progress') or visual.get('actual_phase_progress') or {}
    scope = trial.get('protocol_receipt') or {}
    return dict(trial_id=trial['trial_id'], raw_directory=trial.get('raw_directory'),
                phase_id=trial.get('phase_id'), source_identity=trial.get('source_identity'),
                source_build_receipt=trial.get('source_build_receipt'),
                phase_build_receipt=trial.get('phase_build_receipt'),
                source_qualified=qualified_source(trial), outcome=trial.get('outcome'),
                survived_120s=trial.get('survived_requested_interval'),
                observed_duration_s=trial.get('observed_duration_s'),
                termination_cause=trial.get('termination_cause'),
                terminal_timing_receipt=trial.get('terminal_timing_receipt'),
                udp_tunnel=trial.get('udp_tunnel'), metrics=trial.get('metrics'),
                visual_progress=visual, healthy_visual_prefix=healthy,
                network=trial.get('network'), selected_protocol_receipt=scope,
                raw_provider_errors=trial.get('raw_provider_error_event_count'),
                admitted_phase_provider_errors=trial.get('counter_error_event_count'),
                all_provider_error_events_with_phase=trial.get('all_provider_error_events_with_phase'),
                collector_footer_present=(trial.get('counter_report') or {}).get('collector_terminal_receipt_present'),
                collector_terminal_receipts=(trial.get('counter_report') or {}).get('collector_terminal_receipts'),
                counter_report=trial.get('counter_report'),
                process_bound_counter_report=trial.get('process_bound_counter_report'),
                clock_alignment=trial.get('clock_alignment'),
                execution_error=trial.get('execution_error'),
                cleanup_errors=(trial.get('trial_result_receipt') or {}).get('cleanup_errors'),
                artifact_issues=trial.get('artifact_issues'),
                lifecycle=lifecycle(trial),
                **{name: None for name in UNKNOWN})


def compare_row(key, before, after, *, historical=False, external_controls_qualified=None):
    eligible = qualified_source(before) and qualified_source(after)
    for trial in (before,after):
        life=lifecycle(trial)
        if (life['cleanup_qualified'] is not True or life['external_duration_geometry_qualified'] is not True
                or life['execution_errors'] != []):
            eligible=False
    if not historical:
        if external_controls_qualified is not True:
            eligible=False
        for trial in (before, after):
            scope=trial.get('protocol_receipt') or {}
            if scope.get('selector_matches') is not True or scope.get('nonselected_bypass',{}).get('dropped_packets') != 0:
                eligible = False
        if key[1] == 'local':
            a = before['source_identity']; b = after['source_identity']
            if a.get('recorded_binary_sha256') != b.get('recorded_binary_sha256'):
                raise ValueError('local control binary changed between phases')
            if a.get('actual_source_hashes') != b.get('actual_source_hashes'):
                raise ValueError('local control source changed between phases')
    metrics = {}
    for name in sorted(set(before['metrics']) | set(after['metrics'])):
        a = before['metrics'].get(name); b = after['metrics'].get(name)
        av = a.get('mean') if a else None; bv = b.get('mean') if b else None
        metrics[name] = dict(before=a, after=b,
                            after_minus_before_mean=bv-av if eligible and finite(av) and finite(bv)
                            and count(a.get('n')) and a['n']>0 and count(b.get('n')) and b['n']>0 else None)
    return dict(protocol=key[0], engine=key[1], loss_percent=key[2],
                comparison_kind='historical-all-ipv4' if historical else 'protocol-selected-before-after',
                source_comparison_eligible=qualified_source(before) and qualified_source(after),
                comparison_eligible=eligible,before=brief(before), after=brief(after), metrics=metrics,
                qualification=('Earlier no-PCAP all-IPv4 campaign uses different collector/runner source and cleanup. '
                               if historical else '') +
                'One unseeded trial per phase and condition. Means of failed prefixes cover shorter durations. '
                'No pooling, statistical significance, physical FPS, verified goodput or latency-tail claim.')


def original_provider_count(trial):
    original=trial.get('process_bound_counter_report')
    if original is None:
        return trial.get('counter_error_event_count')
    steps=original.get('steps') or []
    return steps[0].get('counter_error_event_count') if len(steps)==1 else None


def audit(trials):
    return dict(planned=len(trials), survived_120s=sum(t.get('survived_requested_interval') is True for t in trials),
                session_ended=sum(t.get('outcome') == 'SESSION_ENDED' for t in trials),
                source_qualified=sum(qualified_source(t) for t in trials),
                artifact_issues=sum(len(t.get('artifact_issues') or []) for t in trials),
                raw_provider_errors=sum(t['raw_provider_error_event_count'] for t in trials)
                  if all(count(t.get('raw_provider_error_event_count')) for t in trials) else None,
                admitted_phase_provider_errors=sum(t['counter_error_event_count'] for t in trials)
                  if all(count(t.get('counter_error_event_count')) for t in trials) else None,
                original_process_bound_phase_provider_errors=sum(original_provider_count(t) for t in trials)
                    if all(count(original_provider_count(t)) for t in trials) else None,
                provider_error_unknown_trials=sum(not count(t.get('raw_provider_error_event_count')) for t in trials),
                missing_collector_footers=sum((t.get('counter_report') or {}).get('collector_terminal_receipt_present') is not True for t in trials),
                graceful_collector_timeouts=sum(lifecycle(t)['collector_stop_status']=='GRACEFUL_STOP_TIMEOUT' for t in trials),
                known_fallback_requests=sum(lifecycle(t)['fallback_owned_cleanup_required'] is True for t in trials),
                unknown_collector_stop_receipts=sum(lifecycle(t)['collector_stop_status'] is None for t in trials))


def same_coordinator(reports):
    values={name:(report.get('campaign_execution') or {}).get('coordinator_sha256')
            for name,report in reports.items() if name!='prior_all'}
    known=all(isinstance(value,str) and len(value)==64 for value in values.values())
    if known and len(set(values.values()))!=1:
        raise ValueError('production coordinator changed between current phases')
    return dict(coordinator_sha256_by_phase=values,qualified=known and len(set(values.values()))==1,
                qualification='Current production phases use one frozen coordinator; historical all-IPv4 has a separately recorded older harness.')


def write(prefix, report):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix('.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    fields = ['comparison_kind', 'protocol', 'engine', 'loss_percent', 'source_comparison_eligible','comparison_eligible',
              'before_trial_id', 'after_trial_id', 'before_outcome', 'after_outcome',
              'before_seconds', 'after_seconds', 'before_fps_mean', 'after_fps_mean',
              'before_fps_median', 'after_fps_median', 'before_fps_n', 'after_fps_n',
              'fps_mean_delta', 'before_termination', 'after_termination',
              'before_raw_provider_errors', 'after_raw_provider_errors',
              'before_phase_provider_errors', 'after_phase_provider_errors', *UNKNOWN]
    with prefix.with_name(prefix.name+'-pairs').with_suffix('.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator='\n'); writer.writeheader()
        for item in report['comparisons']:
            a=item['before']; b=item['after']; metric=item['metrics']['server_output_fps']
            row = {key:item.get(key) for key in fields}
            row.update(before_trial_id=a['trial_id'],after_trial_id=b['trial_id'],
                       before_outcome=a['outcome'],after_outcome=b['outcome'],
                       before_seconds=a['observed_duration_s'],after_seconds=b['observed_duration_s'],
                       before_fps_mean=metric['before']['mean'],after_fps_mean=metric['after']['mean'],
                       before_fps_median=metric['before']['median'],after_fps_median=metric['after']['median'],
                       before_fps_n=metric['before']['n'],after_fps_n=metric['after']['n'],
                       fps_mean_delta=metric['after_minus_before_mean'],
                       before_termination=a['termination_cause'],after_termination=b['termination_cause'],
                       before_raw_provider_errors=a['raw_provider_errors'],after_raw_provider_errors=b['raw_provider_errors'],
                       before_phase_provider_errors=a['admitted_phase_provider_errors'],after_phase_provider_errors=b['admitted_phase_provider_errors'])
            writer.writerow(row)
    trial_fields=['phase_id','protocol','engine','loss_percent','trial_id','outcome','observed_duration_s',
                  'survived_120s','source_qualified','cleanup_qualified','external_duration_geometry_qualified',
                  'termination_cause','logged_terminal_relative_s','logged_terminal_upper_relative_s',
                  'collector_stop_status','collector_stop_elapsed_s','fallback_owned_cleanup_required',
                  'server_output_fps_mean','server_output_fps_median','server_output_fps_n',
                  'raw_provider_errors','admitted_phase_provider_errors','collector_footer_present',
                  'small_sent','small_drops','small_drop_fraction','large_sent','large_drops','large_drop_fraction',
                  'bypass_sent','bypass_drops','visual_samples','visual_comparisons','visual_changes',
                  'visual_changed_fraction','visual_longest_unchanged_s','visual_attention',*UNKNOWN]
    with prefix.with_suffix('.csv').open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=trial_fields,lineterminator='\n');writer.writeheader()
        for trial in report['trials']:
            row={key:trial.get(key) for key in trial_fields}
            fps=trial['metrics']['server_output_fps'];scope=trial.get('selected_protocol_receipt') or {}
            bypass=scope.get('nonselected_bypass') or {};visual=trial.get('healthy_visual_prefix') or {}
            stalls=trial['visual_progress'].get('sampled_stalls') or {}
            row.update(server_output_fps_mean=fps['mean'],server_output_fps_median=fps['median'],server_output_fps_n=fps['n'],
                       cleanup_qualified=trial['lifecycle']['cleanup_qualified'],
                       external_duration_geometry_qualified=trial['lifecycle']['external_duration_geometry_qualified'],
                       collector_stop_status=trial['lifecycle']['collector_stop_status'],
                       collector_stop_elapsed_s=trial['lifecycle']['collector_stop_elapsed_s'],
                       fallback_owned_cleanup_required=trial['lifecycle']['fallback_owned_cleanup_required'],
                       logged_terminal_relative_s=(trial.get('terminal_timing_receipt') or {}).get('logged_terminal_relative_s'),
                       logged_terminal_upper_relative_s=(trial.get('terminal_timing_receipt') or {}).get('logged_terminal_upper_relative_s'),
                       bypass_sent=bypass.get('transmitted_packets'),bypass_drops=bypass.get('dropped_packets'),
                       visual_samples=visual.get('samples'),visual_comparisons=visual.get('comparison_intervals'),
                       visual_changes=visual.get('changed_intervals'),visual_changed_fraction=stalls.get('changed_fraction'),
                       visual_longest_unchanged_s=stalls.get('longest_consecutive_unchanged_s'),
                       visual_attention=trial['visual_progress'].get('surviving_visual_attention'))
            for name in ('small','large'):
                band=trial['network'].get(name) or {}
                row.update({name+'_sent':band.get('transmitted_packets'),name+'_drops':band.get('dropped_packets'),
                            name+'_drop_fraction':band.get('drop_fraction_completed_attempts')})
            writer.writerow(row)
    fmt=lambda x: 'unknown' if x is None else f'{x:.3f}' if isinstance(x,float) else str(x)
    lines=['# VM protocol-loss before/after comparison','',
           'Each row compares one independent connection before and after. Trials are not pooled. Failed-prefix FPS is retained with its shorter duration.', '',
           '| Scope | Client | Loss | Before outcome / seconds | After outcome / seconds | Before → after server FPS mean (N) |',
           '|---|---|---:|---|---|---:|']
    for row in report['comparisons']:
        a=row['before'];b=row['after'];m=row['metrics']['server_output_fps']
        scope=row['protocol']+(' (historical)' if row['comparison_kind']=='historical-all-ipv4' else '')
        lines.append(f"| {scope} | {row['engine']} | {row['loss_percent']}% | {a['outcome']} / {fmt(a['observed_duration_s'])} | {b['outcome']} / {fmt(b['observed_duration_s'])} | {fmt(m['before']['mean'])} ({m['before']['n']}) → {fmt(m['after']['mean'])} ({m['after']['n']}) |")
    lines += ['', 'Windows output FPS means frames made available by the server. Physical client FPS, CRC-verified goodput and latency p95/p99 are unknown.', '',
              'Poll duration is retained separately from logged fatal time. The PR all-IPv470 timeout was logged before120s even though process polling crossed120s; it is not admitted as120s survival.', '',
              'TCP-only/UDP-only means impairment selects that ingress protocol; both RDP transports remain enabled. Loss applies only to VM ingress; the return path is clean. No bandwidth cap or loss seed is used. Large selected packets retain 1ms delay and70% reordering even in the0% loss control. Actual large/small drops and nonselected bypass drops are separate in JSON.', '',
              'The baseline and fixed protocol matrices retain identical production local binaries. PR code/source identity changes are explicit. The historical all-IPv4 comparison uses the older counter/cleanup harness; it is a separate exploratory comparison.', '',
              'Provider errors before/during/after the admitted measurement window, partial visual prefixes, UDP/main-TCP termination, clocks, source hashes and cleanup remain in JSON. Diagnostic trace/capture runs never supply production comparison values.', '']
    lines += ['One fixed TCP25 PR collector stop exceeded its15s wait and requested guarded fallback cleanup. Its final requested-stop footer is retained; footer presence does not claim every bounded stop wait succeeded.', '']
    lines += ['| Phase | Scope | Client | Loss | Changed sampled intervals | Longest unchanged sampled span (s) | Visual attention |',
              '|---|---|---|---:|---:|---:|---|']
    for trial in report['trials']:
        stalls=trial['visual_progress'].get('sampled_stalls') or {}
        lines.append(f"| {trial['phase_id']} | {trial['protocol']} | {trial['engine']} | {trial['loss_percent']}% | {fmt(stalls.get('changed_intervals'))} / {fmt(stalls.get('comparison_intervals'))} | {fmt(stalls.get('longest_consecutive_unchanged_s'))} | {fmt(trial['visual_progress'].get('surviving_visual_attention'))} |")
    lines += ['', 'These are1Hz central-region pixel comparisons, excluding samples at/after the first logged terminal event. An unchanged sampled span is not continuous frame measurement. The10s attention threshold flags inspection of a surviving session, without declaring native failure.', '']
    prefix.with_suffix('.md').write_text('\n'.join(lines))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','fixed','fixed-all'):
        parser.add_argument('--'+name, required=True, type=Path)
    parser.add_argument('--prior-all', type=Path)
    parser.add_argument('--diagnostic-report', action='append', type=Path, default=[])
    parser.add_argument('--output-prefix', required=True, type=Path)
    args=parser.parse_args()
    paths={'baseline':args.baseline,'fixed':args.fixed,'fixed_all':args.fixed_all}
    reports={name:read(path) for name,path in paths.items()}
    old=phase_trials(reports['baseline'],('tcp','udp'),dict(local=LOCAL,pr=OLD_PR))
    new=phase_trials(reports['fixed'],('tcp','udp'),dict(local=LOCAL,pr=FIXED_PR))
    allnew=phase_trials(reports['fixed_all'],('all',),dict(local=LOCAL,pr=FIXED_PR))
    if args.prior_all:
        reports['prior_all']=read(args.prior_all);paths['prior_all']=args.prior_all
        allold=phase_trials(reports['prior_all'],('all',),dict(local=LOCAL,pr=OLD_PR),historical=True)
    for report in reports.values():
        for trial in report['trials']:
            if 'owned_windows_cleanup_receipt' not in trial and isinstance(trial.get('raw_directory'),str):
                cleanup=Path(trial['raw_directory'])/'cleanup.json'
                trial['owned_windows_cleanup_receipt']=read(cleanup) if cleanup.is_file() else None
            add_visual_stalls(trial)
    controls=same_coordinator(reports)
    comparisons=[compare_row(key,old[key],new[key],external_controls_qualified=controls['qualified']) for key in sorted(old)]
    if args.prior_all:
        comparisons += [compare_row(key,allold[key],allnew[key],historical=True) for key in sorted(allnew)]
    diagnostics=[]
    for path in args.diagnostic_report:
        d=read(path)
        if d.get('trial_kind')!='diagnostic' or any(t.get('performance_eligible') is not False for t in d['trials']):
            raise ValueError('observer runs must remain diagnostic')
        diagnostics.append(dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                phase_id=d['phase_id'],audit=d.get('audit_summary'),
                                trial_ids=[t['trial_id'] for t in d['trials']],
                                qualification='Retained separately; no production metric substitution.'))
    report=dict(schema='freerdp.vm_protocol_phase_comparison',version=1,
                generated_utc=datetime.now(timezone.utc).isoformat(),
                reporter_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                sources={name:dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                  phase_id=reports[name].get('phase_id')) for name,path in paths.items()},
                phase_audits={name:audit(d['trials']) for name,d in reports.items()},
                external_controls=controls,
                phase_provenance={name:{key:d.get(key) for key in ('phase_id','frozen_variants','run_configuration',
                                                                  'campaign_execution','reporter','provenance')}
                                  for name,d in reports.items()},
                trials=[dict(brief(t),protocol=key[0],engine=key[1],loss_percent=key[2])
                        for rows in (old,new,allnew) for key,t in rows.items()],
                comparisons=comparisons,fixed_all_trials=[brief(t) for t in allnew.values()],
                diagnostic_reports=diagnostics,
                qualification='Exploratory independent phase comparisons; no pooling, selective replacement or statistical significance claim.',
                **{name:None for name in UNKNOWN})
    write(args.output_prefix,report)
    print(json.dumps(dict(phase_audits=report['phase_audits'],comparison_rows=len(comparisons)),indent=2))


if __name__=='__main__':
    main()
