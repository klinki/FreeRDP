#!/usr/bin/env python3
"""Offline regressions; no process is signaled and no Docker/VM is contacted."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('comparison_process_guard', Path(__file__).with_name('process-guard.py'))
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def stat(birth=123, state='S'):
    fields = [state, *(['0'] * 18), str(birth), *(['0'] * 30)]
    return '42 (tcpdump) ' + ' '.join(fields)


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.record = dict(owner='owned', pid=42, start_ticks=123,
                           executable='/usr/bin/tcpdump',
                           arguments=['-i', 'lo', '-w', '/bench/owned.pcap', 'udp'])
        self.argv = [self.record['executable'], *self.record['arguments']]

    def test_uid_drop_allows_only_complete_capture_identity(self):
        result = guard.checked_snapshot(self.record, 'owned', stat(), exe_denied=True,
                                        argv=self.argv, comm='tcpdump')
        self.assertTrue(result['alive'])
        self.assertTrue(result['executable_access_denied'])
        self.assertIn('exact argv', result['identity_guard'])

    def test_reused_birth_time_is_rejected(self):
        with self.assertRaisesRegex(guard.IdentityError, 'birth time'):
            guard.checked_snapshot(self.record, 'owned', stat(birth=124), exe_denied=True,
                                    argv=self.argv, comm='tcpdump')

    def test_wrong_output_argument_is_rejected(self):
        wrong = [*self.argv]
        wrong[4] = '/bench/foreign.pcap'
        with self.assertRaisesRegex(guard.IdentityError, 'argv or comm'):
            guard.checked_snapshot(self.record, 'owned', stat(), exe_denied=True,
                                    argv=wrong, comm='tcpdump')

    def test_wrong_owner_is_rejected(self):
        with self.assertRaisesRegex(guard.IdentityError, 'ownership'):
            guard.checked_snapshot(self.record, 'other', stat(), exe_denied=True,
                                    argv=self.argv, comm='tcpdump')

    def test_executable_denial_does_not_relax_client_guard(self):
        client = dict(self.record, executable='/bench/linux-pr/client/X11/xfreerdp')
        with self.assertRaisesRegex(guard.IdentityError, 'non-capture'):
            guard.checked_snapshot(client, 'owned', stat(), exe_denied=True)

    def test_wrong_capture_comm_is_rejected(self):
        with self.assertRaisesRegex(guard.IdentityError, 'argv or comm'):
            guard.checked_snapshot(self.record, 'owned', stat(), exe_denied=True,
                                    argv=self.argv, comm='other')

    def test_rejected_identity_never_signals_and_closes_pidfd(self):
        opened = Mock(return_value=91)
        sent = Mock()
        closed = Mock()
        with patch.object(Path, 'read_text', return_value=stat(birth=124)), \
             patch.object(Path, 'read_bytes', return_value=b'\0'.join(x.encode() for x in self.argv)), \
             patch.object(Path, 'resolve', side_effect=PermissionError):
            with self.assertRaises(guard.IdentityError):
                guard.action(self.record, 'owned', 2, pidfd_open=opened,
                              pidfd_send_signal=sent, close=closed)
        sent.assert_not_called()
        closed.assert_called_once_with(91)

    def test_matching_uid_drop_uses_bound_pidfd_not_raw_pid(self):
        opened = Mock(return_value=91)
        sent = Mock()
        closed = Mock()
        with patch.object(Path, 'read_text', side_effect=[stat(), 'tcpdump\n']), \
             patch.object(Path, 'read_bytes', return_value=b'\0'.join(x.encode() for x in self.argv)), \
             patch.object(Path, 'resolve', side_effect=PermissionError):
            result = guard.action(self.record, 'owned', 2, pidfd_open=opened,
                                   pidfd_send_signal=sent, close=closed)
        self.assertEqual(result['outcome'], 'SIGNALED_OWNED_PROCESS')
        opened.assert_called_once_with(42)
        sent.assert_called_once_with(91, 2)
        closed.assert_called_once_with(91)

    def test_one_packet_footer_preserves_successful_capture(self):
        self.assertEqual(guard.capture_footer('1 packet captured\n0 packets dropped by kernel\n'),
                         dict(captured_packets=1, kernel_drops=0))

    def test_missing_footer_is_unknown_not_zero(self):
        self.assertEqual(guard.capture_footer('tcpdump: listening on lo\n'),
                         dict(captured_packets=None, kernel_drops=None))


if __name__ == '__main__':
    unittest.main()
