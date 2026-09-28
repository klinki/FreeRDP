#!/usr/bin/env python3
"""Run the multi-monitor client with a bounded rolling capture. Ctrl+C saves and stops both."""
import argparse
import datetime
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time


LOG_ROTATE_BYTES = 100 * 1024 * 1024
LOG_ROTATE_KEEP = 3


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


def file_sha(path):
    """Best-effort sha256, empty string on any failure."""
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()[:16]
    except OSError:
        return ''


def git_rev(repo):
    """Best-effort short HEAD, empty string outside a git tree."""
    try:
        out = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--short', 'HEAD'],
                                      text=True, stderr=subprocess.DEVNULL)
        return out.strip()
    except (OSError, subprocess.CalledProcessError):
        return ''


def rotate_logs(path, stop):
    """Copytruncate rotation for the live client log (DEBUG logs are loss
    tolerant: lines written during the copy/truncate window may duplicate or
    drop; DEP-acceptable for diagnostics, never used for accounting)."""
    path = Path(path)
    while not stop.wait(5):
        try:
            if path.stat().st_size < LOG_ROTATE_BYTES:
                continue
        except OSError:
            continue
        for i in range(LOG_ROTATE_KEEP - 1, 0, -1):
            src = path.with_name('%s.%d' % (path.name, i))
            if not src.exists():
                continue
            try:
                src.replace(path.with_name('%s.%d' % (path.name, i + 1)))
            except OSError:
                pass
        try:
            shutil.copyfile(path, path.with_name('%s.1' % path.name))
            with open(path, 'r+b') as handle:
                handle.truncate(0)
        except OSError:
            pass


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
    launcher_sha = file_sha(launcher)
    rev = git_rev(repo)
    freeze_helper = repo / 'freeze-snapshot.sh'
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
        f'Launcher: {launcher} (sha {launcher_sha or "unknown"})\n'
        f'Git rev: {rev or "unknown"}\n'
        f'FreeRDP binary: {env["FREERDP_BIN"]}\n'
        f'Capture ring: {args.files} x {args.file_mb} MB\n'
        f'Client log rotates at {LOG_ROTATE_BYTES // (1024 * 1024)} MB, '
        f'keeping {LOG_ROTATE_KEEP} copies (copytruncate: lines at the rotation '
        f'boundary may duplicate or drop).\n'
        'TLS secrets allow session decryption. Retain them with these captures.\n')
    (folder / 'WHEN_FROZEN.txt').write_text(
        'UI frozen? Run this in another terminal BEFORE killing anything:\n'
        f'  bash {freeze_helper} "$(pgrep -f sdl-freerdp | head -1)" '
        f'{folder}/freeze-$(date +%H%M%S)\n'
        'It captures: process sample (stacks), CPU delta, load, memory pressure,\n'
        'GPU/power (needs sudo, skipped otherwise), sockets, open files.\n')
    capture = client = None
    result = 0
    rot_stop = threading.Event()
    rot_thread = threading.Thread(target=rotate_logs,
                                  args=(Path(env['LOG_FILE']), rot_stop), daemon=True)
    print(f'Debug files: {folder}\nCapture limit: approximately {args.files * args.file_mb} MB.'
          '\nCtrl+C in this terminal stops the client and capture, preserving files.', flush=True)
    try:
        rot_thread.start()
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
        rot_stop.set()
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
