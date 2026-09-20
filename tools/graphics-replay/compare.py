#!/usr/bin/env python3
"""Run sequential, alternating A/B trials against one extracted graphics stream."""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def counters(path, result):
    monitors = defaultdict(lambda: defaultdict(int))
    for line in path.read_text().splitlines():
        row = json.loads(line)
        if row['schema'] != 'freerdp.sdl_render_metrics':
            raise ValueError('Unexpected metrics schema')
        # The runner flushes at both boundaries, so no interval straddles them.
        if row['interval_start_ns'] < result['measurement_start_ns']:
            continue
        if row['interval_start_ns'] + row['interval_duration_ns'] > result['measurement_end_ns']:
            raise ValueError('Metrics extend beyond the measurement window')
        for key in ('frames', 'uploaded_bytes', 'uploaded_pixels', 'upload_calls',
                    'upload_wall_ns', 'draw_calls', 'draw_wall_ns', 'present_calls', 'present_wall_ns'):
            monitors[str(row['monitor_id'])][key] += row[key]
    if not monitors or not sum(x['uploaded_bytes'] for x in monitors.values()):
        raise ValueError('No measured upload counters')
    return dict(monitors)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--optimized', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='new private results directory')
    parser.add_argument('--renderer', choices=['metal', 'software'], default='metal')
    parser.add_argument('--start-seconds', type=int, default=20)
    parser.add_argument('--stop-seconds', type=int, default=80)
    parser.add_argument('--repeats', type=int, default=2)
    args = parser.parse_args()
    if args.repeats < 1 or not 0 <= args.start_seconds < args.stop_seconds:
        parser.error('Positive repeats and 0 <= start < stop are required')
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (args.output / '.gitignore').write_text('*\n')
    digest = sha256(args.input)
    summary = {'version': 1, 'replay_sha256': digest, 'renderer': args.renderer,
               'start_seconds': args.start_seconds, 'stop_seconds': args.stop_seconds,
               'binaries': {}, 'runs': []}
    for label in ('baseline', 'optimized'):
        binary = getattr(args, label).resolve()
        manifest = binary.with_suffix('.build.json')
        summary['binaries'][label] = {'path': str(binary), 'sha256': sha256(binary),
                                     'build': json.loads(manifest.read_text()) if manifest.exists() else None}
    print(f'Results: {args.output.resolve()}', flush=True)
    signature = None
    for trial in range(1, args.repeats + 1):
        order = ('baseline', 'optimized') if trial % 2 else ('optimized', 'baseline')
        for label in order:
            prefix = args.output / f'{label}-{trial}'
            metrics = prefix.with_suffix('.jsonl')
            command = [str(getattr(args, label).resolve()), '--input', str(args.input.resolve()),
                       '--renderer', args.renderer, '--metrics', str(metrics.resolve()),
                       '--start-seconds', str(args.start_seconds), '--stop-seconds', str(args.stop_seconds)]
            print(f'{label} trial {trial}/{args.repeats}', flush=True)
            with prefix.with_suffix('.log').open('x') as log:
                process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if process.returncode:
                raise RuntimeError(f'{label} failed ({process.returncode}); see {prefix}.log')
            results = [json.loads(line) for line in prefix.with_suffix('.log').read_text().splitlines()
                       if line.startswith('{"schema":"freerdp.graphics_replay_result"')]
            if len(results) != 1:
                raise ValueError(f'Expected one result in {prefix}.log')
            result = results[0]
            if (result['verification_enabled'] or result['decode_errors'] or result['paced'] or
                    result.get('visible') or result.get('cancelled')):
                raise ValueError('Invalid benchmark mode or decoder errors')
            current = {k: result[k] for k in ('renderer', 'messages', 'measured_messages',
                       'start_capture_us', 'last_capture_us', 'paint_callbacks', 'measured_paints', 'gfx_stats')}
            if signature is not None and signature != current:
                raise ValueError('A/B replay workloads differ')
            signature = current
            result['monitor_counters'] = counters(metrics, result)
            result['label'], result['trial'] = label, trial
            summary['runs'].append(result)
            (args.output / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
            print(f"  render {result['render_wall_s']:.3f}s; process CPU {result['process_cpu_s']:.3f}s", flush=True)
    if sha256(args.input) != digest:
        raise ValueError('Replay file changed during comparison')
    for label in ('baseline', 'optimized'):
        if sha256(getattr(args, label)) != summary['binaries'][label]['sha256']:
            raise ValueError('Replay binary changed during comparison')
    lines = ['# Offline graphics replay comparison', '',
             f'Replay SHA-256: `{digest}`', '',
             f'Renderer: {args.renderer}. Recorded interval: {args.start_seconds}–{args.stop_seconds}s. '
             f'{args.repeats} sequential trials per build, alternating order. Prefix decoded before measurement.', '',
             '| Measurement (median across trials) | Baseline | Optimized | Reduction |',
             '|---|---:|---:|---:|']
    fields = [('Decode + rendering wall, s', lambda r: r['decode_and_render_wall_s']),
              ('Process CPU, s', lambda r: r['process_cpu_s']),
              ('Rendering wall, s', lambda r: r['render_wall_s']),
              ('Rendering p95, ms', lambda r: r['render_p95_ms']),
              ('Total texture upload, decimal GB', lambda r: sum(m['uploaded_bytes'] for m in r['monitor_counters'].values()) / 1e9)]
    for monitor in sorted(summary['runs'][0]['monitor_counters']):
        fields.append((f'Monitor {monitor} texture upload, decimal GB',
                       lambda r, monitor=monitor: r['monitor_counters'].get(monitor, {}).get('uploaded_bytes', 0) / 1e9))
    for name, measure in fields:
        values = [statistics.median(measure(r) for r in summary['runs'] if r['label'] == label)
                  for label in ('baseline', 'optimized')]
        reduction = f'{(1-values[1]/values[0])*100:.1f}%' if values[0] else 'n/a'
        lines.append(f'| {name} | {values[0]:.3f} | {values[1]:.3f} | {reduction} |')
    lines += ['', 'These are local decoder/render measurements, not network traffic. Hidden windows '
              'and synchronous painting do not reproduce onscreen presentation, live UI queue coalescing, '
              'or input latency. Verification/readback is disabled during these timed trials. '
              'Run separate pixel verification before interpreting a changed renderer.', '']
    report = '\n'.join(lines)
    (args.output / 'comparison.md').write_text(report)
    print(report)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        sys.exit(f'Comparison failed: {error}')
