#!/usr/bin/env python3
"""Ten sequential live-VM UDP establishment checks; no fault injection.

RDP_TEST_PASSWORD is required. Raw logs stay in the supplied output directory.
Each process gets a PTY so startup log timings aren't distorted by buffering.
Only errors before our intentional disconnect affect the result.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--count', type=int, default=10)
parser.add_argument('--observe', type=float, default=12)
parser.add_argument('--startup-timeout', type=float, default=25)
parser.add_argument('--gap', type=float, default=3)
args = parser.parse_args()
password = os.environ['RDP_TEST_PASSWORD']
out = Path(args.output)
out.mkdir(parents=True, exist_ok=False)
flags = [
    '/v:192.168.64.2', '/u:David', '/cert:tofu', '/gfx:AVC444:on',
    '/network:lan', '/clipboard', '/multimon', '+f', '/monitors:3,2',
    '/sdl-monitor-scale:3=175/100,2=100/100', '+multitransport',
    '/log-filters:com.freerdp.core:DEBUG,com.freerdp.core.rdpeudp:DEBUG,'
    'com.freerdp.core.multitransport:DEBUG,com.freerdp.channels.rdpgfx.client:TRACE',
]
markers = {
    'udp_requested': 'spawning async UDP establishment',
    'udp_connected': 'RDP-UDP connected to',
    'udp_tls': 'TLS over RDP-UDP established',
    'udp_tunnel': 'established over UDP',
    'tcp_warning': 'staying on TCP to preserve ordering',
    'udp_receive': 'UDP recv migrated on first DVC PDU',
    'active': '--> CONNECTION_STATE_ACTIVE',
    'first_frame': 'RecvEndFramePdu:',
}
fault = re.compile(
    r'RDPEUDP2 send timeout|RDP-UDP async establishment failed|'
    r'RDP-UDP handshake .*failed|TLS over RDP-UDP failed|'
    r'Tunnel Create .*failed|UDP channel send failed|'
    r'ERRCONNECT_CONNECT_TRANSPORT_FAILED|ERRINFO_[A-Z_]+|'
    r'Connection reset by peer|Broken pipe|context->EndFrame failed'
)
summary = {
    'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'binary': args.binary,
    'binary_sha256': hashlib.sha256(Path(args.binary).read_bytes()).hexdigest(),
    'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'arguments_without_password': flags,
    'observe_seconds': args.observe,
    'startup_timeout_seconds': args.startup_timeout,
    'gap_seconds': args.gap,
    'attempts': [],
}


def save():
    temp = out / 'summary.tmp'
    temp.write_text(json.dumps(summary, indent=2) + '\n')
    temp.replace(out / 'summary.json')


def run(number):
    master, slave = pty.openpty()
    child_env = os.environ.copy()
    for name in ['RDP_TEST_PASSWORD', 'DYLD_INSERT_LIBRARIES', 'RDPEUDP_TRACE', 'SSLKEYLOGFILE']:
        child_env.pop(name, None)
    start = time.monotonic()
    child = subprocess.Popen([args.binary, *flags, '/p:' + password],
                             stdin=subprocess.DEVNULL, stdout=slave, stderr=slave,
                             env=child_env, start_new_session=True)
    os.close(slave)
    attempt = dict(number=number, pid=child.pid, stages={}, frames=0, faults=[],
                   started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    summary['attempts'].append(attempt)
    save()
    print(f'Attempt {number}/{args.count}: pid={child.pid}', flush=True)
    pending = b''
    stopping = False
    log = (out / f'attempt-{number:02}.log').open('wb')

    def read_output(timeout):
        nonlocal pending
        if not select.select([master], [], [], timeout)[0]:
            return
        try:
            data = os.read(master, 65536)
        except OSError:
            return
        log.write(data)
        log.flush()
        pending += data
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            line = line.decode('utf-8', 'replace').rstrip('\r')
            elapsed = round(time.monotonic() - start, 3)
            if stopping:
                continue
            for key, text in markers.items():
                if text in line and key not in attempt['stages']:
                    attempt['stages'][key] = elapsed
                    save()
            if 'RecvEndFramePdu:' in line:
                attempt['frames'] += 1
                attempt['last_frame_seconds'] = elapsed
            if fault.search(line):
                attempt['faults'].append(dict(seconds=elapsed, message=line))
                save()

    try:
        deadline = start + args.startup_timeout
        established_at = None
        while child.poll() is None:
            read_output(0.1)
            now = time.monotonic()
            stages = attempt['stages']
            if established_at is None and all(k in stages for k in
                                              ('active', 'udp_tunnel', 'udp_receive', 'first_frame')):
                established_at = now
                attempt['observation_started_seconds'] = round(now - start, 3)
                deadline = now + args.observe
                save()
                print(f'  UDP receiving + graphics ready at {now-start:.2f}s; observing {args.observe:g}s', flush=True)
            if now >= deadline:
                break
        read_output(0)
        attempt['alive_before_stop'] = child.poll() is None
        attempt['observed_seconds'] = round(time.monotonic() - start, 3)
        attempt['exit_before_stop'] = child.poll()
        attempt['outcome'] = (
            'udp_receive_and_graphics_stable'
            if established_at is not None and attempt['alive_before_stop'] and not attempt['faults']
            else 'failed_or_incomplete'
        )
        attempt['intentional_disconnect_seconds'] = round(time.monotonic() - start, 3)
        stopping = True
        save()
    finally:
        for sig, grace in [(signal.SIGINT, 3), (signal.SIGTERM, 2), (signal.SIGKILL, 1)]:
            if child.poll() is not None:
                break
            os.killpg(child.pid, sig)
            stop_at = time.monotonic() + grace
            while child.poll() is None and time.monotonic() < stop_at:
                read_output(0.1)
        child.wait(timeout=2)
        read_output(0)
        attempt['exit_after_cleanup'] = child.returncode
        log.close()
        os.close(master)
        save()
    print(f"  Result: {attempt['outcome']}; frames={attempt['frames']}; faults={len(attempt['faults'])}", flush=True)


for i in range(1, args.count + 1):
    run(i)
    if i < args.count:
        time.sleep(args.gap)
summary['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
save()
print('Completed all attempts: ' + str(out / 'summary.json'), flush=True)
