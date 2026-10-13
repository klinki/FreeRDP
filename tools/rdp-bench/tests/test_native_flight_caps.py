"""Exact active-packet caps are separate from byte queues and protocol spans."""
import os
from pathlib import Path
import socket
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rdpbench.native import FLIGHT_FIELDS, QUEUE_FIELDS, Native
from rdpbench.packet_trace import decode_udp2


class NativeFlightCapTests(unittest.TestCase):
    @staticmethod
    def fixture(values=None, available=True, getter_result=1):
        driver = Native.__new__(Native)
        driver.h = object()
        driver.flight_available = available
        values = values or [1, 16, 16, 127, 127, 128, 127, 7, 7, 131072,
                            16, 17, 16, 17, 16, 1]

        def getter(handle, output, count):
            for i, value in enumerate(values):
                output[i] = value
            return getter_result

        driver.lib = SimpleNamespace(freerdp_bench_v1_flight_config=Mock(side_effect=getter))
        return driver

    def test_optional_abi_does_not_invent_zero_configuration(self):
        driver = self.fixture(available=False)
        self.assertIsNone(driver.flight_config())
        driver.lib.freerdp_bench_v1_flight_config.assert_not_called()

    def test_separate_abi_retains_count_span_and_measurement_scope(self):
        driver = self.fixture()
        value = driver.flight_config()
        self.assertEqual(len(FLIGHT_FIELDS), 16)
        self.assertEqual(len(QUEUE_FIELDS), 40)
        fields = value['fields']
        self.assertEqual(fields['configured_max_inflight_packets'], 16)
        self.assertEqual(fields['effective_max_inflight_packets'], 16)
        self.assertEqual(fields['queue_limit_bytes'], 131072)
        self.assertEqual(fields['current_inflight_packets'], 16)
        self.assertEqual(fields['current_active_span_packets'], 17)
        self.assertEqual(fields['measurement_peak_valid'], 1)
        self.assertIn('warmup and drain', value['peak_scope'])
        args = driver.lib.freerdp_bench_v1_flight_config.call_args.args
        self.assertEqual(args[0], driver.h)
        self.assertEqual(args[2], 16)

    def test_getter_failure_and_unknown_version_are_visible(self):
        with self.assertRaisesRegex(RuntimeError, 'snapshot failed'):
            self.fixture(getter_result=0).flight_config()
        values = [2] + [0] * 15
        with self.assertRaisesRegex(RuntimeError, 'unsupported.*ABI'):
            self.fixture(values=values).flight_config()

    def test_disabled_measurement_peak_stays_explicitly_unknown(self):
        values = [1, 0, 31, 31, 31, 128, 63, 5, 5, 32768, 0, 0, 0, 0, 0, 0]
        value = self.fixture(values=values).flight_config()
        self.assertEqual(value['fields']['configured_max_inflight_packets'], 0)
        self.assertEqual(value['fields']['effective_max_inflight_packets'], 31)
        self.assertEqual(value['fields']['measurement_peak_valid'], 0)
        self.assertIn('unavailable', value['measurement_peak_scope'])

    def test_peer_shrink_retains_grandfathered_active_packets(self):
        values = [1, 64, 3, 127, 3, 128, 127, 7, 2, 131072,
                  64, 64, 64, 64, 0, 0]
        fields = self.fixture(values=values).flight_config()['fields']
        self.assertEqual(fields['effective_max_inflight_packets'], 3)
        self.assertEqual(fields['current_inflight_packets'], 64)
        self.assertEqual(fields['configured_max_inflight_packets'], 64)

    @unittest.skipUnless(os.environ.get('RDP_BENCH_TEST_LIBRARY'),
                         'set RDP_BENCH_TEST_LIBRARY for actual native cap validation')
    def test_actual_native_default_and_invalid_configuration(self):
        library = os.environ['RDP_BENCH_TEST_LIBRARY']
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer, patch.dict(os.environ):
            peer.bind(('127.0.0.1', 0))
            os.environ.pop('RDP_BENCH_MAX_INFLIGHT_PACKETS', None)
            os.environ.pop('RDP_BENCH_SEND_QUEUE_BYTES', None)
            os.environ['RDPEUDP_RECOVERY_TRACE'] = ''
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            driver = Native(library, '127.0.0.1', port,
                            '127.0.0.1', peer.getsockname()[1])
            try:
                fields = driver.flight_config()['fields']
                self.assertEqual(fields['configured_max_inflight_packets'], 0)
                self.assertEqual(fields['effective_max_inflight_packets'], 31)
                self.assertEqual(fields['own_log_window'], 5)
                self.assertEqual(fields['peer_log_window'], 5)
                self.assertEqual(fields['usable_storage_packets'], 63)
                self.assertEqual(fields['queue_limit_bytes'], 32768)
            finally:
                driver.close()
            os.environ['RDP_BENCH_MAX_INFLIGHT_PACKETS'] = '17'
            with self.assertRaisesRegex(RuntimeError, 'endpoint creation failed'):
                Native(library, '127.0.0.1', port,
                       '127.0.0.1', peer.getsockname()[1])
            # Invalid input cannot leave the endpoint's UDP socket allocated.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reusable:
                reusable.bind(('127.0.0.1', port))

    @unittest.skipUnless(os.environ.get('RDP_BENCH_TEST_LIBRARY'),
                         'set RDP_BENCH_TEST_LIBRARY for actual native cap validation')
    def test_actual_native_caps_reach_exact_count_and_block_extra_chunk(self):
        library = os.environ['RDP_BENCH_TEST_LIBRARY']
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(('127.0.0.1', 0))
            peer.settimeout(.02)
            for cap in (16, 32, 48, 64):
                with self.subTest(cap=cap), patch.dict(os.environ, {
                        'RDP_BENCH_MAX_INFLIGHT_PACKETS': str(cap),
                        'RDP_BENCH_SEND_QUEUE_BYTES': '131072',
                        'RDP_BENCH_QUEUE_OBSERVER': '1',
                        'RDPEUDP_RECOVERY_TRACE': '',
                }):
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                        reservation.bind(('127.0.0.1', 0))
                        port = reservation.getsockname()[1]
                    driver = Native(library, '127.0.0.1', port,
                                    '127.0.0.1', peer.getsockname()[1])
                    try:
                        config = driver.flight_config()['fields']
                        self.assertEqual(config['configured_max_inflight_packets'], cap)
                        self.assertEqual(config['effective_max_inflight_packets'], cap)
                        self.assertEqual(config['queue_limit_bytes'], 131072)
                        self.assertEqual(config['local_protocol_capacity_packets'], 127)
                        self.assertEqual(config['peer_protocol_capacity_packets'], 127)
                        self.assertEqual(config['sent_storage_slots'], 128)
                        payload = driver.queue_stats()['fields']['max_payload_bytes']
                        data = b'x' * ((cap + 1) * payload)
                        # Each API call may emit a final short chunk. Align
                        # the fixture's calls so every tested packet is full.
                        write_bytes = (60000 // payload) * payload
                        for start in range(0, len(data), write_bytes):
                            driver.send(data[start:start + write_bytes])
                        packets = []
                        while True:
                            try:
                                packet = decode_udp2(peer.recv(2048))
                            except socket.timeout:
                                break
                            if packet['data_seq'] is not None and not packet['dummy']:
                                packets.append(packet)
                        self.assertEqual(len(packets), cap)
                        self.assertEqual([p['data_seq'] for p in packets], list(range(1, cap + 1)))
                        self.assertTrue(all(p['data_bytes'] == payload for p in packets))
                        fields = driver.flight_config()['fields']
                        self.assertEqual(fields['current_inflight_packets'], cap)
                        self.assertEqual(fields['peak_inflight_packets'], cap)
                        self.assertEqual(fields['current_active_span_packets'], cap)
                        self.assertEqual(fields['measurement_peak_valid'], 0)
                        self.assertEqual(driver.stats()['send_failed'], 0)
                    finally:
                        driver.close()
                    self.assertIsNone(driver.h)


if __name__ == '__main__':
    unittest.main()
