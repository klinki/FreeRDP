#!/usr/bin/env python3
"""Run the tracked ingress step and a separate sampled-image observer.

The first injected monotonic clock read is the existing ramp's measurement
boundary after tc change/snapshot. No native code or ramp policy is replaced.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from types import SimpleNamespace


def save(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def stop_owned(receipt, owner, sig):
    record = json.loads(Path(receipt).read_text())
    if record.get('owner') != owner:
        raise ValueError('image process ownership differs')
    pid = record['pid']
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    except FileNotFoundError:
        return dict(outcome='ALREADY_EXITED', pid=pid)
    if int(fields[19]) != record['start_ticks']:
        raise ValueError('image process birth-time differs; refusing to signal')
    if fields[0] in ('Z', 'X'):
        return dict(outcome='ALREADY_EXITED', pid=pid)
    if str(Path(f'/proc/{pid}/exe').resolve()) != record['executable']:
        raise ValueError('image process executable differs; refusing to signal')
    os.kill(pid, sig)
    return dict(outcome='SIGNALED_OWNED_PROCESS', pid=pid, signal=int(sig))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ramp', type=Path, required=True)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--provenance', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--loss', type=float, required=True)
    parser.add_argument('--seconds', type=float, default=120)
    parser.add_argument('--image-script', type=Path, default=Path('/bench/image-progress.py'))
    parser.add_argument('--owned-exec', type=Path, default=Path('/bench/owned-exec.py'))
    parser.add_argument('--display', default=':99')
    parser.add_argument('--region', type=int, nargs=4, default=[448, 280, 384, 240])
    parser.add_argument('--save-ppm', action='store_true')
    args = parser.parse_args()
    parent = args.output.parent
    spec = importlib.util.spec_from_file_location('comparison_ramp', args.ramp)
    ramp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ramp)
    observer = log = None
    observer_receipt = parent / 'image-owned.json'
    observer_status = dict(scope='1 Hz sampled visual progress, not FPS/input latency/goodput',
                           requested_seconds=args.seconds, region=args.region, display=args.display,
                           outcome='NOT_STARTED')
    started = False

    def clock():
        nonlocal started, observer, log
        now = time.monotonic_ns()
        if not started:
            started = True
            save(parent / 'measurement-boundary.json',
                 dict(linux_monotonic_ns=now, linux_unix_s=time.time(),
                      utc=datetime.now(timezone.utc).isoformat(), loss_percent=args.loss,
                      requested_seconds=args.seconds,
                      definition='tracked ramp start after loss change and before-snapshot'))
            try:
                if not args.image_script.is_file():
                    raise ValueError('image observer script unavailable')
                log = (parent / 'image-console.log').open('xb')
                command = [sys.executable, str(args.owned_exec), '--owner', args.owner,
                           '--receipt', str(observer_receipt), '--executable', sys.executable,
                           '--', str(args.image_script), '--output', str(parent / 'image-progress.jsonl'),
                           '--seconds', str(args.seconds), '--interval', '1', '--display', args.display,
                           '--start-ns', str(now), '--region', *map(str, args.region)]
                if args.save_ppm:
                    command.append('--save-ppm')
                observer = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                observer_status.update(outcome='STARTED', command=command,
                                       supervisor_pid=observer.pid, scheduled_start_ns=now)
            except Exception as error:
                observer_status.update(outcome='OBSERVER_START_FAILED', error=repr(error))
            save(parent / 'image-observer-status.json', observer_status)
        return now / 1e9

    result = None
    try:
        result = ramp.run(SimpleNamespace(state=args.state, pid=args.pid,
                                         provenance=args.provenance, output=args.output,
                                         loss=[args.loss], seconds=args.seconds), clock=clock)
    finally:
        if observer:
            if result and all(step['outcome'] == 'COMPLETE' for step in result['steps']):
                # Allow the exact-end observer footer before considering an early stop.
                try:
                    observer.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            if observer.poll() is None:
                try:
                    observer_status['stop_receipt'] = stop_owned(observer_receipt, args.owner, signal.SIGTERM)
                except Exception as error:
                    observer_status['stop_error'] = repr(error)
            try:
                observer_status['exit_code'] = observer.wait(timeout=15)
            except subprocess.TimeoutExpired:
                try:
                    observer_status['kill_receipt'] = stop_owned(observer_receipt, args.owner, signal.SIGKILL)
                    observer_status['exit_code'] = observer.wait(timeout=5)
                except Exception as error:
                    observer_status['kill_error'] = repr(error)
            observer_status['ended_utc'] = datetime.now(timezone.utc).isoformat()
        if log:
            log.close()
        save(parent / 'image-observer-status.json', observer_status)
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result and all(s['outcome'] == 'COMPLETE' for s in result['steps']) and result['cleanup']['outcome'] == 'LOSS_RESET_TO_ZERO' else 1


if __name__ == '__main__':
    raise SystemExit(main())
