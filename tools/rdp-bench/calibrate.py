#!/usr/bin/env python3
"""Empirically calibrate the portable relay; never approve a lab receipt.

Independent raw UDP endpoints count and validate packets, timestamp reception,
and measure IPv4+UDP wire rate. These are HARNESS_CALIBRATION_ONLY results, not
FreeRDP results. The relay's own counters remain enabled in observer off/on runs.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import multiprocessing
import select
import socket
import statistics
import struct
import sys
import time
import uuid
import zlib
from pathlib import Path

from rdpbench.common import Journal, environment, seal, sha256, timing_summary, write_json
from rdpbench.link import Relay

HEADER = struct.Struct('!4sBBHQQQI')
PREFIX = struct.Struct('!4sBBHQQQ')
MAGIC = b'RC01'
BULK, PROBE, ECHO = 0, 1, 2


def _relay_worker(connection, left_port, right_port, profile, seed):
    """Keep endpoint Python work out of the production relay's GIL/process."""
    relay = None
    cpu_start = time.process_time()
    try:
        relay = Relay(left_port, right_port, profile, seed)
        connection.send({'ports': relay.ports})
        relay.start(connection.recv()['start_ns'])
        connection.recv()
        shape = relay.close()
        relay = None
        shape['calibration_relay_cpu_s'] = time.process_time() - cpu_start
        connection.send(shape)
    except BaseException as error:
        try:
            connection.send({'error': repr(error)})
        except (BrokenPipeError, EOFError, OSError):
            pass
    finally:
        if relay:
            relay.close()
        connection.close()


class IsolatedRelay:
    def __init__(self, left_port, right_port, profile, seed):
        context = multiprocessing.get_context('spawn')
        self.connection, child = context.Pipe()
        self.process = context.Process(target=_relay_worker, args=(child, left_port, right_port, profile, seed), daemon=True)
        self.process.start()
        child.close()
        if not self.connection.poll(10):
            self.process.terminate()
            self.process.join(5)
            self.connection.close()
            raise RuntimeError('calibration relay startup timed out')
        reply = self.connection.recv()
        if 'ports' not in reply:
            self.process.join(5)
            self.connection.close()
            raise RuntimeError('calibration relay startup failed: ' + str(reply))
        self.ports = reply['ports']

    def start(self, epoch_ns):
        self.connection.send({'start_ns': epoch_ns})

    def close(self):
        try:
            self.connection.send({'close': True})
            if not self.connection.poll(10):
                return {'error': 'calibration relay shutdown timed out'}
            return self.connection.recv()
        except (BrokenPipeError, EOFError, OSError) as error:
            return {'error': repr(error)}
        finally:
            self.connection.close()
            self.process.join(5)
            if self.process.is_alive():
                self.process.terminate()
                self.process.join(5)


def payload(seed, size):
    """A known seed-specific payload; metadata and body are protected by CRC."""
    if size < HEADER.size:
        raise ValueError('datagram smaller than calibration header')
    block = hashlib.sha256(str(seed).encode()).digest()
    return (block * ((size - HEADER.size + len(block) - 1) // len(block)))[:size - HEADER.size]


def packet(kind, direction, seq, scheduled_ns, sent_ns, body):
    prefix = PREFIX.pack(MAGIC, kind, direction, 0, seq, scheduled_ns, sent_ns)
    return prefix + struct.pack('!I', zlib.crc32(prefix + body)) + body


def decode(data, seed):
    if len(data) < HEADER.size:
        raise ValueError('short calibration datagram')
    magic, kind, direction, reserved, seq, scheduled, sent, crc = HEADER.unpack_from(data)
    body = data[HEADER.size:]
    if magic != MAGIC or reserved or kind not in (BULK, PROBE, ECHO) or direction not in (0, 1):
        raise ValueError('invalid calibration metadata')
    if crc != zlib.crc32(data[:PREFIX.size] + body):
        raise ValueError('calibration CRC mismatch')
    if body != payload(seed, len(data)):
        raise ValueError('calibration payload mismatch')
    return kind, direction, seq, scheduled, sent, body


def seeded_loss(seed, direction, seq, probability):
    """Predict the documented portable model for an otherwise lossless arrival trace."""
    name = ('c2s', 's2c')[direction]
    directional_seed = seed + (1000003 if direction else 0)
    value = hashlib.blake2b(f'{directional_seed}:{name}:{seq + 1}:0'.encode(), digest_size=8).digest()
    return int.from_bytes(value, 'big') / 2**64 < probability


def peak_pps(timestamps, window_s):
    """Highest observed count in any half-open sliding window."""
    if not timestamps:
        return 0
    width = int(window_s * 1e9)
    left = best = 0
    for right, stamp in enumerate(timestamps):
        while timestamps[left] <= stamp - width:
            left += 1
        best = max(best, right - left + 1)
    return best / window_s


def receiver_rate_bounds(offered_wire_mbps, cap_mbps, loss, candidates, seconds, packet_wire_bytes):
    """Loss-aware raw UDP envelope, never a FreeRDP goodput certification.

    The loss draw is after blackout/queue admission. Use that offered cohort,
    not only surviving packets. A two-sided Hoeffding envelope with alpha .001
    bounds finite Bernoulli sampling; the existing 5% pacing envelope and two
    boundary datagrams remain separate. Zero/total loss have no random spread.
    Exact receiver/drop/seed accounting is checked independently by run_sample.
    """
    numbers = (offered_wire_mbps, cap_mbps, loss, seconds, packet_wire_bytes)
    if (any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v)
            for v in numbers) or offered_wire_mbps <= 0 or cap_mbps < 0 or
            not 0 <= loss <= 1 or seconds <= 0 or packet_wire_bytes <= 0 or
            isinstance(candidates, bool) or not isinstance(candidates, int) or candidates <= 0):
        return {'valid': False, 'reason': 'known positive offered cohort/rate required'}
    base = min(offered_wire_mbps, cap_mbps) if cap_mbps else offered_wire_mbps
    delta = math.sqrt(math.log(2 / .001) / (2 * candidates)) if 0 < loss < 1 else 0
    low_loss, high_loss = max(0, loss - delta), min(1, loss + delta)
    quantum = 2 * packet_wire_bytes * 8 / seconds / 1e6
    return {'valid': True, 'expected_wire_mbps': base * (1 - loss),
            'lower_wire_mbps': max(0, .95 * base * (1 - high_loss) - quantum),
            'upper_wire_mbps': 1.05 * base * (1 - low_loss) + quantum,
            'base_wire_mbps': base, 'offered_wire_mbps': offered_wire_mbps,
            'loss_probability': loss, 'loss_draw_candidates': candidates,
            'finite_cohort_probability_delta': delta, 'sampling_alpha': .001,
            'pacing_tolerance_fraction': .05, 'boundary_datagram_allowance': 2,
            'counting_layer': 'UDP datagram + 28 IPv4/UDP bytes',
            'scope': 'raw UDP network/harness envelope; no ARQ or application goodput'}


def run_sample(profile, profile_id, seed, mode, direction, seconds, output,
               packet_size=1232, offered_mbps=9, target_pps=6000, probe_hz=60,
               observer=True, drain_s=.25, burst_factor=4):
    """One physical-socket sample. No native FreeRDP driver is involved."""
    folder = Path(output).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    journal = Journal(folder / 'observer.jsonl')
    sockets = []
    relay = None
    counts = collections.Counter()
    traces = []
    one_way = []
    rtts = []
    deadline_lags = []
    sent_times = []
    received_times = []
    offered = {}
    observed = set()
    replies = set()
    seen = set()
    errors = []
    cpu_start = time.process_time()
    shape = {}
    start = stop = end = 0
    try:
        for _ in range(2):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(('127.0.0.1', 0))
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            sock.setblocking(False)
            sockets.append(sock)
        relay = IsolatedRelay(sockets[0].getsockname()[1], sockets[1].getsockname()[1], profile, seed)
        destinations = [('127.0.0.1', port) for port in relay.ports]
        start = time.monotonic_ns() + 20_000_000
        stop = start + int(seconds * 1e9)
        end = stop + int(drain_s * 1e9)
        relay.start(start)
        next_send = start
        next_sample = start
        next_observation = start
        seq = 0
        cache = {n: payload(seed, n) for n in (packet_size, 64, 100)}
        while time.monotonic_ns() < end:
            now = time.monotonic_ns()
            # Bounded catch-up keeps receiver work fair, and any pacing deficit is visible.
            for _ in range(64):
                now = time.monotonic_ns()
                if now < next_send or now >= stop:
                    break
                size = 100 if mode == 'probe' else (64 if mode == 'headroom' and seq % 2 else packet_size)
                kind = PROBE if mode == 'probe' else BULK
                injected = time.monotonic_ns()
                data = packet(kind, direction, seq, next_send, injected, cache[size])
                try:
                    sockets[direction].sendto(data, destinations[direction])
                    offered[seq] = (injected, size)
                    counts['offered_datagrams'] += 1
                    counts['offered_wire_bytes'] += size + 28
                    sent_times.append(injected)
                    traces.append(('send', injected, seq, kind, direction, size, injected, next_send))
                    deadline_lags.append(max(0, injected - next_send) / 1e6)
                    if observer and kind == PROBE:
                        journal.emit('probe_sent', id=seq, scheduled_ns=next_send, sent_ns=injected)
                except (BlockingIOError, OSError) as error:
                    counts['socket_send_errors'] += 1
                    errors.append(repr(error))
                seq += 1
                if mode == 'probe':
                    interval = 1e9 / probe_hz
                elif mode == 'headroom':
                    # 10-ms, 4x-rate bursts each 250 ms, on top of sustained target PPS.
                    burst = ((next_send - start) % 250_000_000) < 10_000_000
                    interval = 1e9 / (target_pps * (burst_factor if burst else 1))
                else:
                    interval = (size + 28) * 8 * 1000 / offered_mbps
                next_send += max(1, int(interval))
            now = time.monotonic_ns()
            deadline = min(end, next_send if now < stop else end)
            readable, _, _ = select.select(sockets, [], [], min(.005, max(0, deadline - now) / 1e9))
            for sock in readable:
                side = sockets.index(sock)
                for _ in range(128):
                    try:
                        data, source = sock.recvfrom(65535)
                    except BlockingIOError:
                        break
                    received = time.monotonic_ns()
                    if source != destinations[side]:
                        counts['unexpected_source'] += 1
                        continue
                    try:
                        kind, origin, index, scheduled, injected, body = decode(data, seed)
                    except ValueError as error:
                        counts['corrupt_datagrams'] += 1
                        errors.append(str(error))
                        continue
                    key = (kind, origin, index)
                    if key in seen:
                        counts['duplicate_datagrams'] += 1
                        continue
                    seen.add(key)
                    traces.append(('receive', received, index, kind, origin, len(data), injected, scheduled))
                    if origin != direction or index not in offered or offered[index] != (injected, len(data)):
                        counts['unexpected_datagrams'] += 1
                        continue
                    if kind in (BULK, PROBE) and side == 1 - direction:
                        observed.add(index)
                        counts['received_datagrams'] += 1
                        one_way.append((received - injected) / 1e6)
                        if start <= received < stop:
                            counts['received_measure_datagrams'] += 1
                            counts['received_measure_wire_bytes'] += len(data) + 28
                            received_times.append(received)
                        if kind == PROBE:
                            response = packet(ECHO, origin, index, scheduled, injected, body)
                            try:
                                sock.sendto(response, destinations[side])
                                counts['echoes_sent'] += 1
                            except (BlockingIOError, OSError) as error:
                                counts['socket_send_errors'] += 1
                                errors.append(repr(error))
                    elif kind == ECHO and side == direction and mode == 'probe':
                        replies.add(index)
                        rtts.append((received - injected) / 1e6)
                        counts['echoes_received'] += 1
                    else:
                        counts['unexpected_datagrams'] += 1
                    # Comparable to native 60-Hz probe events plus 1-Hz counter logging.
                    if observer and received >= next_observation:
                        journal.emit('packet_observed', id=index, packet_kind=kind, received_ns=received,
                                     sent_ns=injected, udp_bytes=len(data))
                        next_observation = received + 16_666_667
            now = time.monotonic_ns()
            if observer and now >= next_sample:
                journal.emit('sample', counters=dict(counts), process_cpu_s=time.process_time() - cpu_start)
                next_sample = now + 1_000_000_000
    except Exception as error:
        errors.append(repr(error))
        counts['runner_errors'] += 1
    finally:
        if relay:
            shape = relay.close()
        for sock in sockets:
            sock.close()
        cpu_total = time.process_time() - cpu_start + shape.get('calibration_relay_cpu_s', 0)
        journal.close()

    missing = sorted(set(offered) - observed)
    unanswered = sorted(set(offered) - replies) if mode == 'probe' else []
    direction_name = ('c2s', 's2c')[direction]
    directional_shape = shape.get(direction_name, {})
    dropped = sum(directional_shape.get(key, 0) for key in ('random_drops', 'queue_drops', 'blackout_drops'))
    forward_late = directional_shape.get('lateness_ms', {}).get('p99')
    criteria = []

    def check(name, passed, detail):
        criteria.append({'criterion': name, 'pass': bool(passed), 'detail': detail})

    check('packet integrity', not any(counts[k] for k in ('corrupt_datagrams', 'duplicate_datagrams', 'unexpected_datagrams', 'unexpected_source')), dict(counts))
    check('socket/runner errors', not errors and not shape.get('error') and not any(shape.get(d, {}).get('socket_send_errors', 0) for d in ('c2s', 's2c')), errors or shape.get('error'))
    check('forward offered/delivered/drop accounting',
          directional_shape.get('received_datagrams', 0) == len(offered)
          and directional_shape.get('delivered_datagrams', 0) == len(observed)
          and len(missing) == dropped,
          {'offered': len(offered), 'relay_received': directional_shape.get('received_datagrams', 0),
           'independent_received': len(observed), 'missing': len(missing), 'explained_drops': dropped})
    for name in ('c2s', 's2c'):
        lag = shape.get(name, {}).get('lateness_ms', {}).get('p99')
        if lag is not None:
            tolerance = max(.5, profile[name]['one_way_ms'] * .1)
            check(name + ' relay dispatch timing', lag <= tolerance, {'p99_ms': lag, 'budget_ms': tolerance})

    offered_rate = counts['offered_wire_bytes'] * 8 / seconds / 1e6
    received_rate = counts['received_measure_wire_bytes'] * 8 / seconds / 1e6
    if mode in ('paced', 'saturated'):
        cap = profile[direction_name]['rate_mbps']
        check('sender offered rate', .95 * offered_mbps <= offered_rate <= 1.05 * offered_mbps,
              {'requested_wire_mbps': offered_mbps, 'observed_wire_mbps': offered_rate})
        candidates = directional_shape.get('received_datagrams', 0) - sum(
            directional_shape.get(k, 0) for k in ('queue_drops', 'blackout_drops'))
        envelope = receiver_rate_bounds(offered_rate, cap, profile[direction_name].get('loss', 0),
                                        candidates, seconds, packet_size + 28)
        check('independent receiver wire rate', envelope['valid'] and
              envelope['lower_wire_mbps'] <= received_rate <= envelope['upper_wire_mbps'],
              dict(envelope, received_wire_mbps=received_rate))
    if mode == 'paced':
        check('no unintended paced queue loss', not directional_shape.get('queue_drops'), directional_shape.get('queue_drops', 0))
        expected_missing = sorted(index for index in offered if seeded_loss(seed, direction, index, profile[direction_name].get('loss', 0)))
        check('seeded loss at independent receiver', missing == expected_missing,
              {'expected_missing_ids': expected_missing, 'observed_missing_ids': missing})
        latency = timing_summary(one_way)
        base = profile[direction_name]['one_way_ms'] + (packet_size + 28) * 8 / (profile[direction_name]['rate_mbps'] * 1000)
        jitter = profile[direction_name].get('jitter_ms', 0)
        check('paced one-way delay/jitter envelope',
              latency['p50'] is not None and base - jitter - .5 <= latency['p50'] <= base + jitter + 1.5
              and latency['p99'] <= base + jitter + 3,
              {'expected_base_ms': base, 'configured_uniform_jitter_ms': jitter, 'observed_ms': latency,
               'note': 'Includes local socket, CRC driver and scheduling; not a statistical loss/jitter certification.'})
    if mode == 'probe':
        reverse = shape.get(('s2c', 'c2s')[direction], {})
        reverse_drops = sum(reverse.get(k, 0) for k in ('random_drops', 'queue_drops', 'blackout_drops'))
        check('echo/drop accounting', counts['echoes_sent'] == len(observed)
              and reverse.get('received_datagrams', 0) == counts['echoes_sent']
              and reverse.get('delivered_datagrams', 0) == len(replies)
              and len(unanswered) == len(missing) + reverse_drops,
              {'sent': len(offered), 'received_replies': len(replies), 'unanswered': len(unanswered),
               'forward_drops': len(missing), 'reverse_drops': reverse_drops})
        latency = timing_summary(rtts)
        base = sum(profile[d]['one_way_ms'] + 128 * 8 / (profile[d]['rate_mbps'] * 1000) for d in ('c2s', 's2c'))
        jitter = sum(profile[d].get('jitter_ms', 0) for d in ('c2s', 's2c'))
        check('unloaded raw RTT', latency['p50'] is not None and base - jitter - 1 <= latency['p50'] <= base + jitter + 3
              and latency['p99'] <= base + jitter + 5,
              {'expected_base_ms': base, 'configured_total_jitter_ms': jitter, 'observed_ms': latency})
    if mode == 'headroom':
        observed_pps = len(observed) / seconds
        check('sustained packet headroom', observed_pps >= .98 * target_pps,
              {'target_pps': target_pps, 'independent_delivered_pps': observed_pps,
               '10ms_send_peak_pps': peak_pps(sent_times, .01), '10ms_receive_peak_pps': peak_pps(received_times, .01)})
        check('no unshaped headroom loss', not missing and not dropped, {'missing': len(missing), 'drops': dropped})
        check('short offered burst', peak_pps(sent_times, .01) >= 2 * target_pps,
              {'required_10ms_pps': 2 * target_pps, 'observed_10ms_pps': peak_pps(sent_times, .01), 'requested_factor': burst_factor})
    result = {'version': 1, 'scope': 'HARNESS_CALIBRATION_ONLY', 'profile_id': profile_id,
              'seed': seed, 'mode': mode, 'direction': direction_name, 'observer_enabled': observer,
              'observer_definition': '1-Hz counters and up to 60-Hz packet events; relay counters always enabled in a separate process',
              'measurement_s': seconds, 'clock': {'start_ns': start, 'stop_ns': stop, 'end_ns': end},
              'packet_bytes': packet_size, 'wire_layer': 'UDP datagram + 28 IPv4/UDP bytes',
              'counts': dict(counts), 'missing_ids': missing, 'unanswered_probe_ids': unanswered,
              'offered_wire_mbps': offered_rate, 'delivered_wire_mbps': received_rate,
              'delivered_pps': len(observed) / seconds, 'send_deadline_lateness_ms': timing_summary(deadline_lags),
              'one_way_ms': timing_summary(one_way), 'raw_rtt_ms': timing_summary(rtts),
              'relay_forward_p99_lateness_ms': forward_late, 'cpu_total_s': cpu_total,
              'criteria': criteria, 'pass': all(item['pass'] for item in criteria), 'errors': errors}
    write_json(folder / 'result.json', result)
    write_json(folder / 'profile.json', profile)
    write_json(folder / 'shaper.json', shape)
    # Write complete independent packet evidence after the measured sample.
    with (folder / 'packets.jsonl').open('w') as stream:
        for event, stamp, index, kind, origin, size, injected, scheduled in traces:
            stream.write(json.dumps({'type': event, 't_ns': stamp, 'id': index, 'kind': kind,
                                     'direction': origin, 'udp_bytes': size, 'sent_ns': injected,
                                     'scheduled_ns': scheduled}, separators=(',', ':')) + '\n')
    seal(folder)
    result['evidence'] = {'path': str(folder / 'result.json'), 'sha256': sha256(folder / 'result.json'),
                          'packet_evidence_path': str(folder / 'packets.jsonl'),
                          'packet_evidence_sha256': sha256(folder / 'packets.jsonl')}
    return result


def observer_comparison(samples):
    pairs = []
    for off in [r for r in samples if r['mode'] == 'headroom' and not r['observer_enabled']]:
        on = next(r for r in samples if r['mode'] == 'headroom' and r['observer_enabled'] and r['seed'] == off['seed'])
        pairs.append({'seed': off['seed'], 'cpu_delta_fraction': (on['cpu_total_s'] - off['cpu_total_s']) / max(.000001, off['cpu_total_s']),
                      'delivered_rate_delta_fraction': (on['delivered_pps'] - off['delivered_pps']) / max(1, off['delivered_pps']),
                      'off_p99_lateness_ms': off['relay_forward_p99_lateness_ms'],
                      'on_p99_lateness_ms': on['relay_forward_p99_lateness_ms']})
    cpu_delta = statistics.median(p['cpu_delta_fraction'] for p in pairs)
    rate_delta = statistics.median(p['delivered_rate_delta_fraction'] for p in pairs)
    return {'pairs': pairs, 'median_cpu_delta_fraction': cpu_delta,
            'median_delivered_rate_delta_fraction': rate_delta,
            'criteria': {'cpu_increase_at_most_10_percent': cpu_delta <= .10,
                         'delivered_rate_decrease_at_most_5_percent': rate_delta >= -.05},
            'pass': cpu_delta <= .10 and rate_delta >= -.05,
            'limitations': 'Off disables external journal writes only. Both runs keep validation, timestamps and relay counters. Native observer/driver overhead requires separate evidence.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--profiles', default=str(Path(__file__).parent / 'configs/profiles.json'))
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--seeds', default='11,29,47')
    parser.add_argument('--packet-bytes', type=int, default=1232)
    parser.add_argument('--paced-mbps', type=float, default=9)
    parser.add_argument('--saturated-mbps', type=float, default=20)
    parser.add_argument('--native-pps', type=float, default=0, help='Measured peak native packet rate; zero means not yet supplied')
    parser.add_argument('--headroom-pps', type=float, default=6000)
    parser.add_argument('--probe-hz', type=float, default=60)
    parser.add_argument('--drain-s', type=float, default=.25)
    parser.add_argument('--burst-factor', type=float, default=4)
    args = parser.parse_args(argv)
    numeric = (args.seconds, args.paced_mbps, args.saturated_mbps, args.native_pps,
               args.headroom_pps, args.probe_hz, args.drain_s, args.burst_factor)
    if any(not math.isfinite(n) for n in numeric) or not (1 <= args.seconds <= 120 and 64 <= args.packet_bytes <= 60000
            and 0 < args.paced_mbps < 10 < args.saturated_mbps and args.native_pps >= 0
            and 0 < args.headroom_pps <= 100000 and 0 < args.probe_hz <= 1000
            and .1 <= args.drain_s <= 5 and 2 <= args.burst_factor <= 10):
        parser.error('invalid sample, rate, size, burst or drain bounds')
    try:
        seeds = [int(s) for s in args.seeds.split(',')]
    except ValueError:
        parser.error('seeds must be comma-separated integers')
    if not seeds or len(set(seeds)) != len(seeds) or any(not 0 <= s < 2**31 - 1000003 for s in seeds):
        parser.error('need unique seeds in 0..2146483643')
    target = max(args.headroom_pps, 2 * args.native_pps)
    root = Path(args.output).expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    profiles = json.loads(Path(args.profiles).read_text())['profiles']
    samples = []
    for profile_id in ('ten_mbit_clean', 'ten_mbit'):
        for seed in seeds:
            tests = [('probe', 0)] + [(mode, direction) for mode in ('paced', 'saturated') for direction in (0, 1)]
            for mode, direction in tests:
                folder = root / f'{profile_id}-{mode}-{direction}-s{seed}-{uuid.uuid4().hex[:8]}'
                print('CALIBRATE', folder.name, flush=True)
                result = run_sample(profiles[profile_id], profile_id, seed, mode, direction, args.seconds, folder,
                                    packet_size=args.packet_bytes, offered_mbps=args.paced_mbps if mode == 'paced' else args.saturated_mbps,
                                    target_pps=target, probe_hz=args.probe_hz, drain_s=args.drain_s, burst_factor=args.burst_factor)
                samples.append(result)
                print('PASS' if result['pass'] else 'FAIL', '; '.join(c['criterion'] for c in result['criteria'] if not c['pass']), flush=True)
    for seed_index, seed in enumerate(seeds):
        # Alternate order to reduce observer comparisons' ordering bias.
        for observer in ((False, True) if seed_index % 2 == 0 else (True, False)):
            folder = root / f'headroom-observer-{int(observer)}-s{seed}-{uuid.uuid4().hex[:8]}'
            print('CALIBRATE', folder.name, flush=True)
            result = run_sample(profiles['loopback'], 'loopback', seed, 'headroom', 0, args.seconds, folder,
                                packet_size=args.packet_bytes, target_pps=target, observer=observer,
                                probe_hz=args.probe_hz, drain_s=args.drain_s, burst_factor=args.burst_factor)
            samples.append(result)
            print('PASS' if result['pass'] else 'FAIL', '; '.join(c['criterion'] for c in result['criteria'] if not c['pass']), flush=True)
    overhead = observer_comparison(samples)
    headroom = [r for r in samples if r['mode'] == 'headroom']
    accepted_pps = min(r['delivered_pps'] for r in headroom) if all(r['pass'] for r in headroom) else 0
    failed = [r['evidence']['path'] for r in samples if not r['pass']]
    summary = {'version': 1, 'scope': 'HARNESS_CALIBRATION_ONLY', 'approved': False,
               'environment': environment(), 'configuration': vars(args), 'target_headroom_pps': target,
               'driver_sha256': sha256(Path(__file__).resolve()),
               'engine_files': {name: sha256(Path(__file__).resolve().parent / name)
                                for name in ('rdpbench/link.py', 'rdpbench/common.py')},
               'profile_source_sha256': sha256(Path(args.profiles)),
               'empirically_passed_packet_rate': accepted_pps,
               'covers_2x_supplied_native_pps': args.native_pps > 0 and accepted_pps >= 2 * args.native_pps,
               'samples': samples, 'observer_overhead': overhead, 'failed_samples': failed,
               'pass': not failed and overhead['pass'],
               'limitations': ['No approved calibration receipt is generated.',
                              'Short samples check deterministic loss and delay envelopes; they do not tightly estimate a 0.05% population loss rate.',
                              'Native peer/driver capacity and native instrumentation overhead require separate direct-socket evidence.',
                              'Packet timestamps are local application observations on one Mac, not physical NIC timestamps.']}
    write_json(root / 'calibration.json', summary)
    evidence = [{'path': str(root / 'calibration.json'), 'sha256': sha256(root / 'calibration.json')}]
    evidence.extend({'path': r['evidence']['path'], 'sha256': r['evidence']['sha256']} for r in samples)
    write_json(root / 'evidence.json', {'version': 1, 'approved': False, 'evidence': evidence})
    print(json.dumps({'pass': summary['pass'], 'failed_samples': len(failed),
                      'observer_overhead_pass': overhead['pass'], 'calibration': str(root / 'calibration.json')}), flush=True)
    return 0 if summary['pass'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print('Interrupted; finished calibration samples are retained.', file=sys.stderr)
        sys.exit(130)
