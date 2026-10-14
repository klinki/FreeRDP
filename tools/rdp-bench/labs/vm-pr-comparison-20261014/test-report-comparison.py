"""Offline counter-boundary and visual-prefix regressions; run after campaign."""
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

BASE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('vm_comparison_report', BASE / 'report-comparison.py')
report = importlib.util.module_from_spec(spec); spec.loader.exec_module(report)
spec = importlib.util.spec_from_file_location('upstream_method_test', report.REPORTER)
method = importlib.util.module_from_spec(spec); spec.loader.exec_module(method)


def put(directory, name, value):
    (directory / name).write_text(json.dumps(value) + '\n')


class ComparisonReportTests(unittest.TestCase):
    def test_nested_before_after_clock_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, offset, error in [('clock-before.json', .04, .005), ('clock-after.json', .05, .008)]:
                put(root, name, {'selected': {'offset_windows_minus_host_s': offset, 'uncertainty_s': error}})
            for name, offset, error in [('linux-clock-before.json', .01, .001), ('linux-clock-after.json', .02, .002)]:
                put(root, name, {'selected': {'offset_linux_minus_host_s': offset, 'uncertainty_s': error}})
            result = report.clocks(root, {}, [])
            self.assertEqual(result['status'], 'RECORDED')
            self.assertAlmostEqual(result['windows_minus_linux_s'], .03)
            self.assertAlmostEqual(result['uncertainty_ms'], 20)
            (root / 'linux-clock-after.json').unlink()
            self.assertEqual(report.clocks(root, {}, [])['status'], 'UNKNOWN')

    def test_counter_boundary_exclusion_and_corroborated_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            start = datetime(2026, 10, 14, tzinfo=timezone.utc)
            campaign = {'steps': [{'id': 'loss50', 'loss_percent': 50, 'requested_s': 120,
                                  'start_utc': start.isoformat(), 'end_utc': (start + timedelta(seconds=120)).isoformat(),
                                  'outcome': 'COMPLETE', 'client_alive_end': True}]}
            selector = {'counter_instance': 'RDP-Tcp 0', 'counter_instance_evidence': 'CORROBORATED'}
            put(root, 'session-discovery.json', selector)
            rows = [{'type': 'sample', 'interval_start_utc': (start + timedelta(seconds=a)).isoformat(),
                     'interval_end_utc': (start + timedelta(seconds=b)).isoformat(), 'counters': [
                         {'metric': 'server_output_fps', 'instance': 'RDP-tcp 0', 'status': 0, 'value': fps}]}
                    for a, b, fps in [(0.04, 10.04, 100), (10.04, 20.04, 8), (20.04, 30.04, 10)]]
            (root / 'counters.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in rows))
            alignment = {'status': 'RECORDED', 'method': 'fixture UTC brackets',
                         'windows_minus_linux_s': .03, 'uncertainty_s': .02, 'uncertainty_ms': 20}
            result = report.counter_report(root, campaign, {'counter_instance': 'RDP-Tcp 0'}, alignment, method, [])
            self.assertEqual(len(result['boundary_exclusions']), 1)
            fps = result['steps'][0]['metrics']['server_output_fps']
            self.assertEqual(fps['n'], 2)
            self.assertEqual(fps['mean'], 9)
            campaign['steps'][0]['end_utc'] = (start + timedelta(seconds=119.995)).isoformat()
            campaign['steps'][0]['observed_duration_s'] = 120.002
            result = report.counter_report(root, campaign, {'counter_instance': 'RDP-Tcp 0'}, alignment, method, [])
            self.assertEqual(result['steps'][0]['outcome'], 'COMPLETE')
            self.assertEqual(result['steps'][0]['upstream_reporter_outcome_before_monotonic_qualification'], 'INCOMPLETE')
            self.assertEqual(result['steps'][0]['counter_errors'], [])
            error = {'type': 'collector_error', 'utc': (start + timedelta(seconds=130)).isoformat(),
                     'error': 'The data is not valid.'}
            with (root / 'counters.jsonl').open('a') as stream:
                stream.write(json.dumps(error) + '\n')
            result = report.counter_report(root, campaign, {'counter_instance': 'RDP-Tcp 0'}, alignment, method, [])
            self.assertEqual(result['steps'][0]['counter_error_event_count'], 0)
            self.assertEqual(result['raw_provider_error_event_count'], 1)
            self.assertEqual(result['steps'][0]['all_provider_error_events_with_phase'][0]['phase'], 'AFTER_MEASUREMENT')
            self.assertIn(error, result['raw_counter_rows'])
            selector['counter_instance_evidence'] = 'NO_MATCHING_PDH_INSTANCE'
            put(root, 'session-discovery.json', selector)
            result = report.counter_report(root, campaign, {'counter_instance': 'RDP-Tcp 0'}, alignment, method, [])
            self.assertFalse(result['phase_assignment_eligible'])
            self.assertEqual(result['steps'][0]['metrics']['server_output_fps']['n'], 0)
            self.assertIsNone(result['steps'][0]['metrics']['server_output_fps']['mean'])

    def test_visual_prefix_excludes_post_exit_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            summary = {'observer_valid': True, 'completed_measurement_window': False, 'sampling_complete': False,
                       'first_capture_end_ns': 115, 'last_capture_end_ns': 225, 'substantial_change_intervals': 2}
            put(root, 'image-progress.summary.json', summary)
            rows = [
                {'type': 'sample', 'capture_start_ns': 110, 'capture_end_ns': 115,
                 'compared_previous_capture_end_ns': None, 'changed_pixels': None, 'substantial_change': None},
                {'type': 'sample', 'capture_start_ns': 150, 'capture_end_ns': 155,
                 'compared_previous_capture_end_ns': 115, 'changed_pixels': 10, 'substantial_change': True},
                {'type': 'sample', 'capture_start_ns': 220, 'capture_end_ns': 225,
                 'compared_previous_capture_end_ns': 155, 'changed_pixels': 10, 'substantial_change': True},
            ]
            (root / 'image-progress.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in rows))
            result = report.visual(root, {'start_monotonic_s': 100e-9, 'end_monotonic_s': 200e-9,
                                          'observed_duration_s': 100e-9, 'outcome': 'SESSION_ENDED'}, [])
            self.assertEqual(result['status'], 'PARTIAL_SAMPLED_PROGRESS')
            self.assertEqual(result['actual_phase_progress']['samples'], 2)
            self.assertEqual(result['actual_phase_progress']['substantial_change_intervals'], 1)
            self.assertEqual(len(result['actual_phase_progress']['excluded_samples']), 1)
            self.assertFalse(result['capture_bounds_within_actual_phase'])

    def test_drop_fraction_and_missing_counter_stay_distinct(self):
        before = {'netem_counters': {'small': {'transmitted_packets': 10, 'dropped_packets': 5, 'backlog_packets': 1}}}
        after = {'netem_counters': {'small': {'transmitted_packets': 20, 'dropped_packets': 15, 'backlog_packets': 3}}}
        result = report.network({'network': {'qdisc_before': before, 'qdisc_after': after}})
        self.assertEqual(result['small']['drop_fraction_completed_attempts'], .5)
        self.assertEqual(result['small']['backlog_after_packets'], 3)
        self.assertIsNone(result['large']['drop_fraction_completed_attempts'])
        self.assertIsNone(result['large']['transmitted_packets'])

    def test_logged_terminal_cutoff_keeps_black_transition_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            put(root, 'image-progress.summary.json', {
                'observer_valid': True, 'completed_measurement_window': False, 'sampling_complete': False,
                'first_capture_end_ns': 115_000_000_000, 'last_capture_end_ns': 190_000_000_000})
            rows = [dict(type='sample', capture_start_ns=a * 1_000_000_000, capture_end_ns=b * 1_000_000_000,
                         capture_end_utc_ns_estimate=b * 1_000_000_000,
                         compared_previous_capture_end_ns=previous * 1_000_000_000 if previous else None,
                         changed_pixels=10 if previous else None, substantial_change=True if previous else None)
                    for a, b, previous in [(110, 115, None), (150, 155, 115), (185, 190, 155)]]
            (root / 'image-progress.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in rows))
            (root / 'image-progress.last.ppm').write_bytes(b'P6\n1 1\n255\n\0\0\0')
            transport = {'first_measured_terminal': {'linux_utc': '1970-01-01T00:03:00+00:00', 'kind': 'MAIN_TRANSPORT_TIMEOUT'}}
            result = report.visual(root, {'start_monotonic_s': 100, 'end_monotonic_s': 200,
                                          'observed_duration_s': 100, 'outcome': 'SESSION_ENDED'}, [], transport)
            self.assertEqual(result['actual_phase_progress']['samples'], 3)
            self.assertEqual(result['before_logged_terminal_progress']['samples'], 2)
            self.assertEqual(result['before_logged_terminal_progress']['changed_intervals'], 1)
            self.assertEqual(result['before_logged_terminal_progress']['samples_at_or_after_terminal_or_without_utc'], 1)
            self.assertTrue(result['last_saved_frame_all_black'])

    def test_post_phase_udp_error_is_not_measured_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'client.log').write_text(
                '[00:00:01:000] [INFO] UDP multitransport tunnel ready\n'
                '[00:02:05:000] [WARN] RDP-UDP connection failed: cleanup\n')
            result = report.tunnel(root, {'start_utc': '2026-10-14T00:00:00+00:00',
                                         'end_utc': '2026-10-14T00:02:00+00:00'}, [])
            self.assertTrue(result['failure_log_observed'])
            self.assertFalse(result['measured_udp_failure_logged'])
            self.assertIsNone(result['first_measured_terminal'])
            self.assertEqual(result['classified_terminal_events'][0]['phase'], 'AFTER_MEASUREMENT')

    def test_permission_error_does_not_establish_zero_capture_drops(self):
        result = report.capture_status({'offered': {'error': 'PermissionError'}, 'received': {'error': 'PermissionError'}})
        self.assertEqual(result['status'], 'UNKNOWN')
        self.assertIsNone(result['kernel_drops'])
        self.assertIsNone(result['wire_evidence_valid'])
        self.assertEqual(len(result['errors']), 2)

    def test_disabled_capture_empty_map_is_not_failed_or_zero_drop_capture(self):
        trial = {'capture_health': {}, 'artifacts': {'capture-health.json': {}}}
        result = report.disabled_capture_status(trial)
        self.assertEqual(result['status'], 'NOT_REQUESTED')
        self.assertIsNone(result['kernel_drops'])
        trial['capture_health'] = {'offered': {'error': 'PermissionError'}}
        with self.assertRaises(ValueError):
            report.disabled_capture_status(trial)
        trial['capture_health'] = {}
        trial['artifacts']['offered.pcap'] = {}
        with self.assertRaises(ValueError):
            report.disabled_capture_status(trial)


if __name__ == '__main__':
    unittest.main()
