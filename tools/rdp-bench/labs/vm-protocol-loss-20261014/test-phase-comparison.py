"""Offline fixtures only. Execute after all timed phases have closed."""
import copy
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC=importlib.util.spec_from_file_location('phase_comparison',Path(__file__).with_name('compare-protocol-phases.py'))
m=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(m)


def trial(engine='pr',commit=m.OLD_PR,protocol='udp',loss=70):
    return dict(trial_id='test-'+engine,phase_id='baseline',engine=engine,protocol=protocol,loss_percent=loss,
                performance_eligible=True,source_comparison_eligible=True,
                source_identity=dict(expected_commit=commit,recorded_commit=commit,commit_matches=True,
                                     binary_matches=True,recorded_binary_sha256='same',
                                     source_hashes_match_phase=True,actual_source_hashes={'core':'same'}),
                artifact_issues=[],requested_s=120,trial_provenance={'warmup':{'minimum_requested_s':45,'observed_host_monotonic_s':45.01}},
                trial_result_receipt={'errors':[],'cleanup_errors':[]},
                metrics={'server_output_fps':{'mean':4.,'median':4.,'n':3}},
                protocol_receipt={'selector_matches':True,'nonselected_bypass':{'dropped_packets':0}},
                outcome='SESSION_ENDED',observed_duration_s=43.,survived_requested_interval=False,
                raw_provider_error_event_count=2,counter_error_event_count=1,
                counter_report={'collector_terminal_receipt_present':True},capture_status={},
                **{key:None for key in m.UNKNOWN})


class ComparisonTests(unittest.TestCase):
    def test_failed_prefix_retained_not_survivor_pooled(self):
        a=trial();b=trial(commit=m.FIXED_PR);b.update(outcome='COMPLETE',observed_duration_s=120.,survived_requested_interval=True)
        b['metrics']['server_output_fps'].update(mean=3.,n=11)
        row=m.compare_row(('udp','pr',70),a,b,external_controls_qualified=True)
        self.assertTrue(row['comparison_eligible']);self.assertEqual(row['metrics']['server_output_fps']['after_minus_before_mean'],-1.)
        self.assertEqual(row['before']['observed_duration_s'],43.);self.assertEqual(row['after']['metrics']['server_output_fps']['n'],11)
        self.assertIn('shorter durations',row['qualification'])

    def test_missing_cleanup_or_bypass_drops_withhold_delta(self):
        a=trial();b=trial(commit=m.FIXED_PR)
        b['trial_result_receipt']['cleanup_errors']=None
        self.assertFalse(m.compare_row(('udp','pr',70),a,b,external_controls_qualified=True)['comparison_eligible'])
        b['trial_result_receipt']['cleanup_errors']=[];b['protocol_receipt']['nonselected_bypass']['dropped_packets']=1
        self.assertIsNone(m.compare_row(('udp','pr',70),a,b,external_controls_qualified=True)['metrics']['server_output_fps']['after_minus_before_mean'])

    def test_local_control_must_remain_identical(self):
        a=trial('local',m.LOCAL);b=copy.deepcopy(a);b['source_identity']['recorded_binary_sha256']='different'
        with self.assertRaisesRegex(ValueError,'local control binary'):
            m.compare_row(('udp','local',70),a,b,external_controls_qualified=True)

    def test_unknown_provider_error_is_not_zero(self):
        a=trial();a['raw_provider_error_event_count']=None
        audit=m.audit([a]);self.assertIsNone(audit['raw_provider_errors']);self.assertEqual(audit['provider_error_unknown_trials'],1)
        self.assertEqual(audit['admitted_phase_provider_errors'],1)
        a['process_bound_counter_report']={'steps':[{'counter_error_event_count':None}]}
        self.assertIsNone(m.audit([a])['original_process_bound_phase_provider_errors'])

    def test_graceful_wait_timeout_retained_despite_later_footer(self):
        t=trial();t['owned_windows_cleanup_receipt']={'collector_stop':{'status':'GRACEFUL_STOP_TIMEOUT',
                         'elapsed_s':15.041,'fallback_owned_cleanup_required':True}}
        life=m.lifecycle(t);self.assertEqual(life['collector_stop_status'],'GRACEFUL_STOP_TIMEOUT')
        self.assertTrue(life['cleanup_qualified'])
        audit=m.audit([t]);self.assertEqual(audit['graceful_collector_timeouts'],1)
        self.assertEqual(audit['missing_collector_footers'],0);self.assertEqual(audit['known_fallback_requests'],1)

    def test_missing_metric_has_no_delta(self):
        a=trial();b=trial(commit=m.FIXED_PR);b['metrics']['server_output_fps'].update(mean=None,n=0)
        self.assertIsNone(m.compare_row(('udp','pr',70),a,b,external_controls_qualified=True)['metrics']['server_output_fps']['after_minus_before_mean'])

    def test_sampled_stalls_bounded_and_not_fps(self):
        rows=[]
        for i in range(5):
            rows.append(dict(capture_end_ns=i*1_000_000_000,compared_previous_capture_end_ns=(i-1)*1_000_000_000,
                             changed_pixels=100 if i==1 else 0))
        result=m.sampled_stalls(rows)
        self.assertEqual(result['changed_fraction'],.25);self.assertEqual(result['longest_consecutive_unchanged_s'],3.)
        self.assertEqual(result['trailing_consecutive_unchanged_s'],3.);self.assertIn('not physical FPS',result['qualification'])
        rows[3]['changed_pixels']=None
        self.assertIsNone(m.sampled_stalls(rows)['longest_consecutive_unchanged_s'])

    def test_unlinked_comparisons_do_not_extend_stall(self):
        rows=[dict(capture_end_ns=i*1_000_000_000,compared_previous_capture_end_ns=(i-1)*1_000_000_000,changed_pixels=0) for i in range(5)]
        rows[2]['compared_previous_capture_end_ns']=None
        result=m.sampled_stalls(rows)
        self.assertEqual(result['comparison_intervals'],3);self.assertEqual(result['longest_consecutive_unchanged_s'],2.)

    def test_unlinked_final_sample_does_not_invent_trailing_run(self):
        rows=[dict(capture_end_ns=i*1_000_000_000,compared_previous_capture_end_ns=(i-1)*1_000_000_000,changed_pixels=0) for i in range(5)]
        rows[-1]['compared_previous_capture_end_ns']=None
        result=m.sampled_stalls(rows)
        self.assertEqual(result['longest_consecutive_unchanged_s'],3.)
        self.assertIsNone(result['trailing_consecutive_unchanged_s'])

    def test_unknown_common_coordinator_withholds_current_delta(self):
        controls=m.same_coordinator({'baseline':{'campaign_execution':{'coordinator_sha256':'a'*64}},'fixed':{}})
        self.assertFalse(controls['qualified'])
        row=m.compare_row(('udp','pr',70),trial(),trial(commit=m.FIXED_PR),external_controls_qualified=controls['qualified'])
        self.assertFalse(row['comparison_eligible']);self.assertIsNone(row['metrics']['server_output_fps']['after_minus_before_mean'])

    def test_terminal_teardown_sample_excluded_and_alive_stall_flagged(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'images.jsonl'
            rows=[dict(type='sample',capture_start_ns=i*1_000_000_000,capture_end_ns=i*1_000_000_000+1,
                       capture_end_utc_ns_estimate=10_000_000_000+i*1_000_000_000,
                       compared_previous_capture_end_ns=(i-1)*1_000_000_000+1,changed_pixels=0 if i<4 else 100) for i in range(5)]
            path.write_text('\n'.join(json.dumps(row) for row in rows))
            t=trial();t['survived_requested_interval']=True
            t['raw_campaign']={'steps':[{'start_monotonic_s':0.,'end_monotonic_s':5.}]}
            t['visual_progress']={'actual_phase_progress':{'sample_file':str(path),'parse_errors':[]},
                                  'before_logged_terminal_progress':{'cutoff_utc_ns':14_000_000_000}}
            m.add_visual_stalls(t)
            stalls=t['visual_progress']['sampled_stalls']
            self.assertEqual(stalls['samples'],4);self.assertEqual(stalls['changed_intervals'],0)
            self.assertTrue(t['visual_progress']['surviving_visual_attention'])
            self.assertIn('not an automatic transport failure',t['visual_progress']['attention_qualification'])

    def test_diagnostics_and_invented_tails_refused(self):
        trials=[trial(e,m.LOCAL if e=='local' else m.OLD_PR,'udp',loss) for e in m.ENGINES for loss in m.LOSSES]
        report=dict(trial_kind='performance',campaign_execution={'finished_utc':'closed'},
                    run_configuration={'packet_capture_enabled':False,'trace_enabled':False},trials=trials)
        self.assertEqual(len(m.phase_trials(report,('udp',),dict(local=m.LOCAL,pr=m.OLD_PR))),8)
        report['trial_kind']='diagnostic'
        with self.assertRaisesRegex(ValueError,'diagnostics'):m.phase_trials(report,('udp',),dict(local=m.LOCAL,pr=m.OLD_PR))
        report['trial_kind']='performance';trials[0]['transport_probe_rtt_p99_ms']=12.
        with self.assertRaisesRegex(ValueError,'does not establish'):m.phase_trials(report,('udp',),dict(local=m.LOCAL,pr=m.OLD_PR))

    def test_machine_exports_keep_unknown_tails_blank_and_failed_prefix(self):
        a=trial();b=trial(commit=m.FIXED_PR)
        for t in (a,b):
            t['network']={'small':{'transmitted_packets':2,'dropped_packets':1,'drop_fraction_completed_attempts':1/3},
                          'large':{'transmitted_packets':4,'dropped_packets':6,'drop_fraction_completed_attempts':.6}}
        pair=m.compare_row(('udp','pr',70),a,b,external_controls_qualified=True)
        rows=[dict(m.brief(t),protocol='udp',engine='pr',loss_percent=70) for t in (a,b)]
        with tempfile.TemporaryDirectory() as temp:
            prefix=Path(temp)/'report';m.write(prefix,dict(trials=rows,comparisons=[pair]))
            data=json.loads(prefix.with_suffix('.json').read_text())
            self.assertIsNone(data['trials'][0]['verified_goodput_mbps'])
            with prefix.with_suffix('.csv').open() as stream:csv_rows=list(csv.DictReader(stream))
            self.assertEqual(len(csv_rows),2);self.assertEqual(csv_rows[0]['observed_duration_s'],'43.0')
            self.assertEqual(csv_rows[0]['transport_probe_rtt_p99_ms'],'')
            self.assertEqual(csv_rows[0]['server_output_fps_n'],'3')


if __name__=='__main__':unittest.main()
