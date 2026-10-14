"""Offline phase/protocol/unknown-counter contracts; run between campaigns."""
import copy
import importlib.util
from pathlib import Path
import unittest
import json
import tempfile
from datetime import datetime,timedelta

BASE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('protocol_loss_report_test', BASE / 'report-protocol-loss.py')
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def index():
    return dict(phase_id='baseline', protocols=['tcp', 'udp'], loss_percent=[0, 25, 50, 70],
                variants=['local', 'pr'], trial_kind='performance',
                trials=[dict(trial_id=f'{protocol}-{engine}-{loss}', phase_id='baseline',
                             protocol=protocol, engine=engine, loss_percent=loss)
                        for protocol in ('tcp', 'udp') for loss in (0, 25, 50, 70)
                        for engine in ('local', 'pr')])


class ProtocolReportTests(unittest.TestCase):
    def test_sixteen_conditions_and_phase_isolation(self):
        value = index()
        self.assertEqual(report.validate_index(value, {'phase_id': 'baseline'}), ('baseline', False))
        with self.assertRaises(ValueError):
            report.validate_index(value, {'phase_id': 'fixed'})
        value['trials'][0]['phase_id'] = 'fixed'
        with self.assertRaises(ValueError):
            report.validate_index(value, {'phase_id': 'baseline'})

    def test_missing_and_duplicate_conditions_are_not_dropped(self):
        value = index(); value['trials'].pop()
        with self.assertRaises(ValueError):
            report.validate_index(value, {'phase_id': 'baseline'})
        value = index(); value['trials'][0] = copy.deepcopy(value['trials'][1])
        with self.assertRaises(ValueError):
            report.validate_index(value, {'phase_id': 'baseline'})

    def test_separate_all_ipv4_eight_conditions(self):
        value = index(); value['protocols'] = ['all']
        value['trials'] = [dict(trial_id=f'all-{engine}-{loss}', phase_id='baseline',
                               protocol='all', engine=engine, loss_percent=loss)
                          for loss in (0, 25, 50, 70) for engine in ('local', 'pr')]
        self.assertEqual(report.validate_index(value, {'phase_id': 'baseline'}), ('baseline', False))

    def test_single_trace_diagnostic_is_never_performance_matrix(self):
        for protocol in ('udp', 'all'):
            value = dict(phase_id='diag', protocols=[protocol], loss_percent=[70], variants=['pr'],
                         trial_kind='diagnostic', trials=[dict(trial_id='one', phase_id='diag', protocol=protocol,
                                                               engine='pr', loss_percent=70)])
            self.assertEqual(report.validate_index(value, {'phase_id': 'diag'}), ('diag', True))
            value['trial_kind'] = 'performance'
            with self.assertRaises(ValueError):
                report.validate_index(value, {'phase_id': 'diag'})

    def test_bypass_counter_unknown_is_distinct_from_zero(self):
        unknown = report.delta({}, {})
        self.assertIsNone(unknown['dropped_packets'])
        self.assertIsNone(unknown['drop_fraction_completed_attempts'])
        a = dict(transmitted_packets=0, dropped_packets=0, backlog_packets=0)
        b = dict(transmitted_packets=20, dropped_packets=0, backlog_packets=2)
        known = report.delta(a, b)
        self.assertEqual(known['dropped_packets'], 0)
        self.assertEqual(known['drop_fraction_completed_attempts'], 0)
        self.assertEqual(known['backlog_after_packets'], 2)
        with self.assertRaises(ValueError):
            report.delta(b, a)

    def test_actual_selector_mismatch_rejects_false_protocol_label(self):
        trial = dict(trial_id='one', raw_campaign={'steps': [dict(start_utc='stamp', network={
            'qdisc_before': {'state': {'loss_protocol': 'tcp'}},
            'qdisc_after': {'state': {'loss_protocol': 'tcp'}}})]})
        with self.assertRaises(ValueError):
            report.protocol_receipt(trial, 'udp')

    def test_native_kind_metadata_footer_are_not_counted_as_events(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            metadata = {'kind':'metadata','schema':'freerdp-pr13608-recovery-v1','total_events':1,'overwritten_events':0}
            footer = {'kind':'footer','retained_sequence_valid':True,'complete_from_arm':True,'overwritten_events':0}
            rows = [metadata, {'kind':'event','id':1,'event':'rx_data'}, footer]
            (path/'recovery-trace.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
            result = report.trace_receipt(path)
            self.assertEqual(result['metadata'],metadata)
            self.assertEqual(result['footer'],footer)
            self.assertEqual(result['retained_event_rows'],1)
            self.assertEqual(result['parse_errors'],0)

    def test_near_boundary_fatal_is_separate_from_poll_duration(self):
        trial=dict(outcome='SESSION_ENDED',client_alive_end=False,observed_duration_s=120.005,
                   requested_s=120,raw_campaign={'steps':[{'start_utc':'2026-10-14T13:29:20.565802+00:00'}]},
                   udp_tunnel={'first_measured_terminal':{'kind':'MAIN_TRANSPORT_TIMEOUT',
                       'linux_utc':'2026-10-14T13:31:20.350000+00:00','timestamp_precision_ms':1,
                       'timestamp_basis':'date inferred from trial and WLog clock in UTC Docker environment'}})
        receipt=report.terminal_timing(trial)
        self.assertTrue(receipt['demonstrably_before_requested_end'])
        self.assertAlmostEqual(receipt['logged_terminal_relative_s'],119.784198)
        self.assertEqual(receipt['poll_detected_duration_s'],120.005)
        self.assertEqual(trial['observed_duration_s'],120.005)
        trial['outcome']='COMPLETE'
        self.assertIsNone(report.terminal_timing(trial))
        trial['outcome']='SESSION_ENDED'
        trial['udp_tunnel']['first_measured_terminal']['linux_utc']='2026-10-14T13:31:20.565500+00:00'
        self.assertFalse(report.terminal_timing(trial)['demonstrably_before_requested_end'])

    def test_fatal_bound_withholds_late_counter_preserves_raw_and_duration(self):
        spec=importlib.util.spec_from_file_location('terminal_bound_method',report.counter.REPORTER)
        method=importlib.util.module_from_spec(spec);spec.loader.exec_module(method)
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp);start=datetime.fromisoformat('2026-10-14T13:29:20.565802+00:00')
            raw={'steps':[{'id':'one','loss_percent':70,'requested_s':120,'start_utc':start.isoformat(),
                          'end_utc':(start+timedelta(seconds=120.005)).isoformat(),'outcome':'SESSION_ENDED',
                          'client_alive_end':False,'observed_duration_s':120.005}]}
            (directory/'session-discovery.json').write_text(json.dumps({'counter_instance':'RDP-Tcp 0','counter_instance_evidence':'CORROBORATED'}))
            rows=[{'type':'sample','interval_start_utc':(start+timedelta(seconds=a)).isoformat(),
                   'interval_end_utc':(start+timedelta(seconds=b)).isoformat(),
                   'counters':[{'metric':'server_output_fps','instance':'RDP-tcp 0','status':0,'value':v}]}
                  for a,b,v in [(10,20,8),(110,119.9,100)]]
            rows.append({'type':'collector_error','utc':(start+timedelta(seconds=119.95)).isoformat(),'error':'lost provider after fatal'})
            (directory/'counters.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
            alignment={'status':'RECORDED','method':'fixture','windows_minus_linux_s':0,'uncertainty_s':.02,'uncertainty_ms':20}
            provenance={'counter_instance':'RDP-Tcp 0'}
            original=report.counter.counter_report(directory,raw,provenance,alignment,method,[])
            trial=dict(outcome='SESSION_ENDED',client_alive_end=False,observed_duration_s=120.005,requested_s=120,
                       raw_campaign=raw,counter_report=original,clock_alignment=alignment,trial_provenance=provenance,
                       artifact_issues=[],udp_tunnel={'first_measured_terminal':{'kind':'MAIN_TRANSPORT_TIMEOUT',
                       'linux_utc':'2026-10-14T13:31:20.350000+00:00','timestamp_precision_ms':1,
                       'timestamp_basis':'date inferred from trial and WLog clock in UTC Docker environment'}})
            report.refine_terminal_bounds(trial,directory,method)
            self.assertFalse(trial['survived_requested_interval']);self.assertEqual(trial['observed_duration_s'],120.005)
            self.assertEqual(trial['metrics']['server_output_fps']['n'],1);self.assertEqual(trial['metrics']['server_output_fps']['mean'],8)
            self.assertEqual(trial['process_bound_counter_report']['steps'][0]['metrics']['server_output_fps']['n'],2)
            self.assertEqual(trial['counter_report']['raw_counter_rows'],rows)
            self.assertEqual(trial['counter_report']['raw_provider_error_event_count'],1)
            self.assertEqual(trial['counter_error_event_count'],0)


if __name__ == '__main__':
    unittest.main()
