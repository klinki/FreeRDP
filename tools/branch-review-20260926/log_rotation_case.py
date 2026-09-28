#!/usr/bin/env python3
"""Exercise the real rotator against tee without launching an RDP session."""
import argparse
from pathlib import Path
import subprocess
import time


class OneRotation:
    def __init__(self):
        self.calls = 0

    def wait(self, _seconds):
        self.calls += 1
        return self.calls > 1


def wait_size(path, size):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size == size:
            return
        time.sleep(0.01)
    raise RuntimeError(f'tee did not reach {size} bytes')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--launcher', type=Path)
    args = p.parse_args()
    namespace = {'__name__': 'review_rotator', '__file__': str(args.source)}
    exec(compile(args.source.read_text(), str(args.source), 'exec'), namespace)
    namespace['LOG_ROTATE_BYTES'] = 8
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.output / 'rotation.log'
    path.write_bytes(b'')  # Match the launcher's fresh log before starting tee.
    launcher = args.launcher or args.source.parent / 'free-rdp-multi-screen.sh'
    append = 'tee -a "${LOG_FILE}"' in launcher.read_text()
    process = subprocess.Popen(['tee', *(['-a'] if append else []), str(path)], stdin=subprocess.PIPE,
                               stdout=subprocess.DEVNULL)
    try:
        process.stdin.write(b'123456789')
        process.stdin.flush()
        wait_size(path, 9)
        namespace['rotate_logs'](path, OneRotation())
        process.stdin.write(b'next\n')
        process.stdin.flush()
        # Wait for the writer after truncation, in either append or original mode.
        deadline = time.monotonic() + 2
        while path.stat().st_size == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        actual = path.read_bytes()
        print(f'After rotation: expected b"next\\n", actual {actual!r}')
        print(f'Logical size: expected 5, actual {len(actual)}')
        assert actual == b'next\n', 'rotation must not preserve the old write offset'
    finally:
        process.stdin.close()
        process.wait(timeout=2)


if __name__ == '__main__':
    main()
