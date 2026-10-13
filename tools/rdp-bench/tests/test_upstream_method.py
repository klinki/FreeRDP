"""Prevent counter absence, process survival, or cross-phase samples becoming success."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import upstream_method as method


def sample(start='2026-10-13T12:00:10Z', end='2026-10-13T12:00:20Z', value=30, instance='rdp-tcp#1', status=0):
    return dict(type='sample',interval_start_utc=start,interval_end_utc=end,
                counters=[dict(metric='server_output_fps',instance=instance,value=value,status=status)])


def campaign():
    return dict(steps=[dict(id='loss25',loss_percent=25,start_utc='2026-10-13T12:00:00Z',
                           end_utc='2026-10-13T12:02:00Z',requested_s=120,
                           client_alive_end=True,outcome='COMPLETE')])


class UpstreamMethodTests(unittest.TestCase):
    def test_missing_counters_are_unknown_and_not_zero(self):
        report=method.summarize(campaign(),[sample()])
        metrics=report['steps'][0]['metrics']
        self.assertEqual(metrics['server_output_fps']['mean'],30)
        self.assertIsNone(metrics['server_udp_bandwidth']['mean'])
        self.assertEqual(metrics['server_udp_bandwidth']['n'],0)
        self.assertIsNone(report['steps'][0]['verified_goodput_mbps'])
        self.assertIsNone(report['steps'][0]['physical_client_fps'])

    def test_ambiguous_counter_instance_is_not_first_match(self):
        row=sample();row['counters'].append(dict(row['counters'][0],instance='rdp-tcp#2',value=2))
        report=method.summarize(campaign(),[row])
        self.assertIsNone(report['steps'][0]['metrics']['server_output_fps']['mean'])
        self.assertIn('ambiguous',report['steps'][0]['counter_errors'][0])
        cfg=campaign();cfg['counter_instance']='rdp-tcp#2'
        self.assertEqual(method.summarize(cfg,[row])['steps'][0]['metrics']['server_output_fps']['mean'],2)

    def test_windows_instance_case_is_equal_without_collapsing_ambiguity(self):
        row=sample(instance='RDP-Tcp 0')
        row['counters'].append(dict(metric='server_udp_bandwidth',instance='RDP-tcp 0',value=4000,status=0))
        cfg=campaign();cfg['counter_instance']='rdp-TCP 0'
        metrics=method.summarize(cfg,[row])['steps'][0]['metrics']
        self.assertEqual(metrics['server_output_fps']['mean'],30)
        self.assertEqual(metrics['server_udp_bandwidth']['mean'],4000)
        row['counters'].append(dict(row['counters'][0],instance='rdp-tcp 0',value=2))
        report=method.summarize(cfg,[row])
        self.assertIsNone(report['steps'][0]['metrics']['server_output_fps']['mean'])
        self.assertIn('ambiguous',report['steps'][0]['counter_errors'][0])

    def test_total_nonzero_status_and_nonfinite_are_not_valid(self):
        for kwargs in (dict(instance='_total'),dict(status=1),dict(value=float('nan')),dict(status=True)):
            self.assertEqual(method.summarize(campaign(),[sample(**kwargs)])['steps'][0]['metrics']['server_output_fps']['n'],0)

    def test_cross_boundary_and_unknown_interval_are_excluded(self):
        overlap=sample(start='2026-10-13T11:59:59Z',end='2026-10-13T12:00:01Z',value=99)
        unknown=dict(type='sample',utc='2026-10-13T12:00:21Z',counters=sample()['counters'])
        report=method.summarize(campaign(),[overlap,unknown,sample()])
        step=report['steps'][0]
        self.assertEqual(step['cross_boundary_samples'],1)
        self.assertEqual(step['metrics']['server_output_fps']['n'],1)
        self.assertIn('interval start unknown',step['counter_errors'][0])

    def test_ended_and_unreached_steps_are_retained(self):
        cfg=campaign();cfg['steps'][0].update(end_utc='2026-10-13T12:00:30Z',outcome='SESSION_ENDED',client_alive_end=False)
        cfg['steps'].append(dict(id='loss50',loss_percent=50,outcome='NOT_RUN',requested_s=120))
        cfg['steps'].append(dict(id='loss70',loss_percent=70,outcome='NOT_RUN',requested_s=120))
        report=method.summarize(cfg,[sample()])
        self.assertEqual([step['outcome'] for step in report['steps']],['SESSION_ENDED','NOT_RUN','NOT_RUN'])
        self.assertEqual(report['steps'][0]['observed_duration_s'],30)

    def test_false_complete_requires_duration_and_process_survival(self):
        for change in (dict(client_alive_end=None),dict(client_alive_end=False),dict(end_utc='2026-10-13T12:01:00Z')):
            cfg=campaign();cfg['steps'][0].update(change)
            self.assertEqual(method.summarize(cfg,[])['steps'][0]['outcome'],'INCOMPLETE')

    def test_bad_artifacts_survive_report_and_overlap_is_rejected(self):
        report=method.summarize(campaign(),[sample(end='bad'),sample()],parse_errors=[dict(line=5,error='bad JSON')])
        self.assertEqual(len(report['artifact_errors']),2)
        cfg=campaign();cfg['steps'].append(dict(cfg['steps'][0],id='loss50',loss_percent=50))
        with self.assertRaisesRegex(ValueError,'overlap'):
            method.summarize(cfg,[])

    def test_plan_freezes_sources_and_preserves_method_scope(self):
        with tempfile.TemporaryDirectory() as folder:
            for name in method.SOURCE_FILES:(Path(folder)/name).write_text(name)
            plan=method.plan(folder)
            self.assertEqual(plan['requested_loss_percent'],[25,50,70])
            self.assertEqual(plan['network']['protocol_scope'],'all IPv4 from VM')
            self.assertEqual(plan['network']['large_packet_reorder_percent'],70)
            self.assertIsNone(plan['network']['rate_cap_mbps'])
            self.assertEqual(len(plan['source_files']),6)
            self.assertTrue(all(len(row['sha256'])==64 for row in plan['source_files']))

    def test_continuous_counter_stream_keeps_every_rate_and_lost_provider_event(self):
        baseline=dict(sample(start='2026-10-13T12:00:00Z',end='2026-10-13T12:00:00Z',value=99),
                      type='baseline',interval_start_utc=None,baseline=True)
        first=sample(start='2026-10-13T12:00:00Z',end='2026-10-13T12:00:10Z',value=30)
        second=sample(start='2026-10-13T12:00:10Z',end='2026-10-13T12:00:20Z',value=20)
        lost=dict(type='collector_error',utc='2026-10-13T12:00:30Z',error='No data to return')
        ended=dict(type='collector_end',utc='2026-10-13T12:00:30Z',observed_counter_sets=3)
        report=method.summarize(campaign(),[baseline,first,second,lost,ended])
        metric=report['steps'][0]['metrics']['server_output_fps']
        self.assertEqual(metric['n'],2)
        self.assertEqual(metric['mean'],25)
        self.assertEqual(report['counter_diagnostics'],[baseline,lost,ended])

    def test_client_exit_before_ten_second_interval_does_not_promote_baseline(self):
        cfg=campaign();cfg['steps'][0].update(end_utc='2026-10-13T12:00:08Z',
                                            outcome='SESSION_ENDED',client_alive_end=False)
        baseline=dict(sample(end='2026-10-13T12:00:00Z',value=99),type='baseline',interval_start_utc=None)
        completed_after_exit=sample(start='2026-10-13T12:00:00Z',end='2026-10-13T12:00:10Z',value=30)
        report=method.summarize(cfg,[baseline,completed_after_exit])
        self.assertEqual(report['steps'][0]['metrics']['server_output_fps']['n'],0)
        self.assertIsNone(report['steps'][0]['metrics']['server_output_fps']['mean'])
        self.assertEqual(report['steps'][0]['outcome'],'SESSION_ENDED')

    def test_collector_contract_has_one_stream_and_records_baseline_and_cadence(self):
        source=(ROOT/'upstream-method/Collect-Counters.ps1').read_text()
        self.assertEqual(source.count('Get-Counter -Counter'),1)
        self.assertIn('-SampleInterval $Interval -MaxSamples $maxSets',source)
        self.assertNotIn('-MaxSamples 2',source)
        self.assertNotIn('Start-Sleep',source)
        self.assertIn("type=if($baseline) {'baseline'} else {'sample'}",source)
        self.assertIn('interval_start_utc=if(!$baseline) {$previousTimestamp',source)
        self.assertIn('collector_sha256=',source)
        self.assertIn('observed_interval_s=$span',source)
        self.assertIn("type='collector_error'",source)
        self.assertIn('raw_cooked_value=$raw;valid_for_rate=$valid',source)

    def test_csv_and_json_retain_unknown_fields(self):
        report=method.summarize(campaign(),[])
        with tempfile.TemporaryDirectory() as folder:
            prefix=Path(folder)/'result';method.write_report(prefix,report)
            loaded=json.loads(prefix.with_suffix('.json').read_text())
            self.assertIsNone(loaded['steps'][0]['metrics']['server_output_fps']['mean'])
            self.assertIn('server_output_fps,0,,,,',prefix.with_suffix('.csv').read_text())
            self.assertIn('unknown',prefix.with_suffix('.md').read_text())


if __name__=='__main__':unittest.main()
