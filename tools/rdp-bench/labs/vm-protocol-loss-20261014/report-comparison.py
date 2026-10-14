#!/usr/bin/env python3
"""Post-campaign analysis of eight fresh Windows-to-FreeRDP comparisons.

This script never launches a client, changes shaping, or controls Windows.
Run only after all timed trials have ended. Raw trial inputs remain untouched.
"""
from __future__ import annotations

import argparse
import copy
import csv
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import statistics

ROOT = Path('/Users/david/projects/FreeRDP')
BASE = Path(__file__).resolve().parent
REPORTER = ROOT / 'tools/rdp-bench/upstream_method.py'
EXPECTED_COMMITS = {
    'local': '79ee244a4d5b2d5a280e165be4c643478484bf9e',
    'pr': 'a306a67c198f4493d8d2000bc10efd75b2432020',
}
CONDITIONS = {(engine, loss) for engine in EXPECTED_COMMITS for loss in (0, 25, 50, 70)}
UNKNOWN_MEASUREMENTS = {
    'verified_goodput_mbps': None, 'physical_client_fps': None,
    'transport_probe_rtt_p95_ms': None, 'transport_probe_rtt_p99_ms': None,
    'input_to_display_p95_ms': None, 'input_to_display_p99_ms': None,
    'crc_integrity_verified': None,
}
NOTES = [
    'One fresh transport connection per client/loss condition; results are exploratory, with no statistical significance claim.',
    'Windows can reconnect the same user desktop; fresh transport connection does not imply a new Windows login session.',
    'Loss affects all IPv4 arriving from Windows only, including TCP and UDP; the return direction is clean.',
    'No 10 Mbit/s rate cap; large IP packets >=512 bytes additionally receive 1 ms delay and 70% reordering; netem limit 100,000 packets.',
    'Zero percent is a random-loss control retaining large-packet delay/reordering, not an unshaped control.',
    'Veth replaces unavailable IFB; original small/large ICMP verification is a recorded method deviation.',
    'Kernel loss/reordering and the upstream coloured-noise workload are unseeded; parameters repeat, exact packet histories do not.',
    'Fresh independent conditions replace the original sequential ramp; all attempts are retained without reruns.',
    'Server output FPS is frames made available by Windows, not physical client presentation FPS.',
    'RemoteFX RTT is a sampled server counter, not a per-response transport probe or input/display latency distribution.',
    'A cooked server RTT value of zero is retained as reported; it does not establish a zero-millisecond transport RTT.',
    'Server bandwidth/sent-rate counters and encrypted packet counts do not establish CRC-verified application goodput.',
    'Sampled content progress is separate from observer health, client survival and UDP tunnel evidence; it is not an automatic native pass.',
    'Process polling bounds termination detection; visual samples after a logged terminal error are retained separately from the pre-error content prefix.',
    'Raw capture lifecycle errors and missing PDH collector-end receipts are preserved; contained valid PDH intervals do not establish complete capture or collector termination.',
    'Metric summaries are within each trial only; no pooling across different conditions or survivor-only aggregate.',
    'The two full clients differ in their FreeRDP base versions and other changes; this is not an isolated UDP-patch effect.',
    'The published Dell/Debian/Hyper-V environment differs from this Mac/Docker/Windows ARM VM; absolute FPS is environment-dependent.',
]


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def count(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def read(path, issues, *, required=False):
    if not path.is_file():
        if required:
            issues.append({'artifact': str(path), 'error': 'missing'})
        return None
    try:
        result = json.loads(path.read_text(encoding='utf-8-sig'))
        if not isinstance(result, dict):
            raise ValueError('JSON object required')
        return result
    except (ValueError, OSError) as error:
        issues.append({'artifact': str(path), 'error': str(error)})
        return None


def first_receipt(directory, names, issues):
    present = [directory / name for name in names if (directory / name).is_file()]
    if len(present) > 1:
        issues.append({'artifact': [str(p) for p in present], 'error': 'ambiguous receipt alternatives'})
        return None
    return read(present[0], issues) if present else None


def offset_bound(before, after, key):
    if not isinstance(before, dict) or not isinstance(after, dict):
        return None
    values = [before.get(key), after.get(key)]
    errors = [before.get('uncertainty_s'), after.get('uncertainty_s')]
    if not all(finite(v) for v in [*values, *errors]) or any(v < 0 for v in errors):
        return None
    return (statistics.mean(values), max(errors) + abs(values[0] - values[1]) / 2)


def clocks(directory, provenance, issues):
    before = read(directory / 'clock-before.json', issues)
    after = read(directory / 'clock-after.json', issues)
    linux_before = provenance.get('linux_clock_before', provenance.get('linux_clock_alignment'))
    if not isinstance(linux_before, dict):
        linux_before = first_receipt(directory, ('linux-clock-before.json', 'linux-clock-alignment.json'), issues)
    linux_after = read(directory / 'linux-clock-after.json', issues)
    win = offset_bound((before or {}).get('selected'), (after or {}).get('selected'), 'offset_windows_minus_host_s')
    selected_linux_before = linux_before.get('selected', linux_before) if isinstance(linux_before, dict) else None
    selected_linux_after = linux_after.get('selected', linux_after) if isinstance(linux_after, dict) else None
    linux = offset_bound(selected_linux_before, selected_linux_after, 'offset_linux_minus_host_s')
    if win is None or linux is None:
        return {'status': 'UNKNOWN', 'method': None, 'uncertainty_ms': None,
                'reason': 'both Windows/host and Linux/host before/after bounds required',
                'windows_before': before, 'windows_after': after,
                'linux_before': linux_before, 'linux_after': linux_after}
    uncertainty = win[1] + linux[1]
    return {'status': 'RECORDED',
            'method': 'before/after Windows-host and Linux-host UTC brackets; boundary exclusion',
            'windows_minus_linux_s': win[0] - linux[0], 'uncertainty_s': uncertainty,
            'uncertainty_ms': uncertainty * 1000,
            'qualification': 'Assumes phase clock drift stays within the endpoint offset spread; not continuous clock monitoring.',
            'windows_before': before, 'windows_after': after,
            'linux_before': linux_before, 'linux_after': linux_after}


def network(step):
    net = step.get('network') or {}
    before = net.get('qdisc_before') or {}
    after = net.get('qdisc_after') or {}
    result = {'scope': 'per-band qdisc completed attempts; drops may include causes other than random loss',
              'source_filter': (before.get('state') or {}).get('vm_ip'),
              'recorded_loss_percent': (before.get('state') or {}).get('loss_percent'),
              'rate_cap_mbps': (before.get('state') or {}).get('rate_cap_mbps'),
              'small': None, 'large': None}
    for band in ('small', 'large'):
        a = (before.get('netem_counters') or {}).get(band) or {}
        b = (after.get('netem_counters') or {}).get(band) or {}
        keys = ('transmitted_packets', 'dropped_packets')
        if not all(count(x.get(k)) for x in (a, b) for k in keys):
            result[band] = {'transmitted_packets': None, 'dropped_packets': None,
                            'drop_fraction_completed_attempts': None,
                            'backlog_before_packets': a.get('backlog_packets'),
                            'backlog_after_packets': b.get('backlog_packets'),
                            'qualification': 'missing/unavailable counters'}
            continue
        sent = b['transmitted_packets'] - a['transmitted_packets']
        dropped = b['dropped_packets'] - a['dropped_packets']
        known = sent >= 0 and dropped >= 0
        result[band] = {'transmitted_packets': sent if known else None,
                        'dropped_packets': dropped if known else None,
                        'drop_fraction_completed_attempts': dropped / (sent + dropped) if known and sent + dropped else None,
                        'backlog_before_packets': a.get('backlog_packets'),
                        'backlog_after_packets': b.get('backlog_packets'),
                        'qualification': 'after-minus-before; pending packets separate' if known else 'counters decreased/reset inside measured phase'}
    return result


def tunnel(directory, step, issues):
    receipt = first_receipt(directory, ('udp-ready.json', 'readiness.json', 'client-ready.json'), issues)
    path = directory / 'client.log'
    text = path.read_text(errors='replace') if path.is_file() else None
    ready = None if text is None else any(term in text for term in (
        'UDP multitransport tunnel ready', 'ready (async)'))
    failure_terms = ('UDP tunnel broke down', 'UDP reassembly stalled', 'UDP peer silent',
                     'RDP-UDP connection failed:', 'UDP receive budget exhausted',
                     'UDP channel receive window overflow', 'unacknowledged after')
    failures = [] if text is None else [line for line in text.splitlines() if any(term in line for term in failure_terms)]
    terminals = [] if text is None else [line for line in text.splitlines() if any(term in line for term in (
        'ERRCONNECT', 'BIO_read', 'Transport write failure', 'TcpAckTimeout', 'ending the connection'))]
    events = []
    start = end = None
    if step.get('start_utc') and step.get('end_utc'):
        start = datetime.fromisoformat(step['start_utc']); end = datetime.fromisoformat(step['end_utc'])
    for line in dict.fromkeys([*failures, *terminals]):
        if not any(level in line for level in ('[ERROR]', '[WARN]')):
            continue
        kind = ('UDP_FAILURE' if any(term in line for term in failure_terms)
                else 'MAIN_TRANSPORT_TIMEOUT' if 'BIO_read' in line and 'Connection timed out' in line
                else 'TRANSPORT_ERROR')
        match = re.match(r'^\[(\d{2}):(\d{2}):(\d{2}):(\d{3})\]', line)
        stamp = None
        if match and start is not None:
            hour, minute, second, millis = map(int, match.groups())
            candidates = [start.replace(hour=hour, minute=minute, second=second, microsecond=millis * 1000)
                          + timedelta(days=delta) for delta in (-1, 0, 1)]
            stamp = min(candidates, key=lambda value: abs((value - start).total_seconds()))
        phase = ('DURING_MEASUREMENT' if start is not None and stamp is not None and start <= stamp <= end
                 else 'BEFORE_MEASUREMENT' if start is not None and stamp is not None and stamp < start
                 else 'AFTER_MEASUREMENT' if start is not None and stamp is not None else 'UNASSIGNED')
        events.append({'kind': kind, 'phase': phase, 'linux_utc': stamp.isoformat() if stamp else None,
                       'line': line, 'timestamp_precision_ms': 1,
                       'timestamp_basis': 'date inferred from trial and WLog clock in UTC Docker environment'})
    measured = [event for event in events if event['phase'] == 'DURING_MEASUREMENT']
    udp_failed = any(event['kind'] == 'UDP_FAILURE' for event in measured)
    first_terminal = min(measured, key=lambda event: event['linux_utc']) if measured else None
    return {'initial_ready_log_observed': ready, 'readiness_receipt': receipt,
            'failure_log_observed': bool(failures) if text is not None else None,
            'measured_udp_failure_logged': udp_failed if text is not None else None,
            'status': 'MEASURED_UDP_FAILURE_LOGGED' if udp_failed else 'READY_NO_MEASURED_UDP_FAILURE_RECORDED' if ready else 'UNKNOWN',
            'failure_logs': failures, 'terminal_logs': terminals,
            'classified_terminal_events': events, 'first_measured_terminal': first_terminal,
            'active_at_measurement_end': None,
            'qualification': 'Initial readiness and recorded failure are separate; absence of a failure log does not prove uninterrupted UDP activity.'}


def visual(directory, step, issues, transport=None):
    summary = first_receipt(directory, ('images.summary.json', 'image-progress.summary.json'), issues)
    status = read(directory / 'image-observer-status.json', issues)
    if summary is None:
        return {'status': 'UNAVAILABLE', 'summary': None, 'process_receipt': status,
                'qualification': 'No readable image observer summary; no visual success inference.'}
    duration = step.get('observed_duration_s')
    start = step.get('start_monotonic_s')
    end = step.get('end_monotonic_s')
    actual_capture_bounds_match = None
    if all(finite(v) for v in (start, end)):
        first = summary.get('first_capture_end_ns'); last = summary.get('last_capture_end_ns')
        if count(first) and count(last):
            actual_capture_bounds_match = start * 1e9 <= first <= last < end * 1e9
    image_paths = [directory / name for name in ('images.jsonl', 'image-progress.jsonl') if (directory / name).is_file()]
    phase_progress = None
    before_terminal_progress = None
    if len(image_paths) == 1 and all(finite(v) for v in (start, end)) and end > start:
        admitted = []; excluded = []; errors = []
        for number, line in enumerate(image_paths[0].read_text(encoding='utf-8-sig').splitlines(), 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise ValueError('image row must be an object')
                if row.get('type') != 'sample':
                    continue
                a = row.get('capture_start_ns'); b = row.get('capture_end_ns')
                if not count(a) or not count(b) or b < a:
                    raise ValueError('invalid image capture interval')
                if start * 1e9 <= a <= b < end * 1e9:
                    admitted.append(row)
                else:
                    excluded.append({'line': number, 'capture_start_ns': a, 'capture_end_ns': b,
                                     'reason': 'image capture outside actual native measured interval'})
            except (TypeError, ValueError) as error:
                errors.append({'line': number, 'error': str(error)})
        comparisons = [(a, b) for a, b in zip(admitted, admitted[1:])
                       if b.get('compared_previous_capture_end_ns') == a['capture_end_ns']]
        known_changes = [b.get('changed_pixels') > 0 for _, b in comparisons if count(b.get('changed_pixels'))]
        known_substantial = [b['substantial_change'] for _, b in comparisons if isinstance(b.get('substantial_change'), bool)]
        phase_progress = {
            'sample_file': str(image_paths[0]), 'samples': len(admitted),
            'comparison_intervals': len(comparisons),
            'changed_intervals': sum(known_changes) if len(known_changes) == len(comparisons) else None,
            'substantial_change_intervals': sum(known_substantial) if len(known_substantial) == len(comparisons) else None,
            'coverage_s': (admitted[-1]['capture_end_ns'] - admitted[0]['capture_end_ns']) / 1e9 if len(admitted) >= 2 else None,
            'first_capture_end_ns': admitted[0]['capture_end_ns'] if admitted else None,
            'last_capture_end_ns': admitted[-1]['capture_end_ns'] if admitted else None,
            'excluded_samples': excluded, 'parse_errors': errors,
            'qualification': 'Only captures wholly inside actual native phase; comparisons require the previous admitted capture, excluding post-exit/cleanup frames.'}
        terminal = (transport or {}).get('first_measured_terminal')
        if terminal and terminal.get('linux_utc'):
            terminal_ns = int(datetime.fromisoformat(terminal['linux_utc']).timestamp() * 1e9)
            # WLog timestamps have millisecond precision. Leave an additional
            # millisecond before the terminal event rather than admit a
            # capture that may overlap the recorded failure.
            cutoff = terminal_ns - 1_000_000
            prefix = [row for row in admitted if count(row.get('capture_end_utc_ns_estimate'))
                      and row['capture_end_utc_ns_estimate'] < cutoff]
            prefix_pairs = [(a, b) for a, b in zip(prefix, prefix[1:])
                            if b.get('compared_previous_capture_end_ns') == a['capture_end_ns']]
            changes = [b['changed_pixels'] > 0 for _, b in prefix_pairs if count(b.get('changed_pixels'))]
            substantial = [b['substantial_change'] for _, b in prefix_pairs if isinstance(b.get('substantial_change'), bool)]
            before_terminal_progress = {
                'samples': len(prefix), 'comparison_intervals': len(prefix_pairs),
                'changed_intervals': sum(changes) if len(changes) == len(prefix_pairs) else None,
                'substantial_change_intervals': sum(substantial) if len(substantial) == len(prefix_pairs) else None,
                'coverage_s': (prefix[-1]['capture_end_ns'] - prefix[0]['capture_end_ns']) / 1e9 if len(prefix) >= 2 else None,
                'terminal_event': terminal, 'cutoff_utc_ns': cutoff,
                'samples_at_or_after_terminal_or_without_utc': len(admitted) - len(prefix),
                'qualification': 'Observed content prefix before first logged terminal event; process polling is a later termination-detection bound.'}
    elif len(image_paths) > 1:
        issues.append({'artifact': [str(p) for p in image_paths], 'error': 'ambiguous image sample alternatives'})
    positive = (phase_progress is not None and count(phase_progress.get('substantial_change_intervals'))
                and phase_progress['substantial_change_intervals'] > 0)
    scoped_valid = (phase_progress is not None and phase_progress['samples'] >= 2
                    and phase_progress['comparison_intervals'] >= 1
                    and not phase_progress['parse_errors'])
    if summary.get('observer_valid') is not True:
        label = 'OBSERVER_INVALID'
    elif not scoped_valid:
        label = 'INSUFFICIENT_ALIGNED_VISUAL_EVIDENCE'
    elif (summary.get('completed_measurement_window') is True and summary.get('sampling_complete') is True
          and actual_capture_bounds_match is True and step.get('outcome') == 'COMPLETE'):
        label = 'COMPLETE_SAMPLED_PROGRESS' if positive else 'COMPLETE_NO_SUBSTANTIAL_CHANGE_OBSERVED'
    else:
        label = 'PARTIAL_SAMPLED_PROGRESS' if positive else 'PARTIAL_NO_SUBSTANTIAL_CHANGE_OBSERVED'
    if before_terminal_progress is not None:
        label = 'PARTIAL_PROGRESS_BEFORE_LOGGED_TERMINATION'
    ppm = directory / 'image-progress.last.ppm'
    terminal_black_frame = None
    if before_terminal_progress is not None and ppm.is_file():
        blocks = ppm.read_bytes().split(b'\n', 3)
        if len(blocks) == 4 and blocks[0] == b'P6' and blocks[2] == b'255':
            try:
                width, height = map(int, blocks[1].split())
                if width > 0 and height > 0 and len(blocks[3]) == width * height * 3:
                    terminal_black_frame = not any(blocks[3])
            except ValueError:
                pass
    return {'status': label, 'summary': summary, 'process_receipt': status,
            'actual_phase_progress': phase_progress,
            'before_logged_terminal_progress': before_terminal_progress,
            'last_saved_frame_all_black': terminal_black_frame,
            'capture_bounds_within_actual_phase': actual_capture_bounds_match,
            'actual_native_phase_seconds': duration,
            'qualification': 'Central-region pixel samples, not physical FPS, latency, full-stream integrity or automatic native success.'}


def counter_report(directory, raw_campaign, provenance, alignment, reporter, issues):
    counter_path = directory / 'counters.jsonl'
    rows, parse_errors = reporter.read_counter_rows(counter_path) if counter_path.is_file() else ([], [{'error': 'counter artifact missing'}])
    campaign = copy.deepcopy(raw_campaign)
    campaign['provenance'] = provenance
    campaign['clock_alignment'] = alignment
    discovery = read(directory / 'session-discovery.json', issues) or {}
    instance = provenance.get('counter_instance', discovery.get('counter_instance'))
    campaign['counter_instance'] = instance
    safe = []; exclusions = []; intervals = []
    if alignment['status'] == 'RECORDED':
        offset = alignment['windows_minus_linux_s']; uncertainty = alignment['uncertainty_s']
        for step in campaign['steps']:
            if not step.get('start_utc') or not step.get('end_utc'):
                continue
            for name in ('start', 'end'):
                step['windows_' + name + '_utc'] = (reporter.timestamp(step[name + '_utc']) + timedelta(seconds=offset)).isoformat()
            intervals.append((reporter.timestamp(step['windows_start_utc']), reporter.timestamp(step['windows_end_utc'])))
        for row in rows:
            if row.get('type') != 'sample':
                safe.append(row); continue
            try:
                a = reporter.timestamp(row['interval_start_utc']); b = reporter.timestamp(row['interval_end_utc'])
                if b <= a:
                    raise ValueError('nonpositive counter interval')
                exclude = False
                for start, end in intervals:
                    if start <= b < end and a >= start and not (start + timedelta(seconds=uncertainty) <= a and b < end - timedelta(seconds=uncertainty)):
                        exclude = True
                if exclude:
                    exclusions.append({'reason': 'counter interval within clock uncertainty of phase boundary', 'row': row})
                else:
                    safe.append(row)
            except (KeyError, TypeError, ValueError) as error:
                exclusions.append({'reason': 'invalid counter interval: ' + str(error), 'row': row})
    # Without explicit session identity and aligned host clocks, never select
    # the first PDH instance or classify Linux times as Windows counter times.
    qualified_identity = (isinstance(instance, str) and bool(instance)
                          and discovery.get('counter_instance_evidence') == 'CORROBORATED'
                          and isinstance(discovery.get('counter_instance'), str)
                          and discovery['counter_instance'].casefold() == instance.casefold())
    admitted = safe if alignment['status'] == 'RECORDED' and qualified_identity else []
    result = reporter.summarize(campaign, admitted, parse_errors)
    result['counter_input_available'] = counter_path.is_file()
    result['counter_instance_proof'] = discovery
    result['phase_assignment_eligible'] = alignment['status'] == 'RECORDED' and qualified_identity
    result['boundary_exclusions'] = exclusions
    result['all_counter_metadata'] = [row for row in rows if row.get('type') == 'metadata']
    result['all_counter_diagnostics'] = [row for row in rows if row.get('type') not in ('metadata', 'sample')]
    result['raw_counter_rows'] = rows
    result['raw_provider_error_events'] = [row for row in rows if row.get('type') == 'collector_error']
    result['raw_provider_error_event_count'] = len(result['raw_provider_error_events']) if counter_path.is_file() else None
    result['collector_terminal_receipt_present'] = any(row.get('type') == 'collector_end' for row in rows)
    result['collector_completion_qualification'] = ('collector_end receipt retained' if result['collector_terminal_receipt_present']
                                                  else 'No collector-end receipt; caller stopped collection during cleanup. Contained valid intervals remain evidence, full collector completion is unknown.')
    result['whole_series_observed_cadence_s'] = reporter.numeric_stats(
        [row.get('observed_interval_s') for row in rows if row.get('type') == 'sample'])
    result['raw_counter_sample_count'] = sum(row.get('type') == 'sample' for row in rows)
    for measured in result['steps']:
        original = next(step for step in raw_campaign['steps'] if step['id'] == measured['id'])
        monotonic_duration = original.get('observed_duration_s')
        measured['reported_utc_wall_span_s'] = measured.get('observed_duration_s')
        measured['native_monotonic_duration_s'] = monotonic_duration
        if (original.get('outcome') == 'COMPLETE' and original.get('client_alive_end') is True
                and finite(monotonic_duration) and monotonic_duration >= original.get('requested_s', 120)
                and measured.get('outcome') == 'INCOMPLETE'):
            measured['upstream_reporter_outcome_before_monotonic_qualification'] = measured['outcome']
            measured['outcome'] = 'COMPLETE'
            measured['counter_errors'] = [error for error in measured['counter_errors']
                                         if error != 'declared complete without duration/client survival proof']
            measured['native_survival_qualification'] = (
                '120-second survival is proven by raw monotonic process evidence; UTC wall-clock span only assigns counter intervals and may be slightly shorter after clock adjustment.')
        events = []
        if measured.get('windows_start_utc') and measured.get('windows_end_utc'):
            start = reporter.timestamp(measured['windows_start_utc']); end = reporter.timestamp(measured['windows_end_utc'])
            for row in rows:
                if row.get('type') == 'collector_error':
                    try:
                        if start <= reporter.timestamp(row['utc']) < end:
                            events.append(row)
                    except (KeyError, ValueError, TypeError):
                        pass
        measured['counter_error_events'] = events
        measured['counter_error_event_count'] = len(events) if counter_path.is_file() and result['phase_assignment_eligible'] else None
        provider_events = []
        for event in result['raw_provider_error_events']:
            phase = 'UNASSIGNED'
            try:
                stamp = reporter.timestamp(event['utc'])
                if result['phase_assignment_eligible'] and measured.get('windows_start_utc') and measured.get('windows_end_utc'):
                    start = reporter.timestamp(measured['windows_start_utc']); end = reporter.timestamp(measured['windows_end_utc'])
                    margin = timedelta(seconds=alignment['uncertainty_s'])
                    phase = ('BEFORE_MEASUREMENT' if stamp < start - margin
                             else 'AFTER_MEASUREMENT' if stamp > end + margin
                             else 'DURING_MEASUREMENT' if start + margin <= stamp < end - margin
                             else 'BOUNDARY_UNCERTAIN')
            except (KeyError, ValueError, TypeError):
                pass
            provider_events.append({'phase': phase, 'raw_event': event})
        measured['all_provider_error_events_with_phase'] = provider_events
        windows = []
        if measured.get('windows_start_utc') and measured.get('windows_end_utc'):
            start = reporter.timestamp(measured['windows_start_utc']); end = reporter.timestamp(measured['windows_end_utc'])
            for row in admitted:
                if row.get('type') != 'sample' or not row.get('interval_start_utc'):
                    continue
                a = reporter.timestamp(row['interval_start_utc']); b = reporter.timestamp(row['interval_end_utc'])
                if start <= a <= b < end:
                    values, problems = reporter.sample_metrics(row, instance)
                    windows.append({'interval_start_utc': row['interval_start_utc'],
                                    'interval_end_utc': row['interval_end_utc'],
                                    'actual_span_s': (b - a).total_seconds(), 'metrics': values,
                                    'metric_issues': problems})
        measured['admitted_counter_windows'] = windows
        measured['admitted_cadence_s'] = reporter.numeric_stats([row['actual_span_s'] for row in windows])
        for metric, stats in measured['metrics'].items():
            if stats['n'] != sum(metric in row['metrics'] for row in windows):
                raise ValueError('counter evidence windows disagree with metric N: ' + metric)
    return result


def capture_status(health, finalization=None):
    if isinstance(finalization, dict):
        receipts = finalization.get('receipts') or []
        drops = []
        for receipt in receipts:
            matches = [re.search(r'^(\d+) packets dropped by kernel$', line) for line in receipt.get('footer', [])]
            values = [int(match[1]) for match in matches if match]
            if len(values) == 1:
                drops.append(values[0])
        known = len(receipts) == 2 and len(drops) == 2 and all(r.get('still_live') is False for r in receipts)
        return {'status': 'INVALID', 'wire_evidence_valid': False,
                'kernel_drops': sum(drops) if known else None,
                'kernel_drop_scope': 'whole capture lifetime, including unintended later trials; not per measured phase',
                'performance_confounded': True,
                'capture_cpu_seconds': sum(r['cpu_seconds'] for r in receipts) if all(finite(r.get('cpu_seconds')) for r in receipts) else None,
                'errors': ['Capture tasks continued beyond intended cleanup; changing background load confounds performance.'],
                'original_cleanup_health': health, 'supplementary_finalization': finalization,
                'qualification': 'Raw captures preserved; kernel losses and overlapping lifetimes prevent packet-absence/completeness claims.'}
    if not isinstance(health, dict):
        return {'status': 'UNAVAILABLE', 'wire_evidence_valid': None, 'kernel_drops': None, 'errors': []}
    bands = [health.get(name) for name in ('offered', 'received')]
    errors = [band['error'] for band in bands if isinstance(band, dict) and band.get('error')]
    known = all(isinstance(band, dict) and band.get('complete') is True and count(band.get('kernel_drops')) for band in bands)
    drops = sum(band['kernel_drops'] for band in bands) if known else None
    return {'status': 'VALID' if known and drops == 0 and not errors else 'INVALID' if known else 'UNKNOWN',
            'wire_evidence_valid': known and drops == 0 and not errors if known else None,
            'kernel_drops': drops, 'errors': errors,
            'qualification': 'No completeness or zero-kernel-drop assertion without both capture termination/statistics receipts.'}


def disabled_capture_status(trial):
    # The frozen coordinator writes capture-health.json={} during cleanup even
    # when its capture map was never populated. That empty receipt is distinct
    # from a failed capture or a falsely asserted zero-drop result.
    if (trial['capture_health'] not in (None, {})
            or any(name.endswith('.pcap') or '-capture-owned.json' in name for name in trial['artifacts'])):
        raise ValueError('disabled-capture campaign contains capture activity')
    return {'status': 'NOT_REQUESTED', 'wire_evidence_valid': None, 'kernel_drops': None,
            'performance_confounded': False, 'errors': [],
            'qualification': 'Coordinator declares packet capture disabled; capture map empty and no PCAP/owned-capture artifacts present.'}


def analyze_trial(declaration, setup, reporter):
    directory = declaration['_directory']; issues = []
    raw = read(directory / 'ramp/campaign.json', issues)
    provenance = read(directory / 'provenance.json', issues) or {}
    failure = read(directory / 'execution-error.json', issues)
    attempt = read(directory / 'attempt.json', issues)
    trial_result = read(directory / 'trial-result.json', issues)
    if raw is None:
        raw = {'steps': [{'id': 'loss' + str(declaration['loss_percent']), 'loss_percent': declaration['loss_percent'],
                          'requested_s': 120, 'outcome': 'FAILED_SETUP' if failure else 'NOT_RUN'}]}
    if len(raw.get('steps', [])) != 1 or raw['steps'][0].get('loss_percent') != declaration['loss_percent']:
        raise ValueError('fresh trial must have exactly its declared loss step: ' + declaration['trial_id'])
    step = raw['steps'][0]
    if step.get('requested_s', 120) != 120:
        raise ValueError('trial duration differs from frozen 120-second condition')
    alignment = clocks(directory, provenance, issues)
    counters = counter_report(directory, raw, provenance, alignment, reporter, issues)
    measured = counters['steps'][0]
    duration = step.get('observed_duration_s')
    if not finite(duration) and all(finite(step.get(k)) for k in ('start_monotonic_s', 'end_monotonic_s')):
        duration = step['end_monotonic_s'] - step['start_monotonic_s']
    alive = step.get('client_alive_end')
    survival = (True if step.get('outcome') == 'COMPLETE' and alive is True and finite(duration) and duration >= 120
                else False if step.get('outcome') == 'SESSION_ENDED' and finite(duration) and duration < 120 else None)
    exit_receipt = read(directory / 'client-exit.json', issues)
    source = setup['builds'][declaration['engine']]
    actual_commit = provenance.get('source_commit')
    if actual_commit and actual_commit != EXPECTED_COMMITS[declaration['engine']]:
        issues.append({'artifact': 'provenance.source_commit', 'error': 'unexpected source commit', 'value': actual_commit})
    actual_binary = provenance.get('binary')
    actual_binary_sha = actual_binary.get('binary_sha256') if isinstance(actual_binary, dict) else None
    source_identity = {'expected_commit': EXPECTED_COMMITS[declaration['engine']], 'recorded_commit': actual_commit,
                       'commit_matches': actual_commit == EXPECTED_COMMITS[declaration['engine']] if actual_commit else None,
                       'expected_binary_sha256': source['binary_sha256'], 'recorded_binary_sha256': actual_binary_sha,
                       'binary_matches': actual_binary_sha == source['binary_sha256'] if actual_binary_sha else None}
    if source_identity['binary_matches'] is False:
        issues.append({'artifact': 'provenance.binary.binary_sha256', 'error': 'unexpected measured binary', 'value': actual_binary_sha})
    transport = tunnel(directory, step, issues)
    capture_health = read(directory / 'capture-health.json', issues)
    capture_finalization = read(directory / 'capture-finalization.json', issues)
    progress = visual(directory, step, issues, transport)
    artifacts = {str(p.relative_to(directory)): {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
                 for p in sorted(directory.rglob('*')) if p.is_file()}
    return {k: declaration[k] for k in ('trial_id', 'engine', 'loss_percent', 'order')} | {
        'raw_directory': str(directory), 'outcome': step.get('outcome', 'UNKNOWN'),
        'counter_report_outcome': measured.get('outcome'), 'requested_s': 120,
        'observed_duration_s': duration, 'survived_requested_interval': survival,
        'client_alive_end': alive, 'client_exit_receipt': exit_receipt,
        'execution_error': failure, 'udp_tunnel': transport,
        'termination_cause': (transport.get('first_measured_terminal') or {}).get('kind'),
        'attempt_receipt': attempt, 'trial_result_receipt': trial_result,
        'capture_health': capture_health, 'capture_status': capture_status(capture_health, capture_finalization),
        'visual_progress': progress, 'metrics': measured['metrics'],
        'counter_samples': measured['counter_samples'], 'counter_errors': measured['counter_errors'],
        'cross_boundary_samples': measured['cross_boundary_samples'],
        'counter_error_events': measured['counter_error_events'],
        'counter_error_event_count': measured['counter_error_event_count'],
        'raw_provider_error_event_count': counters['raw_provider_error_event_count'],
        'all_provider_error_events_with_phase': measured['all_provider_error_events_with_phase'],
        'clock_alignment': alignment, 'network': network(step),
        'source_build_receipt': source, 'source_identity': source_identity,
        'source_comparison_eligible': source_identity['commit_matches'] is True and source_identity['binary_matches'] is True,
        'trial_provenance': provenance,
        'counter_report': counters, 'raw_campaign': raw,
        'artifacts': artifacts, 'artifact_issues': issues, **UNKNOWN_MEASUREMENTS,
    }


def paired(trials):
    result = []
    for loss in (0, 25, 50, 70):
        a = next(t for t in trials if t['engine'] == 'local' and t['loss_percent'] == loss)
        b = next(t for t in trials if t['engine'] == 'pr' and t['loss_percent'] == loss)
        deltas = {}
        for metric in a['metrics']:
            x = a['metrics'][metric]['mean'] if a['source_comparison_eligible'] else None
            y = b['metrics'][metric]['mean'] if b['source_comparison_eligible'] else None
            deltas[metric] = {'local_mean': x, 'local_n': a['metrics'][metric]['n'] if a['source_comparison_eligible'] else 0,
                              'pr_mean': y, 'pr_n': b['metrics'][metric]['n'] if b['source_comparison_eligible'] else 0,
                              'diagnostic_local_n': a['metrics'][metric]['n'], 'diagnostic_pr_n': b['metrics'][metric]['n'],
                              'pr_minus_local': y - x if finite(x) and finite(y) else None}
        result.append({'loss_percent': loss, 'local_trial_id': a['trial_id'], 'pr_trial_id': b['trial_id'],
                       'local_survived_120s': a['survived_requested_interval'], 'pr_survived_120s': b['survived_requested_interval'],
                       'metrics': deltas, 'qualification': 'Single-trial descriptive comparison; short failed-trial means cover a different duration and may be survivor-biased.'})
    return result


def recommendation(trials):
    if any(t['capture_status'].get('performance_confounded') for t in trials):
        return ('Capture tasks remained active across later trials, so changing capture overhead confounds these performance values. '
                'Preserve this entire first campaign as exploratory evidence, including the PR70 main-transport timeout. '
                'Use the complete separately labelled no-capture campaign for the client comparison; do not pool the campaigns or selectively replace individual trials.')
    advantages = {'local': [], 'pr': []}
    for loss in (0, 25, 50, 70):
        a = next(t for t in trials if t['engine'] == 'local' and t['loss_percent'] == loss)
        b = next(t for t in trials if t['engine'] == 'pr' and t['loss_percent'] == loss)
        if not (a['source_comparison_eligible'] and b['source_comparison_eligible']):
            continue
        if a['survived_requested_interval'] is True and b['survived_requested_interval'] is False:
            advantages['local'].append(loss)
        if b['survived_requested_interval'] is True and a['survived_requested_interval'] is False:
            advantages['pr'].append(loss)
    findings = []
    for engine, losses in advantages.items():
        if losses:
            findings.append(f"{engine} survived 120 seconds at loss levels {losses} where the paired client exited early.")
    return (' '.join(findings) + ' ' if findings else '') + (
        'Use these matched trials to assess Windows-downlink RDP survival and sampled graphics progress. '
        'Do not transfer an FPS advantage to CRC goodput or latency-tail claims. '
        'Any apparent winner from one unseeded connection per condition warrants repeated matched trials before changing production policy.')


def write(prefix, report):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix('.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    fields = ['trial_id', 'engine', 'loss_percent', 'outcome', 'observed_duration_s', 'survived_requested_interval',
              'termination_cause', 'udp_status', 'capture_status', 'kernel_drops', 'collector_terminal_receipt_present',
              'visual_status', 'visual_samples', 'visual_raw_samples', 'visual_changed_intervals', 'visual_substantial_intervals',
              'visual_terminal_or_unassigned_samples',
              'visual_missed_samples', 'counter_error_event_count', 'raw_provider_error_event_count',
              'artifact_issue_count', 'small_drop_fraction', 'large_drop_fraction',
              *UNKNOWN_MEASUREMENTS]
    for name in report['trials'][0]['metrics']:
        fields += [name + '_' + stat for stat in ('mean', 'median', 'min', 'max', 'n')]
    with prefix.with_suffix('.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator='\n'); writer.writeheader()
        for trial in report['trials']:
            image = (trial['visual_progress'].get('before_logged_terminal_progress')
                     or trial['visual_progress'].get('actual_phase_progress') or {})
            raw_image = trial['visual_progress'].get('summary') or {}
            row = {k: trial.get(k) for k in fields}
            row.update(udp_status=trial['udp_tunnel']['status'], visual_status=trial['visual_progress']['status'],
                       capture_status=trial['capture_status']['status'], kernel_drops=trial['capture_status']['kernel_drops'],
                       collector_terminal_receipt_present=trial['counter_report']['collector_terminal_receipt_present'],
                       visual_samples=image.get('samples'), visual_changed_intervals=image.get('changed_intervals'),
                       visual_raw_samples=raw_image.get('samples'),
                       visual_terminal_or_unassigned_samples=image.get('samples_at_or_after_terminal_or_without_utc'),
                       visual_substantial_intervals=image.get('substantial_change_intervals'), visual_missed_samples=raw_image.get('missed_samples'),
                       artifact_issue_count=len(trial['artifact_issues']),
                       small_drop_fraction=trial['network']['small']['drop_fraction_completed_attempts'],
                       large_drop_fraction=trial['network']['large']['drop_fraction_completed_attempts'])
            for name, values in trial['metrics'].items():
                row.update({name + '_' + stat: values[stat] for stat in ('mean', 'median', 'min', 'max', 'n')})
            writer.writerow(row)
    def fmt(value):
        return 'unknown' if value is None else f'{value:.3f}' if isinstance(value, float) else str(value)
    def metric(trial, name):
        data = trial['metrics'][name]
        return f"{fmt(data['mean'])} ({data['n']})"
    lines = ['# Windows VM comparison', '',
             'One trial per condition; server counters and sampled content progress. Verified goodput and latency tails remain unavailable.', '',
             '| Client | Loss | Measured s | Outcome | UDP evidence | Server FPS mean (N) | Network-skipped FPS mean (N) | Visual evidence |',
             '|---|---:|---:|---|---|---:|---:|---|']
    for trial in report['trials']:
        lines.append(f"| {trial['engine']} | {trial['loss_percent']}% | {fmt(trial['observed_duration_s'])} | {trial['outcome']} | {trial['udp_tunnel']['status']} | {metric(trial, 'server_output_fps')} | {metric(trial, 'server_skipped_network_fps')} | {trial['visual_progress']['status']} |")
    lines += ['', '| Client | Loss | Small completed-drop fraction | Large completed-drop fraction | Counter error events |',
              '|---|---:|---:|---:|---:|']
    for trial in report['trials']:
        lines.append(f"| {trial['engine']} | {trial['loss_percent']}% | {fmt(trial['network']['small']['drop_fraction_completed_attempts'])} | {fmt(trial['network']['large']['drop_fraction_completed_attempts'])} | {fmt(trial['counter_error_event_count'])} |")
    capture_line = ('Packet capture was disabled; no capture kernel-drop metric is available.' if report['packet_capture_mode'] == 'disabled'
                    else f"Capture health: {report['audit_summary']['capture_invalid_trials']} invalid, {report['audit_summary']['capture_unknown_trials']} unknown trials. Recorded kernel drops cover whole overlapping capture lifetimes, not measured phases.")
    lines += ['', report['recommendation'], '', *['- ' + note for note in report['notes']], '',
              capture_line + f" PDH collector-end receipt absent in {report['audit_summary']['collector_terminal_missing_trials']} trials.", '',
              f"Provider error events: {report['audit_summary']['raw_provider_error_events']} in the whole raw collection, including {report['audit_summary']['after_phase_provider_error_events']} after measurement; {report['audit_summary']['counter_error_events']} inside nominal measured intervals.", '',
              'All planned conditions, counter/observer errors, clock qualifications and raw references are retained in JSON; CSV blanks represent unknown values.', '']
    prefix.with_suffix('.md').write_text('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--index', type=Path, default=BASE / 'campaign/trial-index.json')
    parser.add_argument('--output-prefix', type=Path, default=BASE / 'comparison/vm-pr-comparison-20261014')
    parser.add_argument('--campaign-complete', action='store_true', required=True,
                        help='explicit coordinator gate: all timed trials have ended')
    parser.add_argument('--capture-mode', choices=('requested', 'disabled'), default='requested',
                        help='coordinator declaration; disabled requires no PCAP/capture receipts')
    args = parser.parse_args()
    index = json.loads(args.index.read_text(encoding='utf-8-sig'))
    trials = index.get('trials')
    if not isinstance(trials, list) or len(trials) != 8:
        raise ValueError('index must retain all eight planned conditions')
    keys = {(r.get('engine'), r.get('loss_percent')) for r in trials}
    ids = [r.get('trial_id') for r in trials]
    if keys != CONDITIONS or len(set(ids)) != 8 or any(not isinstance(i, str) or not i for i in ids):
        raise ValueError('index conditions or trial identities differ from planned matrix')
    for number, trial in enumerate(trials, 1):
        if isinstance(trial['loss_percent'], bool) or not isinstance(trial['loss_percent'], int):
            raise ValueError('integer loss percentages required')
        trial['order'] = number
        path = Path(trial['path'])
        path = path if path.is_absolute() else args.index.parent / path
        trial['_directory'] = path.resolve()
        if not trial['_directory'].is_relative_to(BASE.resolve()):
            raise ValueError('trial directory outside comparison workspace')
    issues = []
    execution = read(args.index.parent / 'campaign.json', issues, required=True)
    if not execution or not isinstance(execution.get('finished_utc'), str):
        raise ValueError('closed coordinator campaign receipt required before analysis')
    run_configuration = read(args.index.parent / 'run-configuration.json', issues, required=True)
    if not run_configuration or run_configuration.get('packet_capture_enabled') is not (args.capture_mode == 'requested'):
        raise ValueError('capture mode must match explicit coordinator run-configuration receipt')
    setup = read(BASE / 'linux-setup.json', issues, required=True)
    if not setup or setup.get('outcome') != 'COMPLETE':
        raise ValueError('completed production build provenance required')
    spec = importlib.util.spec_from_file_location('comparison_upstream_method', REPORTER)
    reporter = importlib.util.module_from_spec(spec); spec.loader.exec_module(reporter)
    analyzed = [analyze_trial(trial, setup, reporter) for trial in trials]
    if args.capture_mode == 'disabled':
        for trial in analyzed:
            trial['capture_status'] = disabled_capture_status(trial)
    result = {'schema': 'freerdp.windows_vm_pr_comparison', 'version': 1,
              'interpretation_revision': 'v5-raw-provider-errors-versus-admitted-phase-errors',
              'campaign_label': args.index.parent.name, 'packet_capture_mode': args.capture_mode,
              'run_configuration': run_configuration,
              'performance_qualification': 'CAPTURE_CONFOUNDED_EXPLORATORY' if any(t['capture_status'].get('performance_confounded') for t in analyzed) else 'SINGLE_TRIAL_EXPLORATORY',
              'generated_utc': datetime.now(timezone.utc).isoformat(), 'planned_trials': 8,
              'reporter': {'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                           'upstream_reporter_sha256': hashlib.sha256(REPORTER.read_bytes()).hexdigest(),
                           'index_sha256': hashlib.sha256(args.index.read_bytes()).hexdigest()},
              'campaign_execution': {k: execution.get(k) for k in (
                  'schema', 'version', 'started_utc', 'finished_utc', 'coordinator_sha256',
                  'settings_sha256', 'scope', 'warmup_seconds', 'measurement_seconds',
                  'impairment', 'evidence', 'completion_semantics', 'stopped_early_reason')},
              'source_builds': setup, 'upstream_method_source': read(BASE / 'upstream-source.json', issues),
              'published_claim_context': {
                  'reported_by': 'Ceschi-ops', 'date_utc': '2026-10-12', 'tested_commit': 'd148cdb76',
                  'source_url': 'https://github.com/FreeRDP/FreeRDP/pull/13608#issuecomment-6131161999',
                  'server_output_fps_ranges': {'50_percent': [19, 21], '70_percent': [18, 19]},
                  'qualification': 'Windows-downlink graphics counters in another environment/revision; no 25-percent numerical claim found in the reviewed comments.'},
              'vm_identity_receipt': read(BASE / 'vm-preflight-identity.json', issues),
              'workload': {'bitmap': [240, 135], 'nominal_timer_ms': 33, 'fullscreen': True},
              'session': {'width': 1280, 'height': 800, 'bpp': 32, 'gfx': 'progressive; AVC disabled',
                          'timeout_ms': 20000, 'warmup_requested_s': 45, 'measured_requested_s': 120},
              'trials': analyzed, 'paired_conditions': paired(analyzed), 'artifact_issues': issues,
              'audit_summary': {
                  'survived_120s': sum(t['survived_requested_interval'] is True for t in analyzed),
                  'session_ended_during_measurement': sum(t['outcome'] == 'SESSION_ENDED' for t in analyzed),
                  'measured_udp_failure_logs': sum(t['udp_tunnel']['measured_udp_failure_logged'] is True for t in analyzed),
                  'intentional_planned_client_stops': sum((t['client_exit_receipt'] or {}).get('planned_stop_after_measurement') is True for t in analyzed),
                  'capture_unknown_trials': sum(t['capture_status']['status'] == 'UNKNOWN' for t in analyzed),
                  'capture_invalid_trials': sum(t['capture_status']['status'] == 'INVALID' for t in analyzed),
                  'capture_confounded_trials': sum(t['capture_status'].get('performance_confounded') is True for t in analyzed),
                  'capture_lifecycle_error_entries': sum(len(t['capture_status']['errors']) for t in analyzed),
                  'collector_terminal_missing_trials': sum(not t['counter_report']['collector_terminal_receipt_present'] for t in analyzed),
                  'counter_error_events': sum(t['counter_error_event_count'] for t in analyzed if count(t['counter_error_event_count'])),
                  'counter_error_event_scope': 'nominal measured Windows intervals only; not a whole-query zero-error claim',
                  'raw_provider_error_events': sum(t['raw_provider_error_event_count'] for t in analyzed if count(t['raw_provider_error_event_count'])),
                  'after_phase_provider_error_events': sum(event['phase'] == 'AFTER_MEASUREMENT'
                                                          for t in analyzed for event in t['all_provider_error_events_with_phase']),
              },
              'recommendation': recommendation(analyzed),
              'notes': NOTES}
    write(args.output_prefix, result)
    print(json.dumps({'outputs': [str(args.output_prefix.with_suffix(suffix)) for suffix in ('.json', '.csv', '.md')],
                      'conditions': len(analyzed), 'survived_120s': sum(t['survived_requested_interval'] is True for t in analyzed),
                      'artifact_issue_count': len(issues) + sum(len(t['artifact_issues']) for t in analyzed)}, indent=2))


if __name__ == '__main__':
    main()
