#!/usr/bin/env python3
"""One bounded container-loopback capture; no VM or RDP connection is made."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid

BASE = Path('/bench')
spec = importlib.util.spec_from_file_location('capture_process_guard', BASE / 'process-guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def main():
    owner = uuid.uuid4().hex
    output = BASE / ('capture-guard-smoke-' + owner[:8])
    output.mkdir(exist_ok=False)
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(('127.0.0.1', 0))
    receiver.settimeout(2)
    port = receiver.getsockname()[1]
    executable = shutil.which('tcpdump')
    if not executable:
        raise RuntimeError('tcpdump unavailable')
    receipt_path = output / 'capture-owned.json'
    command = [sys.executable, str(BASE / 'owned-exec.py'), '--owner', owner,
               '--receipt', str(receipt_path), '--executable', executable, '--',
               '-i', 'lo', '-nn', '-U', '-s', '256', '-w', str(output / 'loopback.pcap'),
               f'udp and port {port}']
    log_path = output / 'capture.log'
    log = log_path.open('xb')
    child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    result = dict(scope='one1s local-loopback UDP process-guard smoke; no VM/RDP traffic',
                  command=command, owner=owner, port=port,
                  process_guard_sha256=hashlib.sha256((BASE / 'process-guard.py').read_bytes()).hexdigest())
    record = None
    try:
        deadline = time.monotonic() + 5
        while not receipt_path.exists() or 'listening on' not in log_path.read_text(errors='replace'):
            if child.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('owned loopback capture failed startup')
            time.sleep(.02)
        record = json.loads(receipt_path.read_text())
        result['live_guard'] = guard.action(record, owner)
        started = time.monotonic()
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = b'owned-capture-guard-smoke'
        try:
            sender.sendto(payload, ('127.0.0.1', port))
            received, _ = receiver.recvfrom(256)
            if received != payload:
                raise RuntimeError('local fixture datagram differed')
            result['fixture_received'] = True
        finally:
            sender.close()
        time.sleep(max(0, 1 - (time.monotonic() - started)))
        result['capture_duration_s'] = time.monotonic() - started
        result['stop_guard'] = guard.action(record, owner, signal.SIGINT)
        result['exit_code'] = child.wait(timeout=10)
        result['after_guard'] = guard.action(record, owner)
        text = log_path.read_text(errors='replace')
        result.update(guard.capture_footer(text))
        result['pass'] = (result['exit_code'] == 0 and not result['after_guard']['alive']
                          and result['captured_packets'] is not None and result['captured_packets'] >= 1
                          and result['kernel_drops'] == 0)
    except Exception as error:
        result.update({'pass': False, 'error': repr(error)})
        if record and child.poll() is None:
            result['emergency_stop'] = guard.action(record, owner, signal.SIGTERM)
            child.wait(timeout=5)
    finally:
        receiver.close()
        log.close()
        (output / 'result.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(dict(result, output=str(output)), indent=2, allow_nan=False))
    return 0 if result.get('pass') else 1


if __name__ == '__main__':
    raise SystemExit(main())
