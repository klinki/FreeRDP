"""Pure deferred fixtures for historical matching and censored comparison data."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from compare_pr_flight import compare


def dataset(pr=False):
    losses=(0,25,50,70);windows=(16,32,48,64);seeds=(11,29,47,71,101)
    profiles={}
    for loss in losses:
        direction=dict(one_way_ms=15,jitter_ms=1,rate_mbps=10,queue_ms=30,overhead_bytes=28,loss=loss/100,duplicate=0,reorder=0)
        profiles[f'flight_loss{loss}']=dict(c2s=dict(direction),s2c=dict(direction))
    suite=dict(phase='main',warmup_s=5,measure_s=30,drain_s=12,payload_bytes=16384,probe_hz=20,seeds=list(seeds),
               cases=[dict(id=f'loss{l}-upload-mixed',profile=f'flight_loss{l}',workload='upload_mixed',loss_trace=False) for l in losses])
    variants=[dict(env=dict(RDP_BENCH_MAX_INFLIGHT_PACKETS=str(w),RDP_BENCH_SEND_QUEUE_BYTES='131072',RDP_BENCH_LOSS_TRACE='0'),
                   peer_env=dict(RDP_BENCH_MAX_INFLIGHT_PACKETS='64',RDP_BENCH_SEND_QUEUE_BYTES='131072',RDP_BENCH_LOSS_TRACE='0')) for w in windows]
    geometry=dict(own_log_window=15 if pr else 7,peer_log_window=15 if pr else 7,local_protocol_capacity_packets=32767 if pr else 127,
                  peer_protocol_capacity_packets=32767 if pr else 127,sent_storage_slots=64 if pr else 128,usable_storage_packets=64 if pr else 127,queue_limit_bytes=131072)
    rows=[]
    for loss in losses:
        for window in windows:
            for seed in seeds:
                row=dict(trial_id=f'{"pr" if pr else "old"}-{loss}-{window}-{seed}',loss_percent=loss,window_packets=window,seed=seed,
                         outcome='COMPLETE',validity='PROVISIONAL_UNCALIBRATED',artifact_errors=[],integrity_verified=True,qualified=True,timing_valid=True,
                         native_failure=False,deadline=False,early_exit=False,goodput_mbps=8,rtt_p95_ms=40,rtt_p99_ms=50,scheduled_p99_ms=55,
                         observed_measurement_responses=600,probes=dict(expected=700,injected=700,observed=700,missed=0,unobserved=0,timeouts=0,send_failures=0),
                         flight_config=dict(fields=dict(geometry)),peer_flight_config=dict(fields=dict(geometry)),wire_max_payload=1208 if pr else 1132,
                         endpoint_receipts=[dict(incomplete_record_bytes=0),dict(incomplete_record_bytes=0)],reasons=[])
                if pr:row['driver_failure']=False
                rows.append(row)
    meta=dict(status='complete',scope='native-udp2',byte_queue=131072,wire_mtu=1232,record_payload_bytes=16384,record_header_bytes=36,probe_payload_bytes=64)
    plan=dict(suite=suite,profiles=profiles,variants=variants,source_hashes={'rdpbench/link.py':'same'},commit='PR' if pr else 'original',binary_sha256='pr-sha' if pr else 'old-sha')
    if pr:plan.update(native_adapter=dict(backend=2,stats_semantics='upstream-unified-state'),native_geometry=geometry)
    return dict(schema='freerdp.flight_matrix',meta=meta,plan=plan,trials=rows)


class ComparePrFlightTests(unittest.TestCase):
    def compare(self,old,pr):return compare(old,pr,'old.json','pr.json','/old/main','/pr/main')

    def test_exact_matrix_preserves_intrinsic_differences_and_missing_driver_state(self):
        result=self.compare(dataset(),dataset(True));self.assertEqual(len(result['cells']),16);self.assertEqual(len(result['per_seed']),80)
        intrinsic=result['datasets']
        self.assertEqual(intrinsic['baseline']['intrinsic']['observed_geometry']['own_log_window']['values'],[7])
        self.assertEqual(intrinsic['pr']['intrinsic']['observed_geometry']['own_log_window']['values'],[15])
        self.assertEqual(intrinsic['baseline']['intrinsic']['observed_data_payload_bytes'],[1132])
        self.assertEqual(intrinsic['pr']['intrinsic']['observed_data_payload_bytes'],[1208])
        self.assertIsNone(result['cells'][0]['baseline']['driver_failures']['count'])
        self.assertEqual(result['cells'][0]['baseline']['driver_failures']['unknown'],5)
        self.assertIn('/pr/main/pr-',result['per_seed'][0]['pr']['raw_trial_reference'])

    def test_metric_matching_uses_same_finite_seed_subset_without_pooling(self):
        old=dataset();pr=dataset(True)
        old['trials'][0]['goodput_mbps']=100
        pr['trials'][0]['goodput_mbps']=None
        pr['trials'][1]['goodput_mbps']=7
        result=self.compare(old,pr);m=result['cells'][0]['matched_per_metric']['diagnostic']['goodput_mbps']
        self.assertEqual(m['seeds'],[29,47,71,101]);self.assertEqual(m['n'],4)
        self.assertEqual(m['baseline']['median'],8);self.assertEqual(m['pr']['median'],8)
        self.assertEqual(m['delta_pr_minus_baseline']['median'],0)
        self.assertEqual(result['cells'][0]['matched_per_metric']['diagnostic']['rtt_p99_ms']['n'],5)

    def test_failed_native_high_loss_has_no_qualified_or_full_integrity_rate(self):
        old=dataset();pr=dataset(True)
        for row in pr['trials']:
            if row['loss_percent']==70:
                row.update(outcome='FAIL',integrity_verified=False,qualified=False,native_failure=True,goodput_mbps=5,partial_crc_payload_mbps=5,
                           probes=dict(expected=700,injected=100,observed=20,missed=600,unobserved=80,timeouts=0,send_failures=0),observed_measurement_responses=20)
        result=self.compare(old,pr);cell=result['cells'][-1]
        self.assertEqual(cell['pr']['metrics']['qualified']['goodput_mbps']['n'],0)
        self.assertEqual(cell['pr']['metrics']['diagnostic']['goodput_mbps']['n'],0)
        self.assertEqual(cell['pr']['metrics']['diagnostic']['rtt_p99_ms']['n'],5)
        self.assertEqual(cell['pr']['native_failures']['count'],5)
        self.assertEqual(cell['pr']['probes']['missed']['total'],3000)
        self.assertEqual(cell['pr']['observed_measurement_responses']['total'],100)

    def test_missing_counters_and_artifact_flags_never_become_zero(self):
        old=dataset();pr=dataset(True);pr['trials'][0]['probes']['missed']=None
        pr['trials'][1].update(validity='INVALID_ARTIFACT',artifact_errors=['changed'],qualified=False)
        result=self.compare(old,pr);cell=result['cells'][0]['pr']
        self.assertIsNone(cell['probes']['missed']['total']);self.assertEqual(cell['probes']['missed']['unknown_trials'],2)
        self.assertIsNone(cell['native_failures']['count']);self.assertEqual(cell['native_failures']['unknown'],1)
        self.assertIsNone(cell['incomplete_streams']);self.assertEqual(cell['incomplete_streams_unknown'],1)
        self.assertIsNone(cell['partial_record_trials']['count'])
        self.assertEqual(cell['qualified']['known_true'],3)
        self.assertEqual(cell['metrics']['qualified']['goodput_mbps']['n'],3)
        self.assertEqual(result['cells'][0]['matched_per_metric']['qualified']['goodput_mbps']['seeds'],[47,71,101])

    def test_configuration_change_duplicate_seed_and_link_change_are_rejected(self):
        for mutate in (lambda d:d['plan']['suite'].update(drain_s=10),
                       lambda d:d['trials'].__setitem__(0,copy.deepcopy(d['trials'][1])),
                       lambda d:d['plan']['source_hashes'].update({'rdpbench/link.py':'changed'})):
            pr=dataset(True);mutate(pr)
            with self.assertRaises(ValueError):self.compare(dataset(),pr)

    def test_probe_size_requires_explicit_metadata_or_known_frozen_source(self):
        old=dataset();pr=dataset(True)
        del old['meta']['probe_payload_bytes']
        with self.assertRaisesRegex(ValueError,'probe payload not established'):self.compare(old,pr)
        old['plan']['source_hashes']['rdpbench/endpoint.py']='bc9616eee3702b366e8beb86113af2f57157e6387a8192cefca601c82910004f'
        result=self.compare(old,pr)
        self.assertIn('recognized frozen',result['external_controls']['probe_payload_validation']['baseline']['basis'])
        pr['meta']['probe_payload_bytes']=65
        with self.assertRaisesRegex(ValueError,'probe payload differs'):self.compare(old,pr)

    def test_timing_invalid_full_integrity_is_diagnostic_only(self):
        old=dataset();pr=dataset(True)
        for row in old['trials']:row.update(validity='INVALID_HARNESS',qualified=False,timing_valid=False)
        result=self.compare(old,pr)
        self.assertEqual(result['cells'][0]['baseline']['metrics']['qualified']['goodput_mbps']['n'],0)
        self.assertEqual(result['cells'][0]['baseline']['metrics']['diagnostic']['goodput_mbps']['n'],5)
        self.assertEqual(result['cells'][0]['matched_per_metric']['qualified']['goodput_mbps']['n'],0)

if __name__=='__main__':unittest.main()
