"""Loss diagnostics preserve missing observations rather than fabricating zeros."""
import os
import socket
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rdpbench.native import (Native, LOSS_TRACE_EVENTS, LOSS_TRACE_FIELDS,
                             LOSS_TRACE_MAX_ROWS, LOSS_TRACE_READ_ROWS, LOSS_TRACE_STATS)


class LossTraceTests(unittest.TestCase):
    def driver(self, rows=None, **overrides):
        rows = rows if rows is not None else [
            [1000, 12, 0, 0, 512, 0, 0, 0, 0, 0],
            [1200, 3, 1, 1, 512, 512, 0, 1, 1, 0],
            [1500, 4, 32, 1, 200, 512, 0, 1, 1, 1],
        ]
        stats = dict(enabled=1, valid=1, count=len(rows), capacity=LOSS_TRACE_MAX_ROWS,
                     overflow=0, recovery_timer=0)
        stats.update(overrides)

        class Library:
            def freerdp_bench_v1_loss_trace_stats(self, handle, out, count):
                self.assert_count = count
                for i, field in enumerate(LOSS_TRACE_STATS):
                    out[i] = stats[field]
                return 1

            def freerdp_bench_v1_loss_trace_read(self, handle, start, out, count):
                self.reads.append((start, count))
                selected = rows[start:start+count]
                for i, row in enumerate(selected):
                    for j, value in enumerate(row):
                        out[i*len(LOSS_TRACE_FIELDS)+j] = value
                return len(selected)

        driver = Native.__new__(Native)
        driver.lib = Library()
        driver.lib.reads = []
        driver.h = object()
        driver.loss_trace_available = True
        sample = dict(python_before_ns=100, python_after_ns=106, native_ns=93,
                      offset_ns=-10, uncertainty_ns=3)
        driver.loss_alignment = {'initial':sample, 'drift_allowance_ns':250000}
        driver._queue_clock_sample = Mock(return_value=dict(sample, offset_ns=-12))
        return driver, stats

    def test_rows_align_to_python_clock_and_retain_state(self):
        driver, stats = self.driver()
        trace = driver.loss_trace()
        self.assertTrue(trace['valid'], trace['reasons'])
        self.assertEqual(trace['rows'][0][0], 1010)
        self.assertEqual(trace['rows'][2][1:], [4, 32, 1, 200, 512, 0, 1, 1, 1])
        self.assertEqual(trace['event_names'][4], 'retry_timer')
        self.assertEqual(trace['metadata']['timestamp_clock'], 'python_monotonic_ns')
        self.assertEqual(trace['clock_alignment']['offset_drift_ns'], 2)
        self.assertEqual(driver.lib.reads, [(0, 3)])

    def test_overflow_invalidates_but_retains_available_rows(self):
        driver, stats = self.driver(valid=0, overflow=12)
        trace = driver.loss_trace()
        self.assertFalse(trace['valid'])
        self.assertEqual(len(trace['rows']), 3)
        self.assertIn('native loss trace buffer overflow; events missing', trace['reasons'])
        self.assertEqual(trace['metadata']['overflow'], 12)

    def test_geometry_is_bounded_before_allocating_or_reading(self):
        for capacity, count in ((LOSS_TRACE_MAX_ROWS+1, 1), (0, 0), (2, 3)):
            driver, stats = self.driver(capacity=capacity, count=count)
            trace = driver.loss_trace()
            self.assertFalse(trace['valid'])
            self.assertEqual(driver.lib.reads, [])
            self.assertEqual(trace['rows'], [])

    def test_long_trace_is_read_in_bounded_chunks(self):
        row_count = LOSS_TRACE_READ_ROWS+2
        driver, stats = self.driver([[1000+i, 3, i%65536, 1, 1, 1, 0, 1, 1, 0]
                                    for i in range(row_count)])
        trace = driver.loss_trace()
        self.assertTrue(trace['valid'])
        self.assertEqual(driver.lib.reads, [(0, LOSS_TRACE_READ_ROWS), (LOSS_TRACE_READ_ROWS, 2)])
        self.assertEqual(len(trace['rows']), row_count)

    def test_old_api_disabled_and_missing_alignment_are_unknown(self):
        driver, stats = self.driver()
        driver.loss_trace_available = False
        trace = driver.loss_trace()
        self.assertFalse(trace['available'])
        self.assertFalse(trace['valid'])
        self.assertEqual(trace['rows'], [])
        driver.loss_trace_available = True
        stats['enabled'] = 0
        trace = driver.loss_trace()
        self.assertFalse(trace['enabled'])
        self.assertFalse(trace['valid'])
        self.assertEqual(trace['rows'], [])
        stats['enabled'] = 1
        driver.loss_alignment = None
        trace = driver.loss_trace()
        self.assertFalse(trace['valid'])
        self.assertEqual(trace['rows'][0][0], 1000)
        self.assertEqual(trace['metadata']['timestamp_clock'], 'native_monotonic_ns')

    def test_clock_drift_backwards_and_unknown_events_invalidate(self):
        driver, stats = self.driver([[1200, len(LOSS_TRACE_EVENTS), 0, 0, 0, 0, 0, 0, 0, 0],
                                    [1000, 1, 0, 0, 0, 0, 0, 0, 0, 0]])
        driver._queue_clock_sample.return_value['offset_ns'] = 250000
        trace = driver.loss_trace()
        self.assertFalse(trace['valid'])
        self.assertIn('native loss trace timestamps moved backwards', trace['reasons'])
        self.assertIn('unknown native loss trace event', trace['reasons'])
        self.assertTrue(any('alignment drift' in reason for reason in trace['reasons']))

    def test_partial_read_and_changing_snapshot_invalidate(self):
        driver, stats = self.driver(count=4)
        trace = driver.loss_trace()
        self.assertFalse(trace['valid'])
        self.assertIn('native loss trace read incomplete', trace['reasons'])
        driver, stats = self.driver()
        read = driver.lib.freerdp_bench_v1_loss_trace_read
        def mutate(*args):
            copied = read(*args)
            stats['overflow'] += 1
            return copied
        driver.lib.freerdp_bench_v1_loss_trace_read = mutate
        trace = driver.loss_trace()
        self.assertFalse(trace['valid'])
        self.assertIn('native loss trace changed during export; endpoint still active', trace['reasons'])

    @unittest.skipUnless(os.environ.get('RDP_BENCH_TEST_LIBRARY'),
                         'set RDP_BENCH_TEST_LIBRARY for actual native trace validation')
    def test_actual_native_optin_trace_and_invalid_environment(self):
        library = os.environ['RDP_BENCH_TEST_LIBRARY']
        import ctypes
        native=ctypes.CDLL(library)
        if hasattr(native,'freerdp_bench_v1_backend'):
            native.freerdp_bench_v1_backend.argtypes=[];native.freerdp_bench_v1_backend.restype=ctypes.c_int
            if native.freerdp_bench_v1_backend()==2:self.skipTest('backend2 does not implement native loss trace or recovery-mode overrides')
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(('127.0.0.1', 0))
            peer.settimeout(2)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as temporary:
                temporary.bind(('127.0.0.1', 0))
                port = temporary.getsockname()[1]
            for environment in ({'RDP_BENCH_LOSS_TRACE':'invalid'},
                                {'RDP_BENCH_LOSS_RECOVERY_MODE':'invalid'}):
                with patch.dict(os.environ, environment):
                    with self.assertRaisesRegex(RuntimeError, 'endpoint creation failed'):
                        Native(library, '127.0.0.1', port, '127.0.0.1', peer.getsockname()[1])
            with patch.dict(os.environ, {'RDP_BENCH_LOSS_TRACE':'1',
                                        'RDP_BENCH_LOSS_RECOVERY_MODE':'timer'}):
                driver = Native(library, '127.0.0.1', port, '127.0.0.1', peer.getsockname()[1])
                try:
                    driver.send(b'payload')
                    peer.recv(65535)
                    trace = driver.loss_trace()
                    self.assertTrue(trace['valid'], trace['reasons'])
                    self.assertEqual(trace['metadata']['recovery_mode'], 'timer')
                    self.assertTrue(any(row[1] == 3 and row[4] == 7 for row in trace['rows']))
                    self.assertTrue(all(len(row) == len(LOSS_TRACE_FIELDS) for row in trace['rows']))
                finally:
                    driver.close()


if __name__ == '__main__':
    unittest.main()
