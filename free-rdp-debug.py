#!/usr/bin/env python3
"""Run the multi-monitor client with a bounded rolling capture. Ctrl+C saves and stops both."""
import argparse
import datetime
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time


def stop_group(proc, sig=signal.SIGINT, timeout=8):
    if proc is None:
        return
    # The launcher contains a client/tee pipeline, so stop its whole process group.
    try:
        os.killpg(proc.pid, sig)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        proc.poll()  # Reap the launcher while also checking its pipeline children.
        try:
            os.killpg(proc.pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait()



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', default=os.environ.get('SERVER', 'davidpc'))
    parser.add_argument('--interface', default=os.environ.get('IFACE'))
    parser.add_argument('--file-mb', type=int, default=100, help='MB per capture file (default 100)')
    parser.add_argument('--files', type=int, default=10, help='ring file count (default 10)')
    parser.add_argument('--output', type=Path, default=Path.home() / 'rdp-debug')
    args = parser.parse_args()
    if args.file_mb < 1 or args.files < 2:
        parser.error('--file-mb must be positive; --files must be at least 2')
    repo = Path(__file__).resolve().parent
    launcher = repo / 'free-rdp-multi-screen.sh'
    dumpcap = shutil.which('dumpcap')
    if not dumpcap or not launcher.is_file():
        parser.error('dumpcap and free-rdp-multi-screen.sh are required')
    # Resolve once and connect to that same IPv4 address so the capture filter agrees.
    address = socket.gethostbyname(args.server)
    interface = args.interface
    if not interface:
        route = subprocess.check_output(['/sbin/route', '-n', 'get', address], text=True)
        match = re.search(r'interface:\s*(\S+)', route)
        if not match:
            raise RuntimeError('Cannot find capture interface; supply --interface')
        interface = match.group(1)
    os.umask(0o077)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    folder = args.output.expanduser().resolve() / f'{stamp}-{os.getpid()}'
    folder.mkdir(parents=True)
    env = os.environ.copy()
    env.setdefault('FREERDP_BIN', str(repo / 'build/videotoolbox/client/SDL/SDL3/sdl-freerdp'))
    env.update(SERVER=address, LOG_FILE=str(folder / 'client.log'),
               SECRETS_FILE=str(folder / 'tls-secrets.txt'))
    capture_args = [dumpcap, '-i', interface, '-f', f'host {address} and port 3389',
                    '-s', '0', '-b', f'filesize:{args.file_mb * 1000}',
                    '-b', f'files:{args.files}', '-w', str(folder / 'capture.pcapng')]
    (folder / 'session.txt').write_text(
        f'Start: {datetime.datetime.now().astimezone().isoformat()}\n'
        f'Server: {args.server} ({address})\nInterface: {interface}\n'
        f'FreeRDP binary: {env["FREERDP_BIN"]}\n'
        f'Capture ring: {args.files} x {args.file_mb} MB\n'
        'Old capture files are overwritten; client.log is not size capped.\n'
        'TLS secrets allow session decryption. Retain them with these captures.\n')
    capture = client = None
    result = 0
    print(f'Debug files: {folder}\nCapture limit: approximately {args.files * args.file_mb} MB.'
          '\nCtrl+C in this terminal stops the client and capture, preserving files.', flush=True)
    try:
        with (folder / 'capture.log').open('w') as caplog:
            capture = subprocess.Popen(capture_args, stdout=caplog, stderr=caplog,
                                       start_new_session=True)
            # Wait for a file: do not start the session if permissions/capture setup failed.
            for _ in range(100):
                if capture.poll() is not None:
                    raise RuntimeError((folder / 'capture.log').read_text())
                if list(folder.glob('capture*.pcapng')):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError('Capture did not start within 10 seconds')
            client = subprocess.Popen(['bash', str(launcher)], env=env,
                                      stdin=sys.stdin, start_new_session=True)
            while client.poll() is None:
                if capture.poll() is not None:
                    raise RuntimeError('Capture stopped unexpectedly; see capture.log')
                time.sleep(0.25)
            result = client.returncode
    except KeyboardInterrupt:
        print('\nStopping and preserving debug files...', flush=True)
    finally:
        # Ignore repeated Ctrl+C while flushing files and shutting down the process groups.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        stop_group(client)
        stop_group(capture)
        with (folder / 'session.txt').open('a') as meta:
            meta.write(f'Stop: {datetime.datetime.now().astimezone().isoformat()}\n')
        print(f'Saved: {folder}', flush=True)
    return result


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f'Error: {error}', file=sys.stderr)
        sys.exit(1)
