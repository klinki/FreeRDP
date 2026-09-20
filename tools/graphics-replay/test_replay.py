#!/usr/bin/env python3
"""Check that corrupt/truncated recordings cannot produce benchmark results."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


BINARY = Path(os.environ.get('REPLAY_BINARY', 'build/graphics-replay/optimized')).resolve()
MAGIC = b'FRGFXR01'


class ReplayInputTests(unittest.TestCase):
    def reject(self, data, expected, *options):
        with tempfile.TemporaryDirectory(prefix='freerdp-replay-test-') as tmp:
            path = Path(tmp) / 'test.gfx'
            path.write_bytes(data)
            result = subprocess.run([str(BINARY), '--input', str(path), *options],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(expected, result.stderr)
            self.assertNotIn('freerdp.graphics_replay_result', result.stdout)

    def test_invalid_magic_and_empty_stream(self):
        self.reject(b'wrong', 'replay magic')
        self.reject(MAGIC, 'empty replay')

    def test_truncated_header_and_payload(self):
        self.reject(MAGIC + bytes(11), 'truncated replay header')
        self.reject(MAGIC + struct.pack('<QI', 0, 4) + b'ab', 'truncated replay payload')

    def test_oversized_and_empty_record(self):
        for length in (0, 64*1024*1024+1, 0xffffffff):
            self.reject(MAGIC + struct.pack('<QI', 0, length), 'replay record length')

    def test_time_order_and_invalid_options(self):
        self.reject(MAGIC + struct.pack('<QI', 10, 1) + b'x' + struct.pack('<QI', 9, 1),
                    'timestamps are not ordered')
        self.reject(MAGIC, 'expected unsigned integer', '--stop-seconds', '-1')
        self.reject(MAGIC, 'seconds overflow', '--stop-seconds', str(2**64-1))


if __name__ == '__main__':
    unittest.main()
