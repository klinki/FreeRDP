"""Audit PR geometry/unified health without changing the old comparison cohort."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import test_flight_report as fixture
from flight_report import dashboard,flight_fields,inspect_trial
from rdpbench.adapter import PR_ADAPTER,PR_GEOMETRY
from rdpbench.native import Native

class PrFlightReportTests(unittest.TestCase):
    def setUp(self):
        self.fixture=fixture.FlightReportTests();self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        plan=self.fixture.plan
        plan['native_adapter']=copy.deepcopy(PR_ADAPTER);plan['native_geometry']=copy.deepcopy(PR_GEOMETRY)
        plan['build_metadata']['native_adapter']=copy.deepcopy(PR_ADAPTER)
        for variant in plan['variants']:variant['build_metadata']=copy.deepcopy(plan['build_metadata'])

    def trial(self,mutator=None):
        plan=self.fixture.plan
        def change(summary,manifest,endpoints,shape):
            manifest['build_metadata']=copy.deepcopy(plan['build_metadata'])
            manifest['invariants']['native_adapter']=copy.deepcopy(PR_ADAPTER)
            for endpoint in endpoints:
                fields=endpoint['flight_config']['fields']
                fields.update({k:v for k,v in PR_GEOMETRY.items() if k not in ('send_window_policy','wire_mtu')})
                endpoint['queue_stats']['fields']['max_payload_bytes']=1208
                endpoint.update(native_backend=2,native_driver_error=None,native_driver_error_available=True)
                endpoint['native_stats'].update(backend=2,stats_semantics='upstream-unified-state',transport_failed=0,
                                              send_failed=None,receive_failed=None,loss_reports=None)
            if mutator:mutator(summary,manifest,endpoints,shape)
        folder,declared=self.fixture.trial(mutator=change)
        return inspect_trial(folder,declared,plan)

    def test_native64_storage_and_log15_qualify_with_declared_backend(self):
        row=self.trial()
        self.assertTrue(row['qualified'],row['reasons']);self.assertFalse(row['native_failure'])
        self.assertEqual(row['flight_config']['fields']['sent_storage_slots'],64)
        self.assertEqual(row['flight_config']['fields']['usable_storage_packets'],64)
        self.assertEqual(row['flight_config']['fields']['own_log_window'],15)
        self.assertEqual(row['minimum_data_chunks_per_record'],14)
        result=dashboard(self.fixture.plan,[row]);self.assertEqual(result['meta']['native_geometry'],PR_GEOMETRY)

    def test_default_old_cohort_still_requires_its_log7_geometry(self):
        plain=fixture.FlightReportTests();plain.setUp();self.addCleanup(plain.doCleanups)
        folder,declared=plain.trial();row=inspect_trial(folder,declared,plain.plan)
        self.assertTrue(row['qualified'],row['reasons']);self.assertEqual(row['flight_config']['fields']['own_log_window'],7)

    def test_log7_receipt_does_not_establish_declared_log15(self):
        def change(summary,manifest,endpoints,shape):
            endpoints[0]['flight_config']['fields'].update(own_log_window=7,local_protocol_capacity_packets=127)
        row=self.trial(change)
        self.assertFalse(row['qualified']);self.assertTrue(row['integrity_verified'])
        self.assertIn('requested flight/byte configuration not established',' '.join(row['reasons']))

    def test_legacy_flags_cannot_stand_in_for_unified_backend_health(self):
        def change(summary,manifest,endpoints,shape):
            endpoints[0]['native_stats']=dict(send_failed=0,receive_failed=0)
        row=self.trial(change)
        self.assertFalse(row['qualified']);self.assertIsNone(row['native_failure'])
        self.assertEqual(dashboard(self.fixture.plan,[row])['groups'][0]['native_failure_unknown_count'],1)

    def test_unified_failure_is_native_without_invented_direction(self):
        def change(summary,manifest,endpoints,shape):
            summary.update(sut_outcome='FAIL',integrity_verified=False,sut_failures=['native transport failed'])
            endpoints[0].update(completed=False,error='production receive failed / unhealthy stream')
            endpoints[0]['native_stats']['transport_failed']=1
        row=self.trial(change)
        self.assertTrue(row['native_failure']);self.assertFalse(row['driver_failure']);self.assertFalse(row['qualified'])
        self.assertIsNone(row['endpoint_receipts'][0]['native_stats']['send_failed'])
        group=dashboard(self.fixture.plan,[row])['groups'][0]
        self.assertEqual((group['fail_count'],group['native_failure_count']),(1,1))

    def test_known_driver_buffer_failure_is_harness_not_native_failure(self):
        def change(summary,manifest,endpoints,shape):
            summary.update(sut_outcome='HARNESS_FAILURE',validity='INVALID_HARNESS',integrity_verified=False,
                           reasons=['benchmark driver failure: BENCHMARK_DRIVER_UNREAD_LIMIT'])
            endpoints[0].update(completed=False,error='benchmark driver failure: BENCHMARK_DRIVER_UNREAD_LIMIT',
                                native_driver_error='BENCHMARK_DRIVER_UNREAD_LIMIT')
            endpoints[0]['native_stats']['transport_failed']=1
        row=self.trial(change)
        self.assertTrue(row['driver_failure']);self.assertFalse(row['native_failure']);self.assertFalse(row['qualified'])
        group=dashboard(self.fixture.plan,[row])['groups'][0]
        self.assertEqual((group['harness_failure_count'],group['driver_failure_count'],group['native_failure_count']),(1,1,0))

    def test_adapter_metadata_mismatch_is_invalid_artifact(self):
        def change(summary,manifest,endpoints,shape):manifest['invariants']['native_adapter']['backend']=1
        row=self.trial(change)
        self.assertEqual(row['validity'],'INVALID_ARTIFACT');self.assertIn('adapter metadata differs',row['artifact_errors'][0])

    def test_upstream_peer_advertisement_does_not_create_synthetic_send_gate(self):
        fields=self.fixture.endpoint(0,64)['flight_config']
        fields['fields'].update({k:v for k,v in PR_GEOMETRY.items() if k not in ('send_window_policy','wire_mtu')})
        fields['fields'].update(peer_log_window=2,peer_protocol_capacity_packets=3)
        self.assertEqual(flight_fields(fields,policy='upstream-v3-native-64')['effective_max_inflight_packets'],64)
        with self.assertRaisesRegex(ValueError,'effective capacity'):flight_fields(fields,policy='legacy-peer-capacity')

if __name__=='__main__':unittest.main()
