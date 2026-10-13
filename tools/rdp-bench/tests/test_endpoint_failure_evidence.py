"""Terminal loss failures retain audited prefixes, without inventing probe timeouts."""
import contextlib
import hashlib
import io
import json
import random
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from rdpbench import endpoint
from rdpbench.common import read_json, rows


class Clock:
    def __init__(self):
        self.now = 1_000_000_000

    def advance(self, ns):
        self.now += ns

    def sleep_until(self, ns):
        self.now = max(self.now, ns)


class Driver:
    def __init__(self, clock):
        self.clock = clock
        self.sent = []
        self.reads = 0
        self.closed = False
        self.fail_send = False
        self.fail_read_after = None
        self.snapshot_errors = False
        self.pending_responses = []
        self.read_step_ns = 100_000_000
        self.read_callback = None

    def configure_queue_measurement(self, measure, stop):
        self.bounds = (measure, stop)

    def send(self, data):
        self.sent.append(data)
        if self.fail_send:
            self.clock.advance(50_000_000)
            raise RuntimeError('production send failed: -1/100')
        magic, kind, seq, scheduled, injected, length, crc = endpoint.H.unpack_from(data)
        if kind == 1:
            self.pending_responses.append(endpoint.record(2, seq, scheduled, injected, b'P' * 64))

    def read(self, timeout_ms):
        self.reads += 1
        if self.read_callback:
            return self.read_callback(self)
        self.clock.advance(self.read_step_ns)
        if self.fail_read_after is not None and self.reads >= self.fail_read_after:
            raise RuntimeError('production receive failed / unhealthy stream')
        response = b''.join(self.pending_responses)
        self.pending_responses.clear()
        return response

    def stats(self):
        if self.snapshot_errors:
            raise RuntimeError('native snapshot unavailable')
        return dict(sent_datagrams=123, received_datagrams=247, retransmissions=5,
                    loss_reports=3, sent_window_entries=2, receive_buffer_bytes=10,
                    send_failed=int(self.fail_send), receive_failed=int(self.fail_read_after is not None))

    def queue_stats(self):
        if self.snapshot_errors:
            raise RuntimeError('queue snapshot unavailable')
        return dict(valid=True, fields={'pending_segment_bytes': 16}, summaries={})

    def close(self):
        self.closed = True


class EndpointFailureEvidenceTests(unittest.TestCase):
    CONTROL = dict(start_ns=1_000_000_000, measure_ns=2_000_000_000,
                   stop_ns=5_000_000_000, end_ns=6_000_000_000)

    def run_endpoint(self, driver, clock, *, role='probe', constructor_error=None,
                     trace=False, trace_write_error=False, control=None):
        with tempfile.TemporaryDirectory() as folder:
            config = dict(driver='native', library='unused.dylib', bind='127.0.0.1', port=1,
                          peer='127.0.0.1', peer_port=2, role=role, seed=11,
                          payload_bytes=8, probe_hz=1,
                          events=str(Path(folder) / 'events.jsonl'),
                          result=str(Path(folder) / 'result.json'))
            native = patch('rdpbench.endpoint.Native', side_effect=constructor_error) if constructor_error else patch('rdpbench.endpoint.Native', return_value=driver)
            with contextlib.ExitStack() as stack:
                stack.enter_context(native)
                stack.enter_context(patch('rdpbench.endpoint.time.monotonic_ns', side_effect=lambda: clock.now))
                stack.enter_context(patch('rdpbench.endpoint.time.process_time_ns', side_effect=lambda: clock.now // 1000))
                stack.enter_context(patch('rdpbench.endpoint.sleep_until', side_effect=clock.sleep_until))
                stack.enter_context(patch.dict('os.environ', {'RDP_BENCH_OBSERVER': '1', 'RDP_BENCH_LOSS_TRACE': '1' if trace else '0'}))
                stack.enter_context(patch('sys.stdin', io.StringIO(json.dumps(control or self.CONTROL) + '\n')))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                if trace_write_error:
                    stack.enter_context(patch('rdpbench.endpoint.write_ledger', side_effect=OSError('ledger denied')))
                code = endpoint.run(config)
            result = read_json(config['result'])
            events = list(rows(config['events']))
        return code, result, events

    def assert_probe_accounting(self, result, events):
        counts = result['counters']
        self.assertEqual(counts['probes_expected'], counts['probes_injected'] + counts['probes_not_injected'])
        self.assertEqual(counts['probes_injected'], len([e for e in events if e['type'] == 'probe_injected']))
        self.assertEqual(counts['probes_not_injected'], len([e for e in events if e['type'] == 'probe_not_injected']))
        self.assertEqual(counts['probes_injected'], counts['probes_observed'] + result['probes_pending_at_exit'])

    def test_failed_send_before_measurement_keeps_attempt_separate_from_admission(self):
        clock = Clock(); driver = Driver(clock); driver.fail_send = True
        code, result, events = self.run_endpoint(driver, clock)
        self.assertEqual(code, 1)
        self.assertFalse(result['completed'])
        self.assert_probe_accounting(result, events)
        self.assertEqual(result['counters']['probes_injected'], 1)
        self.assertEqual(result['counters']['probes_admitted'], 0)
        self.assertEqual(result['counters']['probe_send_failures'], 1)
        self.assertEqual(result['counters']['probes_unserved_after_failure'], 3)
        self.assertEqual(result['probe_timeouts'], 0)
        self.assertEqual(result['probe_unobserved'], 1)
        self.assertEqual(result['measurement_observed_s'], 0)
        self.assertEqual(result['probe_rtt_ms']['count'], 0)
        self.assertEqual(result['terminal']['operation'], 'send')
        self.assertEqual(result['terminal']['call_kind'], 'probe')
        self.assertEqual(result['terminal']['since_trial_start_ms'], 50)
        self.assertEqual(result['terminal']['since_measurement_start_ms'], -950)
        self.assertEqual(result['native_stats']['send_failed'], 1)
        self.assertEqual(result['native_stats']['retransmissions'], 5)
        self.assertTrue(driver.closed)
        self.assertFalse([e for e in events if e['type'] == 'probe_timeout'])
        self.assertEqual(len([e for e in events if e['type'] == 'probe_unobserved']), 1)

    def test_measured_read_failure_preserves_valid_records_responses_and_partial_record(self):
        clock = Clock(); driver = Driver(clock); driver.fail_read_after = 4
        payload = random.Random(11).randbytes(8)
        def receive(d):
            if d.reads == 1:
                d.clock.advance(1_000_000_000)
                response = b''.join(d.pending_responses); d.pending_responses.clear()
                return endpoint.record(0, 0, 0, 0, payload) + response
            if d.reads == 2:
                d.clock.advance(100_000_000)
                response = b''.join(d.pending_responses); d.pending_responses.clear()
                return endpoint.record(0, 1, 0, 0, payload) + response + endpoint.record(0, 2, 0, 0, payload)[:endpoint.H.size + 2]
            if d.reads == 3:
                d.clock.advance(900_000_000)
                return b''
            d.clock.advance(50_000_000)
            raise RuntimeError('production receive failed / unhealthy stream')
        driver.read_callback = receive
        code, result, events = self.run_endpoint(driver, clock, role='mixed_probe')
        self.assertEqual(code, 1)
        self.assertFalse(result['completed'])
        self.assert_probe_accounting(result, events)
        self.assertEqual(result['counters']['received_bytes'], 16)
        self.assertEqual(result['counters']['measured_bytes'], 16)
        self.assertEqual(result['received_sha256'], hashlib.sha256(payload * 2).hexdigest())
        self.assertEqual(result['sent_sha256'], hashlib.sha256(b'').hexdigest())
        self.assertEqual(result['incomplete_record_bytes'], endpoint.H.size + 2)
        self.assertAlmostEqual(result['goodput_mbps'], 16 * 8 / 3_000_000)
        self.assertNotIn('verified_goodput_mbps', result)
        self.assertEqual(result['probe_rtt_ms'], dict(count=1, p50=100, p95=100, p99=100, max=100))
        self.assertIn('survivor-only', result['latency_scope'])
        self.assertEqual(result['counters']['probes_observed'], 2)  # Includes the warmup response.
        self.assertEqual(result['counters']['probes_unserved_after_failure'], 1)
        self.assertEqual(result['probes_pending_at_exit'], 1)
        self.assertEqual(result['probe_timeouts'], 0)
        self.assertEqual(result['terminal']['operation'], 'read')
        self.assertEqual(result['terminal']['since_measurement_start_ms'], 1050)
        self.assertEqual(result['native_stats']['receive_failed'], 1)
        self.assertEqual(result['queue_stats']['fields']['pending_segment_bytes'], 16)
        self.assertTrue(driver.closed)

    def test_initialization_failure_has_no_invented_measurement(self):
        clock = Clock(); driver = Driver(clock)
        code, result, events = self.run_endpoint(driver, clock, constructor_error=RuntimeError('cannot open endpoint'))
        self.assertEqual(code, 1)
        self.assertFalse(result['completed'])
        self.assertEqual(result['terminal']['operation'], 'initialization')
        self.assertIsNone(result['terminal']['since_trial_start_ms'])
        self.assertIsNone(result['measurement_s'])
        self.assertIsNone(result['goodput_mbps'])
        self.assertIsNone(result['native_stats'])
        self.assertEqual(result['counters']['probes_expected'], 0)
        self.assertEqual(result['probes_pending_at_exit'], 0)
        self.assertEqual(result['received_sha256'], hashlib.sha256(b'').hexdigest())
        self.assertFalse(driver.closed)

    def test_snapshot_and_trace_export_errors_do_not_hide_original_failure(self):
        clock = Clock(); driver = Driver(clock); driver.fail_send = True; driver.snapshot_errors = True
        code, result, events = self.run_endpoint(driver, clock, trace=True, trace_write_error=True)
        self.assertEqual(code, 1)
        self.assertFalse(result['completed'])
        self.assertIn('production send failed', result['error'])
        self.assertIsNone(result['native_stats'])
        self.assertIsNone(result['queue_stats'])
        self.assertEqual({e['operation'] for e in result['telemetry_errors']}, {'native_stats', 'queue_stats', 'loss_trace_export'})
        self.assertFalse(result['loss_trace']['valid'])
        self.assert_probe_accounting(result, events)
        self.assertTrue(driver.closed)

    def test_successful_probe_path_keeps_existing_latency_and_counters(self):
        clock = Clock(); driver = Driver(clock)
        code, result, events = self.run_endpoint(driver, clock)
        self.assertEqual(code, 0)
        self.assertTrue(result['completed'])
        self.assertNotIn('terminal', result)
        self.assert_probe_accounting(result, events)
        self.assertEqual(result['counters']['probes_injected'], 4)
        self.assertEqual(result['counters']['probes_admitted'], 4)
        self.assertEqual(result['counters']['probes_observed'], 4)
        self.assertEqual(result['probe_rtt_ms']['count'], 3)
        self.assertEqual(result['probe_rtt_ms']['p99'], 100)
        self.assertEqual(result['probe_timeouts'], 0)
        self.assertEqual(result['measurement_observed_s'], 3)
        self.assertEqual(result['telemetry_errors'], [])
        self.assertTrue(driver.closed)

    def test_unobserved_probes_at_actual_drain_deadline_remain_timeouts(self):
        clock = Clock(); driver = Driver(clock)
        driver.send = lambda data: driver.sent.append(data)
        code, result, events = self.run_endpoint(driver, clock)
        self.assertEqual(code, 0)
        self.assertTrue(result['completed'])
        self.assert_probe_accounting(result, events)
        self.assertEqual(result['probe_timeouts'], 4)
        self.assertEqual(result['probe_unobserved'], 4)
        self.assertEqual(result['counters']['probes_unserved_after_failure'], 0)
        self.assertEqual(len([e for e in events if e['type'] == 'probe_timeout']), 4)
        self.assertFalse([e for e in events if e['type'] == 'probe_unobserved'])

    def test_partial_control_failure_does_not_break_result_finalization(self):
        clock = Clock(); driver = Driver(clock)
        code, result, events = self.run_endpoint(driver, clock, control={'start_ns': 1_000_000_000})
        self.assertEqual(code, 1)
        self.assertFalse(result['completed'])
        self.assertEqual(result['terminal']['operation'], 'control')
        self.assertIsNone(result['goodput_mbps'])
        self.assertEqual(result['counters']['probes_expected'], 0)
        self.assertTrue(driver.closed)

    def test_cleanup_failure_is_retained_without_losing_native_terminal_evidence(self):
        clock = Clock(); driver = Driver(clock); driver.fail_send = True
        def close():
            raise RuntimeError('close failed')
        driver.close = close
        code, result, events = self.run_endpoint(driver, clock)
        self.assertEqual(code, 1)
        self.assertFalse(result['completed'])
        self.assertIn('production send failed', result['error'])
        self.assertEqual(result['native_stats']['send_failed'], 1)
        self.assertEqual(result['cleanup_errors'][0]['operation'], 'driver_close')
        self.assertEqual(result['terminal']['admission'], 'unknown_after_failed_call')

    def test_observer_snapshot_failure_does_not_terminate_healthy_transfer(self):
        clock = Clock(); driver = Driver(clock); driver.snapshot_errors = True
        code, result, events = self.run_endpoint(driver, clock)
        self.assertEqual(code, 0)
        self.assertTrue(result['completed'])
        self.assertEqual(result['counters']['probes_observed'], 4)
        self.assertGreater(len(result['telemetry_errors']), 2)
        self.assertIsNone(result['native_stats'])
        self.assertTrue(driver.closed)


if __name__ == '__main__':
    unittest.main()
