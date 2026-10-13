"""Backend-specific telemetry keeps upstream unified failures and unknowns honest."""
import ctypes
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rdpbench.native import Native
from rdpbench.type1 import native_transport_failed

UNKNOWN = (1 << 64) - 1


class PrBackendTests(unittest.TestCase):
    @staticmethod
    def driver(backend=2, values=None, getter_result=1):
        native = Native.__new__(Native)
        native.h = object()
        if backend is not None:
            native.backend = backend
        values = [101, 99, 17, UNKNOWN, 32, 1208, 0, UNKNOWN] if values is None else values

        def snapshot(handle, output, count):
            for index, value in enumerate(values):
                output[index] = value
            return getter_result

        native.lib = SimpleNamespace(freerdp_bench_v1_stats=Mock(side_effect=snapshot))
        return native

    def test_optional_backend_export_preserves_legacy_default(self):
        native = Native.__new__(Native)
        native.lib = SimpleNamespace()
        self.assertEqual(native.detect_backend(), 1)

    def test_backend_getter_uses_zero_argument_integer_abi(self):
        native = Native.__new__(Native)
        native.lib = SimpleNamespace(freerdp_bench_v1_backend=Mock(return_value=2))
        self.assertEqual(native.detect_backend(), 2)
        getter = native.lib.freerdp_bench_v1_backend
        getter.assert_called_once_with()
        self.assertEqual(getter.argtypes, [])
        self.assertIs(getter.restype, ctypes.c_int)
        getter.return_value = 1
        self.assertEqual(native.detect_backend(), 1)

    def test_unsupported_backend_version_is_not_silently_legacy(self):
        for value in (0, 3, -1, 999):
            with self.subTest(backend=value):
                native = Native.__new__(Native)
                native.lib = SimpleNamespace(freerdp_bench_v1_backend=Mock(return_value=value))
                with self.assertRaisesRegex(RuntimeError, 'unsupported private native backend'):
                    native.detect_backend()

    def test_upstream_unified_state_does_not_invent_separate_failure_or_loss_counts(self):
        for failed in (0, 1):
            with self.subTest(failed=failed):
                native = self.driver(values=[101, 99, 17, UNKNOWN, 32, 1208, failed, UNKNOWN])
                result = native.stats()
                self.assertEqual(result['backend'], 2)
                self.assertEqual(result['stats_semantics'], 'upstream-unified-state')
                self.assertEqual(result['transport_failed'], failed)
                for field in ('send_failed', 'receive_failed', 'loss_reports'):
                    self.assertIsNone(result[field])
                self.assertEqual(result['sent_datagrams'], 101)
                self.assertEqual(result['received_datagrams'], 99)
                self.assertEqual(result['retransmissions'], 17)
                self.assertEqual(result['sent_window_entries'], 32)
                self.assertEqual(result['receive_buffer_bytes'], 1208)
                args = native.lib.freerdp_bench_v1_stats.call_args.args
                self.assertEqual(args[0], native.h)
                self.assertEqual(args[2], 8)

    def test_wrong_sentinels_and_non_boolean_unified_flag_reject(self):
        valid = [101, 99, 17, UNKNOWN, 32, 1208, 0, UNKNOWN]
        for index, bad in ((3, 0), (3, UNKNOWN - 1), (7, 0), (7, UNKNOWN - 1),
                           (6, 2), (6, UNKNOWN)):
            with self.subTest(index=index, value=bad):
                values = valid.copy()
                values[index] = bad
                with self.assertRaisesRegex(RuntimeError, 'invalid upstream unified stats/sentinel'):
                    self.driver(values=values).stats()

    def test_missing_snapshot_remains_unknown(self):
        self.assertIsNone(self.driver(getter_result=0).stats())

    def test_legacy_stats_mapping_remains_unchanged(self):
        values = [10, 9, 2, 3, 4, 5, 1, 0]
        expected = dict(sent_datagrams=10, received_datagrams=9, retransmissions=2,
                        loss_reports=3, sent_window_entries=4, receive_buffer_bytes=5,
                        send_failed=1, receive_failed=0)
        for backend in (None, 1):
            with self.subTest(backend=backend):
                self.assertEqual(self.driver(backend=backend, values=values).stats(), expected)

    def test_failure_classifier_accepts_only_known_unified_failure(self):
        for value, expected in ((1, True), (0, False), (None, False),
                                (True, False), ('1', False), (2, False)):
            with self.subTest(value=value):
                receipt = {'native_stats': {'backend': 2,
                    'stats_semantics': 'upstream-unified-state', 'transport_failed': value,
                    'send_failed': None, 'receive_failed': None, 'loss_reports': None}}
                self.assertEqual(native_transport_failed(receipt, 'native'), expected)
                self.assertFalse(native_transport_failed(receipt, 'udp-calibration'))

    def test_deadline_cancellation_is_distinct_from_transport_failure(self):
        receipt = {'benchmark_deadline_reached': True,
                   'native_stats': {'transport_failed': 1, 'send_failed': 1, 'receive_failed': 1},
                   'error': 'production receive failed / unhealthy stream'}
        self.assertFalse(native_transport_failed(receipt, 'native'))
        receipt['benchmark_deadline_reached'] = False
        self.assertTrue(native_transport_failed(receipt, 'native'))

    def test_optional_driver_error_is_unknown_when_export_is_absent(self):
        native = self.driver()
        native.driver_error_available = False
        native.lib.freerdp_bench_v1_driver_error = Mock(return_value=b'never read')
        self.assertIsNone(native.driver_error())
        native.lib.freerdp_bench_v1_driver_error.assert_not_called()

    def test_driver_error_decodes_text_and_preserves_nil(self):
        for raw, expected in ((None, None), (b'', None),
                              (b'BENCHMARK_DRIVER_WAIT: select failed',
                               'BENCHMARK_DRIVER_WAIT: select failed'),
                              (b'bad \xff', 'bad \ufffd')):
            with self.subTest(raw=raw):
                native = self.driver()
                native.driver_error_available = True
                native.lib.freerdp_bench_v1_driver_error = Mock(return_value=raw)
                self.assertEqual(native.driver_error(), expected)
                native.lib.freerdp_bench_v1_driver_error.assert_called_once_with(native.h)

    @classmethod
    def failed_operation_driver(cls, cause=None, deadline=False):
        native = cls.driver()
        native.buf = ctypes.create_string_buffer(32)
        native.driver_error_available = True
        native.deadline_reached = Mock(return_value=deadline)
        native.lib.freerdp_bench_v1_driver_error = Mock(return_value=cause)
        native.lib.freerdp_bench_v1_read = Mock(return_value=-1)
        native.lib.freerdp_bench_v1_send = Mock(return_value=-1)
        return native

    def test_negative_read_send_with_driver_cause_remain_harness_failures(self):
        for operation in ('read', 'send'):
            with self.subTest(operation=operation):
                native = self.failed_operation_driver(b'BENCHMARK_DRIVER_UNREAD_LIMIT')
                with self.assertRaisesRegex(RuntimeError, 'benchmark driver failure: BENCHMARK_DRIVER_UNREAD_LIMIT'):
                    if operation == 'read':
                        native.read()
                    else:
                        native.send(b'payload')
                native.lib.freerdp_bench_v1_driver_error.assert_called_once_with(native.h)

    def test_negative_read_send_without_driver_cause_use_production_errors(self):
        for operation, message in (('read', 'production receive failed / unhealthy stream'),
                                   ('send', 'production send failed: -1/7')):
            with self.subTest(operation=operation):
                native = self.failed_operation_driver()
                with self.assertRaisesRegex(RuntimeError, message):
                    if operation == 'read':
                        native.read()
                    else:
                        native.send(b'payload')

    def test_negative_operation_deadline_precedes_driver_cause(self):
        for operation in ('read', 'send'):
            with self.subTest(operation=operation):
                native = self.failed_operation_driver(b'BENCHMARK_DRIVER_WAIT', deadline=True)
                with self.assertRaisesRegex(RuntimeError, 'benchmark deadline reached; reliable drain incomplete'):
                    if operation == 'read':
                        native.read()
                    else:
                        native.send(b'payload')
                native.lib.freerdp_bench_v1_driver_error.assert_not_called()

    def test_explicit_driver_error_suppresses_native_failure_fallback(self):
        receipt = {'native_driver_error': 'BENCHMARK_DRIVER_WAIT: select failed',
                   'native_stats': {'transport_failed': 1, 'send_failed': 1, 'receive_failed': 1},
                   'error': 'production receive failed / unhealthy stream'}
        self.assertFalse(native_transport_failed(receipt, 'native'))
        receipt['native_driver_error'] = None
        self.assertTrue(native_transport_failed(receipt, 'native'))

    def test_legacy_flags_and_error_fallback_remain_supported(self):
        for stats in ({'send_failed': 1}, {'receive_failed': 1}):
            self.assertTrue(native_transport_failed({'native_stats': stats}, 'native'))
        for error in ('production send failed: -1/16420',
                      'production receive failed / unhealthy stream'):
            self.assertTrue(native_transport_failed({'error': error}, 'native'))
        self.assertFalse(native_transport_failed({'native_stats': {}}, 'native'))


if __name__ == '__main__':
    unittest.main()
