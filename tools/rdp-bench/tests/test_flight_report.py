"""Sealed fixtures exercise matrix admission, censoring, and interruption retention."""
import copy
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from flight_report import collect,dashboard,flight_fields,inspect_trial,relay_counter_values
from flight_sweep import execute,prepare,validate_matrix
from rdpbench.common import digest,read_json,seal,sha256,write_json
from rdpbench.native import FLIGHT_FIELDS


class FlightReportTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name)/'campaign';self.library=Path(temporary.name)/'native.dylib'
        self.library.write_bytes(b'opaque native artifact, not executed')
        self.plan=prepare(self.root,self.library,dict(commit='fixture',binary_sha256=sha256(self.library)))

    @staticmethod
    def endpoint(index,window,missed=0,p95=10,p99=20):
        a=hashlib.sha256(b'a').hexdigest();b=hashlib.sha256(b'b').hexdigest()
        fields=dict(zip(FLIGHT_FIELDS,(1,window,window,127,127,128,127,7,7,131072,window,window+1,window,window+1,window,1)))
        expected=700 if index==0 else 0;observed=expected-missed if index==0 else 0
        sent,received=(16384,0) if index==0 else (0,16384)
        percentiles=dict(count=600 if index==0 else 0,p50=p95,p95=p95,p99=p99,max=p99)
        return dict(completed=True,role='bulk_probe' if index==0 else 'sink',
                    counters=dict(sent_bytes=sent,received_bytes=received,measured_bytes=received,
                                  probes_expected=expected,probes_injected=observed,probes_observed=observed,
                                  probes_not_injected=missed if index==0 else 0,probe_send_failures=0,unexpected_order=0),
                    sent_sha256=a if index==0 else b,received_sha256=b if index==0 else a,
                    native_stats=dict(send_failed=0,receive_failed=0),benchmark_deadline_reached=False,
                    flight_config=dict(version=1,fields=fields),queue_stats=dict(valid=True,fields=dict(max_payload_bytes=1150)),
                    incomplete_record_bytes=0,probe_timeouts=0,probe_unobserved=0,
                    probe_rtt_ms=percentiles,probe_scheduled_ms=dict(percentiles),
                    measurement_observed_s=30,goodput_mbps=received*8/30/1e6,telemetry_errors=[],cleanup_errors=[])

    def trial(self,ident='trial',loss=0,window=16,seed=11,missed=0,invalid_timing=False,mutator=None):
        base=self.root/'main';folder=base/ident;folder.mkdir(parents=True)
        case=next(c for c in self.plan['suite']['cases'] if c['id'].startswith(f'loss{loss}-'))
        variant=next(v for v in self.plan['variants'] if v['id']==f'flight{window}')
        profile=self.plan['profiles'][case['profile']]
        inv=dict(profile=profile,case=case,driver='native',engine='python-relay',loss_trace=False,ack_feedback=None,offered_mbps=0,
                 peer_sha256=self.plan['binary_sha256'],peer_environment=self.plan['fixed_peer']['effective_environment'],
                 suite_source_hash=digest(self.plan['source_hashes']))
        for key in ('warmup_s','measure_s','drain_s','payload_bytes','probe_hz'):inv[key]=self.plan['suite'][key]
        manifest=dict(trial_id=ident,variant=variant['id'],case_id=case['id'],seed=seed,binary_sha256=self.plan['binary_sha256'],
                      profile_id=case['profile'],profile=profile,invariants=inv,effective_variant_environment=variant['effective_environment'])
        endpoints=[self.endpoint(0,window,missed),self.endpoint(1,64)]
        goodput=endpoints[1]['goodput_mbps']
        summary=dict(manifest,validity='INVALID_HARNESS' if invalid_timing else 'PROVISIONAL_UNCALIBRATED',
                     sut_outcome='FAIL' if missed else 'COMPLETE',integrity_verified=True,
                     reasons=['relay lateness'] if invalid_timing else [],sut_failures=['probe deadlines unserved/unobserved'] if missed else [],
                     metrics={'c2s.verified_goodput_mbps':goodput,'client.probe_rtt_ms.p95':10,'client.probe_rtt_ms.p99':20,'client.probe_scheduled_ms.p99':20})
        direction=dict(received_datagrams=100,random_drops=loss,queue_drops=0,blackout_drops=0,delivered_datagrams=100-loss,
                       socket_send_errors=0,duplicates=0,lateness_ms=dict(p99=4 if invalid_timing else .1))
        shape=dict(engine='python_udp_relay_v1',error=None,pending_on_close=0,c2s=copy.deepcopy(direction),s2c=copy.deepcopy(direction))
        if mutator:mutator(summary,manifest,endpoints,shape)
        for name,value in [('summary.json',summary),('manifest.json',manifest),('shaper.json',shape),
                           *[(f'endpoint-{i}.result.json',e) for i,e in enumerate(endpoints)]]:write_json(folder/name,value)
        seal(folder)
        return folder,summary

    def test_preparing_dashboard_does_not_invent_measurements(self):
        result=dashboard(self.plan,[])
        self.assertEqual(len(result['groups']),16)
        self.assertEqual(result['meta']['planned_trials'],80)
        for group in result['groups']:
            self.assertEqual(group['run_count'],0)
            self.assertIsNone(group['actual_loss']['c2s'])
            self.assertIsNone(group['queue_drops']['s2c'])
            self.assertIsNone(group['metrics']['qualified']['goodput_mbps']['median'])

    def test_corrected_reporter_receipt_preserves_frozen_source_identity(self):
        plan=copy.deepcopy(self.plan);plan['reporter_sha256']='0'*64
        result=dashboard(plan,[])
        self.assertEqual(result['meta']['reporter']['frozen_source_sha256'],'0'*64)
        self.assertEqual(result['meta']['reporter']['source_sha256'],sha256(ROOT/'flight_report.py'))
        self.assertFalse(result['meta']['reporter']['matches_frozen_source'])
        self.assertEqual(result['meta']['reporter']['interpretation_revision'],'sparse-counter-v2')

    def test_exact_count_and_larger_span_are_valid_and_actual_payload_reported(self):
        folder,declared=self.trial()
        row=inspect_trial(folder,declared,self.plan)
        self.assertTrue(row['qualified'],row['reasons'])
        self.assertEqual((row['peak_inflight'],row['peak_span']),(16,17))
        self.assertEqual(row['wire_max_payload'],1150)
        self.assertEqual(row['minimum_data_chunks_per_record'],15)

    def test_probe_only_failure_retains_integrity_and_failed_survivor_tails(self):
        folder,declared=self.trial(missed=3)
        row=inspect_trial(folder,declared,self.plan)
        self.assertFalse(row['qualified']);self.assertTrue(row['integrity_valid'])
        result=dashboard(self.plan,[row]);group=result['groups'][0]
        self.assertEqual((group['fail_count'],group['qualified_count'],group['integrity_count']),(1,0,1))
        self.assertIsNone(group['metrics']['qualified']['goodput_mbps']['median'])
        self.assertEqual(group['metrics']['integrity_valid']['rtt_p99_ms']['median'],20)
        self.assertEqual(group['probes']['missed'],3)

    def test_invalid_timing_integrity_rate_stays_diagnostic(self):
        folder,declared=self.trial(invalid_timing=True)
        row=inspect_trial(folder,declared,self.plan)
        self.assertTrue(row['integrity_verified']);self.assertFalse(row['integrity_valid']);self.assertFalse(row['qualified'])
        self.assertIn('timing-unqualified',row['goodput_scope'])
        group=dashboard(self.plan,[row])['groups'][0]
        self.assertEqual(group['metrics']['qualified']['goodput_mbps']['n'],0)
        self.assertEqual(group['metrics']['diagnostic']['goodput_mbps']['n'],1)

    def test_unknown_native_flags_are_not_zero_failure_evidence(self):
        def mutate(summary,manifest,endpoints,shape):del endpoints[0]['native_stats']['receive_failed']
        folder,declared=self.trial(mutator=mutate);row=inspect_trial(folder,declared,self.plan)
        self.assertIsNone(row['native_failure']);self.assertFalse(row['qualified'])
        group=dashboard(self.plan,[row])['groups'][0]
        self.assertEqual(group['native_failure_unknown_count'],1)

    def test_queue_drops_are_separate_from_random_loss(self):
        def mutate(summary,manifest,endpoints,shape):shape['c2s'].update(queue_drops=10,random_drops=25,delivered_datagrams=65)
        folder,declared=self.trial(loss=25,mutator=mutate);row=inspect_trial(folder,declared,self.plan)
        self.assertTrue(row['qualified'],row['reasons'])
        group=next(g for g in dashboard(self.plan,[row])['groups'] if g['loss_percent']==25 and g['window_packets']==16)
        self.assertEqual(group['queue_drops']['c2s'],10)
        self.assertAlmostEqual(group['actual_loss']['c2s'],25/90)
        self.assertEqual(group['pending']['c2s'],0)

    def test_sealed_inconsistent_zero_loss_and_probe_counters_are_rejected(self):
        def mutate(summary,manifest,endpoints,shape):shape['c2s'].update(random_drops=1,delivered_datagrams=99)
        folder,declared=self.trial(mutator=mutate)
        self.assertEqual(inspect_trial(folder,declared,self.plan)['validity'],'INVALID_ARTIFACT')
        def mutate2(summary,manifest,endpoints,shape):endpoints[0]['probe_unobserved']=1
        folder,declared=self.trial(ident='bad-probes',mutator=mutate2)
        self.assertIn('unobserved probes',inspect_trial(folder,declared,self.plan)['artifact_errors'][0])

    def test_real_sparse_counter_zero_contract_is_qualified_and_receipted(self):
        def mutate(summary,manifest,endpoints,shape):
            for direction in ('c2s','s2c'):
                for key in ('random_drops','queue_drops','blackout_drops','socket_send_errors','duplicates'):del shape[direction][key]
        folder,declared=self.trial(mutator=mutate);row=inspect_trial(folder,declared,self.plan)
        self.assertTrue(row['qualified'],row['reasons'])
        self.assertEqual(row['network']['c2s']['random_drops'],0)
        self.assertEqual(row['network']['c2s']['inferred_zero_counters'],['random_drops','queue_drops','blackout_drops','socket_send_errors','duplicates'])
        self.assertEqual(row['network_counter_provenance']['link_source_sha256'],self.plan['source_hashes']['rdpbench/link.py'])

    def test_sparse_counter_unknown_provider_or_manifest_stays_unknown(self):
        sparse=dict(received_datagrams=10,delivered_datagrams=10)
        inv=dict(suite_source_hash=digest(self.plan['source_hashes']))
        unknown_plan=copy.deepcopy(self.plan);unknown_plan['source_hashes']['rdpbench/link.py']='0'*64
        unknown_inv=dict(suite_source_hash=digest(unknown_plan['source_hashes']))
        for shape,plan,receipt in [(dict(engine='unknown'),self.plan,inv),
                                   (dict(engine='python_udp_relay_v1'),unknown_plan,unknown_inv),
                                   (dict(engine='python_udp_relay_v1'),self.plan,dict(suite_source_hash='0'*64))]:
            counters,inferred=relay_counter_values(sparse,shape,plan,receipt)
            self.assertIsNone(counters['random_drops']);self.assertIsNone(counters['queue_drops']);self.assertEqual(inferred,[])

    def test_explicit_null_counter_is_never_normalized_to_zero(self):
        sparse=dict(received_datagrams=10,delivered_datagrams=10,random_drops=None)
        counters,inferred=relay_counter_values(sparse,dict(engine='python_udp_relay_v1'),self.plan,
                                              dict(suite_source_hash=digest(self.plan['source_hashes'])))
        self.assertIsNone(counters['random_drops']);self.assertNotIn('random_drops',inferred)

    def test_changed_raw_artifact_is_rejected(self):
        folder,declared=self.trial();write_json(folder/'shaper.json',{})
        row=inspect_trial(folder,declared,self.plan)
        self.assertEqual(row['validity'],'INVALID_ARTIFACT');self.assertIsNone(row['rtt_p99_ms'])

    def test_grandfathered_peer_shrink_is_valid(self):
        fields=self.endpoint(1,64)['flight_config'];fields['fields'].update(peer_log_window=2,peer_protocol_capacity_packets=3,effective_max_inflight_packets=3)
        self.assertEqual(flight_fields(fields)['current_inflight_packets'],64)

    def test_same_trial_count_with_wrong_conditions_is_rejected(self):
        suite=copy.deepcopy(self.plan['suite']);suite['seeds']=[1,2,3,4,5]
        with self.assertRaisesRegex(ValueError,'seeds'):validate_matrix(suite,self.plan['profiles'])

    def test_interrupted_sealed_attempt_is_retained_and_never_retried(self):
        self.root.joinpath('main').mkdir(exist_ok=True)
        self.root.joinpath('main').rmdir()
        def interrupted(variant,case,profile,suite,seed,folder):
            self.trial(ident=folder.name,loss=0,window=int(variant['env']['RDP_BENCH_MAX_INFLIGHT_PACKETS']),seed=seed)
            raise KeyboardInterrupt
        with patch('flight_sweep.one_trial',side_effect=interrupted) as runner:
            with self.assertRaises(KeyboardInterrupt):execute(self.root,self.plan)
        runner.assert_called_once()
        campaign=read_json(self.root/'main/campaign.json');self.assertEqual(len(campaign['trials']),1)
        self.assertTrue(campaign['interrupted']);self.assertFalse(campaign['complete'])
        result=read_json(self.root/'dashboard/dashboard.json')
        self.assertEqual(len(result['trials']),1);self.assertEqual(len(result['condition_coverage']['unrun']),79)
        self.assertTrue(result['meta']['interrupted']);self.assertEqual(result['meta']['status'],'running')

    def test_unsealed_started_attempt_is_visible_after_controller_disappears(self):
        self.root.joinpath('main').mkdir()
        attempt=dict(trial_id='unfinished',case_id='loss0-upload-mixed',variant='flight16',seed=11,binary_sha256=self.plan['binary_sha256'])
        write_json(self.root/'main/campaign.json',dict(trials=[],attempts=[attempt]))
        result=collect(self.root)
        self.assertEqual(result['condition_coverage']['attempted'],1)
        self.assertEqual(result['trials'][0]['outcome'],'UNKNOWN')
        self.assertEqual(result['trials'][0]['validity'],'INVALID_ARTIFACT')
        self.assertEqual(len(result['condition_coverage']['unrun']),79)

    def test_started_attempt_cannot_escape_campaign_directory(self):
        self.root.joinpath('main').mkdir()
        write_json(self.root/'main/campaign.json',dict(trials=[],attempts=[dict(trial_id='../outside')]))
        with self.assertRaisesRegex(ValueError,'unsafe'):collect(self.root)


if __name__=='__main__':unittest.main()
