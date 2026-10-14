#!/usr/bin/env python3
"""Post-campaign, phase-specific analysis of protocol-selected VM loss trials.

No client, VM, container, network shaping or credential handling is performed.
Never pool baseline/fixed/diagnostic phases; every report consumes one index.
"""
from __future__ import annotations
import argparse
import csv
import copy
from datetime import datetime, timezone, timedelta
import hashlib
import importlib.util
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
LEGACY_PATH = BASE / 'report-comparison.py'
SPEC = importlib.util.spec_from_file_location('vm_counter_analysis', LEGACY_PATH)
counter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(counter)


def read(path):
    value = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict):
        raise ValueError('JSON object required: ' + str(path))
    return value


def validate_index(index, campaign):
    phase = index.get('phase_id')
    if not isinstance(phase, str) or not phase or campaign.get('phase_id') != phase:
        raise ValueError('one explicit matching phase identity required')
    protocols = index.get('protocols'); losses = index.get('loss_percent'); variants = index.get('variants')
    if not all(isinstance(items, list) and items and len(set(items)) == len(items)
               for items in (protocols, losses, variants)):
        raise ValueError('explicit distinct matrix dimensions required')
    if any(p not in ('tcp', 'udp', 'all') for p in protocols):
        raise ValueError('unknown selected protocol')
    if any(isinstance(loss, bool) or loss not in (0, 25, 50, 70) for loss in losses):
        raise ValueError('loss dimensions differ from declared experiment')
    if any(v not in ('local', 'pr') for v in variants):
        raise ValueError('unknown client variant')
    diagnostic = index.get('trial_kind') == 'diagnostic'
    if diagnostic:
        if len(protocols) != 1 or protocols[0] not in ('udp','all') or losses != [70] or variants != ['pr']:
            raise ValueError('diagnostic must remain separate PR70 UDP/all condition')
    elif losses != [0, 25, 50, 70] or set(variants) != {'local', 'pr'}:
        raise ValueError('performance phase must retain full loss/client matrix')
    expected = {(p, v, loss) for p in protocols for v in variants for loss in losses}
    trials = index.get('trials')
    if not isinstance(trials, list) or len(trials) != len(expected):
        raise ValueError('all planned conditions must remain in the index')
    if {(t.get('protocol'), t.get('engine'), t.get('loss_percent')) for t in trials} != expected:
        raise ValueError('index condition set mismatch')
    if any(t.get('phase_id') != phase for t in trials) or len({t.get('trial_id') for t in trials}) != len(trials):
        raise ValueError('mixed phases or duplicate trial identities')
    return phase, diagnostic


def delta(before, after):
    keys = ('transmitted_packets', 'dropped_packets')
    if not all(counter.count(row.get(key)) for row in (before, after) for key in keys):
        return dict(transmitted_packets=None, dropped_packets=None, drop_fraction_completed_attempts=None,
                    backlog_before_packets=before.get('backlog_packets'), backlog_after_packets=after.get('backlog_packets'))
    sent = after['transmitted_packets'] - before['transmitted_packets']
    drops = after['dropped_packets'] - before['dropped_packets']
    if sent < 0 or drops < 0:
        raise ValueError('bypass counters reset/decreased inside measured phase')
    return dict(transmitted_packets=sent, dropped_packets=drops,
                drop_fraction_completed_attempts=drops / (sent + drops) if sent + drops else None,
                backlog_before_packets=before.get('backlog_packets'), backlog_after_packets=after.get('backlog_packets'))


def protocol_receipt(trial, declared):
    raw = trial['raw_campaign']['steps'][0]
    net = raw.get('network') or {}
    before = net.get('qdisc_before') or {}; after = net.get('qdisc_after') or {}
    scopes = [(row.get('state') or {}).get('loss_protocol') for row in (before, after)]
    started = raw.get('start_utc') is not None
    matches = all(scope == declared for scope in scopes) if started else None
    if started and not matches:
        raise ValueError('measured selector differs from indexed protocol: ' + trial['trial_id'])
    bypass = delta(before.get('bypass_counters') or {}, after.get('bypass_counters') or {})
    return dict(selected_protocol=declared, before_selector=scopes[0], after_selector=scopes[1],
                selector_matches=matches, protocol_scope_before=before.get('protocol_scope'),
                protocol_scope_after=after.get('protocol_scope'), nonselected_bypass=bypass,
                qualification='Loss/delay/reorder affect only selected protocol; bypass queue drops, if any, remain separate and may compromise a clean bypass.')


def trace_receipt(directory):
    paths = [directory / name for name in ('recovery-trace.jsonl', 'recovery.jsonl') if (directory / name).is_file()]
    if not paths:
        return None
    if len(paths) != 1:
        raise ValueError('ambiguous diagnostic trace inputs')
    metadata = footer = None; events = errors = 0
    with paths[0].open(encoding='utf-8-sig') as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise ValueError('trace row is not an object')
            except ValueError:
                errors += 1; continue
            if row.get('kind') == 'metadata' or row.get('type') == 'metadata' or row.get('event') == 'metadata':
                metadata = row
            elif row.get('kind') == 'footer' or row.get('type') == 'footer' or row.get('event') == 'footer':
                footer = row
            else:
                events += 1
    return dict(path=str(paths[0]), metadata=metadata, footer=footer, retained_event_rows=events,
                parse_errors=errors,
                qualification='Bounded tail ring: overwritten prefix forbids complete-history or absence claims. Trace is diagnostic, never performance eligible.')


def terminal_timing(trial):
    """Qualify a known fatal log independently of the later process poll."""
    terminal=(trial.get('udp_tunnel') or {}).get('first_measured_terminal') or {}
    steps=(trial.get('raw_campaign') or {}).get('steps') or []
    if (trial.get('outcome')!='SESSION_ENDED' or trial.get('client_alive_end') is not False
            or len(steps)!=1 or terminal.get('kind') not in ('MAIN_TRANSPORT_TIMEOUT','UDP_FAILURE')
            or not terminal.get('linux_utc') or not steps[0].get('start_utc')):
        return None
    precision=terminal.get('timestamp_precision_ms')
    if not counter.finite(precision) or precision<0 or 'UTC Docker' not in terminal.get('timestamp_basis',''):
        return None
    start=datetime.fromisoformat(steps[0]['start_utc']);stamp=datetime.fromisoformat(terminal['linux_utc'])
    if start.tzinfo is None or stamp.tzinfo is None:
        return None
    elapsed=(stamp-start).total_seconds();upper=elapsed+precision/1000
    requested=trial.get('requested_s')
    before=(counter.finite(requested) and 0<=elapsed and upper<requested)
    return dict(logged_terminal=terminal,logged_terminal_relative_s=elapsed,
                logged_terminal_upper_relative_s=upper,poll_detected_duration_s=trial.get('observed_duration_s'),
                raw_client_alive_end=trial['client_alive_end'],raw_outcome=trial['outcome'],
                demonstrably_before_requested_end=before,
                counter_admission_end_utc=(stamp-timedelta(milliseconds=precision)).isoformat() if before else None,
                qualification='Fatal WLog time and phase start share the Linux UTC clock; date is inferred from this trial, WLog precision is retained. Process polling is a later detection bound, not the fatal instant.')


def refine_terminal_bounds(trial, directory, reporter):
    receipt=terminal_timing(trial)
    trial['terminal_timing_receipt']=receipt
    if receipt is None or receipt['demonstrably_before_requested_end'] is not True:
        return
    trial['survived_requested_interval']=False
    # Preserve the original process-bound interpretation, raw counters and raw
    # ramp duration. Only counter admission is narrowed to the logged fatal.
    original=trial['counter_report'];trial['process_bound_counter_report']=original
    raw=copy.deepcopy(trial['raw_campaign'])
    raw['steps'][0]['end_utc']=receipt['counter_admission_end_utc']
    refined=counter.counter_report(directory,raw,trial['trial_provenance'],trial['clock_alignment'],reporter,trial['artifact_issues'])
    refined['counter_terminal_bound_qualification']=receipt
    before=original['steps'][0];after=refined['steps'][0]
    receipt['counter_metric_n_before']={name:stats['n'] for name,stats in before['metrics'].items()}
    receipt['counter_metric_n_after']={name:stats['n'] for name,stats in after['metrics'].items()}
    receipt['counter_windows_before']=len(before.get('admitted_counter_windows') or [])
    receipt['counter_windows_after']=len(after.get('admitted_counter_windows') or [])
    trial['counter_report']=refined
    for key in ('metrics','counter_samples','counter_errors','cross_boundary_samples','counter_error_events',
                'counter_error_event_count','all_provider_error_events_with_phase'):
        trial[key]=after[key]


def compare_cells(trials, protocols, losses):
    result = []
    for protocol in protocols:
        for loss in losses:
            selected = {t['engine']: t for t in trials if t['protocol'] == protocol and t['loss_percent'] == loss}
            if set(selected) != {'local', 'pr'}:
                continue
            a, b = selected['local'], selected['pr']
            metrics = {}
            for name in a['metrics']:
                x = a['metrics'][name]; y = b['metrics'][name]
                eligible = a['performance_eligible'] and b['performance_eligible'] and a['source_comparison_eligible'] and b['source_comparison_eligible']
                metrics[name] = dict(local=x, pr=y,
                    pr_minus_local_mean=y['mean']-x['mean'] if eligible and counter.finite(x['mean']) and counter.finite(y['mean']) else None)
            result.append(dict(phase_id=a['phase_id'], protocol=protocol, loss_percent=loss,
                               local_trial_id=a['trial_id'], pr_trial_id=b['trial_id'], metrics=metrics,
                               local_outcome=a['outcome'], pr_outcome=b['outcome'],
                               qualification='One trial per client/condition in this phase. Failed-trial prefix means cover different durations; no pooling or significance claim.'))
    return result


def write(prefix, report):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix('.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    fields = ['phase_id', 'protocol', 'engine', 'loss_percent', 'trial_id', 'performance_eligible',
              'outcome', 'observed_duration_s', 'survived_requested_interval', 'termination_cause',
              'server_output_fps_mean', 'server_output_fps_median', 'server_output_fps_n',
              'server_skipped_network_fps_mean', 'server_udp_rtt_ms_mean', 'server_retransmission_percent_mean',
              'small_drop_fraction', 'large_drop_fraction', 'bypass_drops', 'bypass_sent',
              'visual_samples', 'visual_changes', 'visual_comparisons', 'visual_status',
              'phase_provider_errors', 'raw_provider_errors', 'collector_footer_present',
              *counter.UNKNOWN_MEASUREMENTS]
    with prefix.with_suffix('.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator='\n'); writer.writeheader()
        for trial in report['trials']:
            row = {key: trial.get(key) for key in fields}
            for name in ('server_output_fps', 'server_skipped_network_fps', 'server_udp_rtt_ms', 'server_retransmission_percent'):
                row[name+'_mean'] = trial['metrics'][name]['mean']
            row.update(server_output_fps_median=trial['metrics']['server_output_fps']['median'],
                       server_output_fps_n=trial['metrics']['server_output_fps']['n'],
                       small_drop_fraction=trial['network']['small']['drop_fraction_completed_attempts'],
                       large_drop_fraction=trial['network']['large']['drop_fraction_completed_attempts'],
                       bypass_drops=trial['protocol_receipt']['nonselected_bypass']['dropped_packets'],
                       bypass_sent=trial['protocol_receipt']['nonselected_bypass']['transmitted_packets'],
                       phase_provider_errors=trial['counter_error_event_count'], raw_provider_errors=trial['raw_provider_error_event_count'],
                       collector_footer_present=trial['counter_report']['collector_terminal_receipt_present'],
                       visual_status=trial['visual_progress']['status'])
            visual = trial['visual_progress'].get('before_logged_terminal_progress') or trial['visual_progress'].get('actual_phase_progress') or {}
            row.update(visual_samples=visual.get('samples'), visual_changes=visual.get('changed_intervals'),
                       visual_comparisons=visual.get('comparison_intervals'))
            writer.writerow(row)
    fmt = lambda value: 'unknown' if value is None else f'{value:.3f}' if isinstance(value, float) else str(value)
    lines = ['# Protocol-selected Windows VM loss phase', '',
             f"Phase: {report['phase_id']}. Kind: {report['trial_kind']}. No pooling with other phases.", '',
             '| Protocol | Client | Loss | Outcome | Seconds | Server FPS mean / median (N) | Termination |',
             '|---|---|---:|---|---:|---:|---|']
    for trial in report['trials']:
        fps = trial['metrics']['server_output_fps']
        lines.append(f"| {trial['protocol']} | {trial['engine']} | {trial['loss_percent']}% | {trial['outcome']} | {fmt(trial['observed_duration_s'])} | {fmt(fps['mean'])} / {fmt(fps['median'])} ({fps['n']}) | {trial['termination_cause'] or 'none recorded'} |")
    lines += ['', 'Server output FPS is frames made available by Windows, not physical client presentation FPS. Verified CRC goodput and transport/input-display p95/p99 remain unknown.', '',
              'Only selected VM-ingress protocol receives random loss and large-packet delay/reordering. Other protocol traffic uses the explicit bypass band; its drops are retained separately. Return traffic is clean, with no bandwidth cap.', '',
              'Source declarations, actual binary/library/source hashes, selector snapshots, before/after clocks, raw provider errors, contained counter windows, visual prefixes, cleanup and bounded diagnostic coverage remain in JSON.', '',
              'One unseeded connection per condition. No selective retries within a phase; diagnostic runs and older full campaigns remain separately labelled.', '']
    prefix.with_suffix('.md').write_text('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--index', type=Path, required=True)
    parser.add_argument('--output-prefix', type=Path, required=True)
    parser.add_argument('--campaign-complete', action='store_true', required=True)
    args = parser.parse_args()
    index = read(args.index); campaign = read(args.index.parent / 'campaign.json')
    if not isinstance(campaign.get('finished_utc'), str):
        raise ValueError('closed coordinator receipt required before analysis')
    phase, diagnostic = validate_index(index, campaign)
    frozen_path = args.index.parent / 'frozen-variants.json'
    frozen = read(frozen_path) if frozen_path.is_file() else dict(
        phase_id=phase, declared=campaign['settings']['variants'],
        observed={name:dict(binary=entry['binary'],binary_sha256=None,source_sha256={},libraries_sha256={})
                  for name,entry in campaign['settings']['variants'].items()},
        qualification='Preflight source/binary receipt unavailable; all comparisons remain source-unqualified.')
    if frozen.get('phase_id') != phase:
        raise ValueError('frozen source phase differs from campaign')
    declared = frozen['declared']; observed = frozen['observed']
    counter.EXPECTED_COMMITS = {name: value['commit'] for name, value in declared.items()}
    setup = {'builds': {name: dict(source_commit=declared[name]['commit'], source_label=declared[name].get('label'),
                                   declared=declared[name], **observed[name]) for name in declared}}
    reporter_spec = importlib.util.spec_from_file_location('upstream_counter_report', counter.REPORTER)
    reporter = importlib.util.module_from_spec(reporter_spec); reporter_spec.loader.exec_module(reporter)
    trials = []
    for number, item in enumerate(index['trials'], 1):
        directory = (args.index.parent / item['path']).resolve()
        if not directory.is_relative_to(BASE):
            raise ValueError('trial path outside this lab')
        entry = dict(item, order=number, _directory=directory)
        trial = counter.analyze_trial(entry, setup, reporter)
        refine_terminal_bounds(trial,directory,reporter)
        cleanup_path=directory/'cleanup.json'
        trial['owned_windows_cleanup_receipt']=read(cleanup_path) if cleanup_path.is_file() else None
        trial.update(phase_id=phase, protocol=item['protocol'], performance_eligible=not diagnostic,
                     protocol_receipt=protocol_receipt(trial, item['protocol']))
        if not campaign.get('packet_capture_enabled'):
            trial['capture_status'] = counter.disabled_capture_status(trial)
        elif any(isinstance(band,dict) and band.get('capture_limit_reached') is True
                 for band in (trial['capture_health'] or {}).values()):
            trial['capture_status'].update(status='PARTIAL_PACKET_LIMIT',wire_evidence_valid=False,
                                          qualification='Capture stopped at its hard packet bound; evidence after that point is unavailable.')
        trial['trace_receipt'] = trace_receipt(directory) if diagnostic else None
        config = declared[item['engine']]
        receipt = config.get('build_receipt')
        receipt_path = Path(receipt) if isinstance(receipt, str) else None
        if receipt_path is not None and not receipt_path.is_absolute():
            receipt_path = BASE / receipt_path
        trial['phase_build_receipt'] = read(receipt_path) if receipt_path is not None and receipt_path.is_file() else None
        trial['source_identity']['actual_source_hashes'] = observed[item['engine']]['source_sha256']
        recorded_source_hashes = (trial['trial_provenance'].get('binary') or {}).get('source_sha256')
        trial['source_identity']['source_hashes_match_phase'] = (
            recorded_source_hashes == observed[item['engine']]['source_sha256'] if recorded_source_hashes else None)
        if trial['source_identity']['source_hashes_match_phase'] is False:
            trial['source_comparison_eligible'] = False
            trial['artifact_issues'].append({'artifact':'trial_provenance.binary.source_sha256','error':'measured source differs from phase freeze'})
        trial['source_identity']['declared_source_is_base_revision'] = True
        trials.append(trial)
    result = dict(schema='freerdp.windows_protocol_loss_report',version=1,phase_id=phase,
                  generated_utc=datetime.now(timezone.utc).isoformat(),trial_kind=index['trial_kind'],
                  planned_trials=len(index['trials']),protocols=index['protocols'],loss_percent=index['loss_percent'],
                  frozen_variants=frozen,run_configuration=read(args.index.parent/'run-configuration.json'),
                  campaign_execution={key:campaign.get(key) for key in ('phase_id','started_utc','finished_utc','coordinator_sha256','settings_sha256','stopped_early_reason','packet_capture_enabled','trace_enabled')},
                  reporter=dict(script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                                inherited_analysis_sha256=hashlib.sha256(LEGACY_PATH.read_bytes()).hexdigest(),
                                upstream_analysis_sha256=hashlib.sha256(counter.REPORTER.read_bytes()).hexdigest()),
                  trials=trials,paired_conditions=compare_cells(trials,index['protocols'],index['loss_percent']),
                  audit_summary=dict(survived_120s=sum(t['survived_requested_interval'] is True for t in trials),
                                     ended=sum(t['outcome']=='SESSION_ENDED' for t in trials),
                                     graceful_collector_timeouts=sum(((t.get('owned_windows_cleanup_receipt') or {}).get('collector_stop') or {}).get('status')=='GRACEFUL_STOP_TIMEOUT' for t in trials),
                                     artifact_issues=sum(len(t['artifact_issues']) for t in trials),
                                     raw_provider_errors=sum(t['raw_provider_error_event_count'] for t in trials) if all(counter.count(t['raw_provider_error_event_count']) for t in trials) else None,
                                     raw_provider_error_unknown_trials=sum(not counter.count(t['raw_provider_error_event_count']) for t in trials),
                                     phase_provider_errors=sum(t['counter_error_event_count'] for t in trials) if all(counter.count(t['counter_error_event_count']) for t in trials) else None,
                                     phase_provider_error_unknown_trials=sum(not counter.count(t['counter_error_event_count']) for t in trials)),
                  qualification='Phase-specific exploratory server-counter/visual evidence; diagnostics never performance eligible; no pooling or statistical significance claim.')
    write(args.output_prefix,result)
    print(json.dumps({'phase_id':phase,'planned':len(trials),'audit':result['audit_summary']},indent=2))


if __name__ == '__main__':
    main()
