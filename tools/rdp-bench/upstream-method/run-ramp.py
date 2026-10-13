#!/usr/bin/env python3
"""Record a bounded Linux ingress loss ramp against an already-running client.

The caller owns VM/load/counter setup. This script changes only the network
state checked by its sibling linux-ingress.py and never terminates the client.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time


class OwnershipError(RuntimeError):
    pass


def save(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def utc():
    return datetime.now(timezone.utc).isoformat()


def process_status(pid):
    """kill(0) establishes existence; /proc additionally detects zombies/reuse."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return {'alive': False, 'exit_code': None, 'start_ticks': None}
    except PermissionError as error:
        raise RuntimeError('client process visibility denied') from error
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    except FileNotFoundError:
        return {'alive': False, 'exit_code': None, 'start_ticks': None}
    zombie = fields[0] in ('Z', 'X')
    exit_code = os.waitstatus_to_exitcode(int(fields[49])) if zombie and len(fields) > 49 else None
    return {'alive': not zombie, 'exit_code': exit_code, 'start_ticks': int(fields[19])}


class Controller:
    def __init__(self, state):
        self.state = Path(state).resolve(strict=True)
        self.script = Path(__file__).resolve().with_name('linux-ingress.py')
        if not self.script.is_file():
            raise ValueError('missing sibling linux-ingress.py')
        self.identity = None

    def call(self, operation, output, loss=None):
        if self.identity is not None:
            current = json.loads(self.state.read_text())
            identity = (current['owner'], current['netns'], current['interface'],
                        current['ingress'], current['receive'])
            if identity != self.identity:
                raise OwnershipError('network state identity changed before controller action')
        args = [sys.executable, str(self.script), '--state', str(self.state),
                '--output', str(output), operation]
        if loss is not None:
            args += ['--loss', str(loss)]
        result = subprocess.run(args, text=True, capture_output=True, check=False)
        if result.returncode:
            reason = result.stderr.strip() or result.stdout.strip()
            if any(text in reason for text in ('ownership alias', 'different network namespace',
                                               'MAC no longer matches', 'must not own an IPv4')):
                raise OwnershipError(reason)
            raise RuntimeError(reason or f'network controller exited {result.returncode}')
        snapshot = json.loads(Path(output).read_text())
        state = snapshot['state']
        identity = (state['owner'], state['netns'], state['interface'], state['ingress'], state['receive'])
        if self.identity is None:
            self.identity = identity
        elif identity != self.identity:
            raise OwnershipError('network state identity changed during ramp')
        return snapshot


def run(args, *, controller=None, probe=process_status, clock=time.monotonic, sleep=time.sleep):
    if args.pid <= 0 or not math.isfinite(args.seconds) or args.seconds <= 0:
        raise ValueError('positive client PID and finite step duration required')
    if not args.loss or any(not math.isfinite(x) or not 0 <= x <= 100 for x in args.loss):
        raise ValueError('loss levels must be percentages from 0 through 100')
    if len(set(args.loss)) != len(args.loss):
        raise ValueError('loss levels must be distinct')
    provenance = json.loads(Path(args.provenance).read_text())
    if not isinstance(provenance, dict):
        raise ValueError('provenance must be a JSON object')
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    controller = controller or Controller(args.state)
    script = Path(__file__)
    campaign = {'schema_version': 1, 'started_utc': utc(), 'client_pid': args.pid,
                'provenance': provenance,
                'runner_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
                'method_fidelity': 'Linux ingress tc/netem via owned veth; adaptation of upstream IFB method',
                'method_deviations': ['veth replaces IFB', 'upstream small/large ICMP preflight omitted'],
                'scope': 'all IPv4 arriving from VM; no rate cap; server-counter method, not verified goodput',
                'verified_goodput_mbps': None,
                'steps': [{'id': f'loss{loss:g}', 'loss_percent': loss,
                           'requested_s': args.seconds, 'outcome': 'NOT_RUN',
                           'client_exit_code': None} for loss in args.loss],
                'cleanup': None}
    path = output / 'campaign.json'
    save(path, campaign)
    current = None
    ownership_failed = False
    initial = None
    try:
        controller.call('snapshot', output / 'network-initial.json')
        initial = probe(args.pid)
        campaign['client_initial_status'] = initial
        if provenance.get('client_pid') != args.pid or isinstance(provenance.get('client_pid'), bool):
            raise ValueError('provenance must record the requested client_pid')
        recorded_start = provenance.get('client_start_ticks')
        if isinstance(recorded_start, bool) or not isinstance(recorded_start, int) or recorded_start <= 0:
            raise ValueError('provenance must record positive client_start_ticks from /proc/PID/stat')
        if initial.get('start_ticks') != recorded_start:
            raise ValueError('client PID identity differs from recorded start ticks before loss ramp')
        for step in campaign['steps']:
            current = step
            if not initial['alive']:
                step.update(outcome='SESSION_ENDED', client_alive_end=False,
                            client_exit_code=initial.get('exit_code'))
                break
            prefix = output / step['id']
            step['outcome'] = 'FAILED_SETUP'
            controller.call('change', prefix.with_suffix('.change.json'), step['loss_percent'])
            before = controller.call('snapshot', prefix.with_suffix('.before.json'))
            step['network'] = {'qdisc_before': before, 'ping_verification':
                               {'outcome': 'NOT_PERFORMED', 'reason': 'recorded method deviation'}}
            start = clock()
            step.update(start_utc=utc(), start_monotonic_s=start, outcome='INCOMPLETE')
            save(path, campaign)
            deadline = start + args.seconds
            while True:
                status = probe(args.pid)
                same_process = status.get('start_ticks') == initial.get('start_ticks')
                if not status['alive'] or not same_process:
                    # A vanished /proc entry proves termination, not PID reuse.
                    # Only a live replacement with a recorded different birth
                    # time establishes that the PID now identifies another process.
                    identity_changed = (status['alive'] and status.get('start_ticks') is not None
                                        and not same_process)
                    step.update(outcome='SESSION_ENDED', client_alive_end=False,
                                client_exit_code=status.get('exit_code') if same_process else None,
                                client_identity_changed=identity_changed)
                    break
                if clock() >= deadline:
                    step.update(outcome='COMPLETE', client_alive_end=True)
                    break
                sleep(min(1.0, max(0.0, deadline - clock())))
            step.update(end_utc=utc(), end_monotonic_s=clock())
            step['observed_duration_s'] = step['end_monotonic_s'] - start
            step['network']['qdisc_after'] = controller.call('snapshot', prefix.with_suffix('.after.json'))
            save(path, campaign)
            if step['outcome'] != 'COMPLETE':
                break
    except (Exception, KeyboardInterrupt) as error:
        ownership_failed = isinstance(error, OwnershipError)
        campaign['error'] = repr(error)
        campaign['interrupted'] = isinstance(error, KeyboardInterrupt)
        if current:
            current['error'] = repr(error)
            if current.get('start_monotonic_s') is not None:
                current.update(end_utc=utc(), end_monotonic_s=clock(), outcome='INCOMPLETE')
                current['observed_duration_s'] = current['end_monotonic_s'] - current['start_monotonic_s']
                if not ownership_failed:
                    try:
                        current.setdefault('network', {})['qdisc_after'] = controller.call(
                            'snapshot', output / (current['id'] + '.interrupted-after.json'))
                    except Exception as snapshot_error:
                        current['snapshot_error'] = repr(snapshot_error)
                        ownership_failed = isinstance(snapshot_error, OwnershipError)
    finally:
        if ownership_failed:
            campaign['cleanup'] = {'outcome': 'NOT_ATTEMPTED', 'reason': 'network ownership validation failed'}
        else:
            try:
                receipt = controller.call('change', output / 'network-reset-zero.json', 0)
                campaign['cleanup'] = {'outcome': 'LOSS_RESET_TO_ZERO', 'receipt': receipt,
                                       'scope': 'veth/netem remains installed; delay/reorder retained'}
            except Exception as error:
                campaign['cleanup'] = {'outcome': 'FAILED', 'error': repr(error)}
        campaign['finished_utc'] = utc()
        save(path, campaign)
    return campaign


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--provenance', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--loss', type=float, nargs='+', default=[25, 50, 70])
    parser.add_argument('--seconds', type=float, default=120)
    args = parser.parse_args()
    campaign = run(args)
    print(json.dumps(campaign, indent=2, allow_nan=False))
    return 0 if all(step['outcome'] == 'COMPLETE' for step in campaign['steps']) and campaign['cleanup']['outcome'] == 'LOSS_RESET_TO_ZERO' else 1


if __name__ == '__main__':
    raise SystemExit(main())
