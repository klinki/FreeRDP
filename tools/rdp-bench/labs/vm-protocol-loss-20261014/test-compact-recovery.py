"""Small compact-summary fixtures; execute only between timed campaigns."""
import copy
import importlib.util
from pathlib import Path
import unittest

SPEC=importlib.util.spec_from_file_location('compact_recovery',Path(__file__).with_name('compact-recovery.py'))
c=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(c)


def ack(*,retired=False,wire=True,timestamp=101_500_000_000,violation=False):
    return dict(kind='explicit_vector_positive',positive_receipt=True,
                peer_aoa_retirement_events=[5] if retired else [],wire_capture_matches=[9] if wire else [],
                time_unix_ns=timestamp,sequence_epoch_verified=True,native_received_before_ack=[3],
                initial_data_receipt_possible=False,qualified_unretained_ack=violation)


def report(feedback=None):
    return dict(schema='freerdp.pr13608.recovery_audit',version=1,scope='fixture; header-only',channel=40,
                native_trace=dict(retained_valid=True,complete_from_arm=True,overwritten_events=0,
                                  rows_truncated=False,errors=[],header_errors=[],clock_slop_ms=0,
                                  alignment=dict(offset_min_ns=100_000_000_000,offset_max_ns=100_000_000_000)),
                native_flow_association=dict(verified=True,flow='fixture:50000',unique_header_matches={'fixture:50000':3}),
                capture_quality={n:dict(absence_qualified=True,complete=True,kernel_drops=0) for n in ('offered','received')},
                captures={n:dict(first_time_ns=100_500_000_000,last_time_ns=102_500_000_000,
                                 record_count=12,snaplen=256,linktype=1,error_count=0) for n in ('offered','received')},
                conclusion=dict(qualified_unretained_ack_count=0),inputs={},
                focused_gap=dict(start_ns=1_000_000_000,end_ns=2_000_000_000,duration_ms=1000,
                                 max_gap_age_ms=1000,max_buffered=10,recovered=True),
                aliases=[dict(flow='fixture:50000',data_seq=10,data_extended=10,channel_extended=40,
                              offered_time_ns=101_000_000_000,received_packets=[7],serial_ambiguous=False,
                              native_rx=[dict(status=1)],feedback=feedback or [ack()])],
                observed_inter_alias_offer_ms=[4000],native_rx_for_channel=[dict(status=1)],
                native_channel_deliveries=[dict(id=20)],native_slot_collisions=[])


class CompactRecoveryTests(unittest.TestCase):
    def test_historical_retired_and_native_only_references_are_not_active_wire_receipts(self):
        value=c.compact(report([ack(),ack(retired=True),ack(wire=False,timestamp=103_000_000_000)]))
        counts=value['ack_qualification']['reference_counts']
        self.assertEqual(counts['positive_references'],3)
        self.assertEqual(counts['positive_references_retired_by_peer_aoa'],1)
        self.assertEqual(counts['eligible_captured_active_positive_references'],1)
        self.assertEqual(counts['outside_offered_observed_packet_window'],1)

    def test_raw_violation_without_captured_active_scope_remains_unqualified(self):
        value=c.compact(report([ack(wire=False,violation=True)]))
        counts=value['ack_qualification']['reference_counts']
        self.assertEqual(counts['raw_analyzer_qualified_violation_references'],1)
        self.assertEqual(counts['eligible_captured_active_violation_references'],0)
        self.assertEqual(counts['native_only_or_otherwise_unqualified_violation_references'],1)

    def test_crossflow_or_incomplete_native_history_blocks_eligibility(self):
        for field in ('flow','trace'):
            with self.subTest(field=field):
                source=report()
                if field=='flow':source['native_flow_association']['verified']=False
                else:source['native_trace']['complete_from_arm']=False
                value=c.compact(source)
                self.assertFalse(value['ack_qualification']['base_eligibility'])
                self.assertEqual(value['ack_qualification']['reference_counts']['eligible_captured_active_positive_references'],0)

    def test_captured_recovery_after_measurement_or_loss_reset_is_not_profile_recovery(self):
        profile=dict(measurement_steps=[dict(start_unix_ns=101_000_000_000,end_unix_ns=101_500_000_000,loss_percent=70)],
                     loss_reset_zero_receipt_unix_ns=101_800_000_000)
        value=c.compact(report(),profile)['gap_scope']
        self.assertTrue(value['inside_both_capture_observed_windows'])
        self.assertFalse(value['inside_measured_loss_step'])
        self.assertTrue(value['recovered_after_zero_loss_receipt'])

    def test_native_recovery_after_capture_is_explicitly_outside_capture(self):
        source=report();source['captures']['received']['last_time_ns']=101_200_000_000
        scope=c.compact(source)['gap_scope']
        self.assertFalse(scope['inside_both_capture_observed_windows'])
        self.assertEqual(scope['native_recovery_after_received_last_packet_ms'],800)

    def test_compact_output_omits_packet_and_ack_arrays(self):
        value=c.compact(report())
        self.assertNotIn('feedback',value['aliases'][0])
        self.assertNotIn('native_rx',value['aliases'][0])
        self.assertNotIn('received_packets',value['aliases'][0])
        self.assertNotIn('native_channel_deliveries',value)
        self.assertEqual(value['native_channel_delivery_count'],1)
        self.assertEqual(value['aliases'][0]['postloss_matches'],1)

    def test_utc_profile_conversion_is_exact_and_naive_time_is_unknown(self):
        self.assertEqual(c.unix_ns('1970-01-01T00:01:41.123456Z'),101_123_456_000)
        self.assertIsNone(c.unix_ns('1970-01-01T00:01:41'))
        profile=c.profile_from_campaign({'steps':[dict(id='loss70',loss_percent=70,
            start_utc='1970-01-01T00:01:41Z',end_utc='1970-01-01T00:01:42Z')]},
            {'state':dict(loss_percent=0,changed_utc='1970-01-01T00:01:43Z')})
        self.assertEqual(profile['loss_reset_zero_receipt_unix_ns'],103_000_000_000)

    def test_schema_mismatch_fails_closed(self):
        source=report();source['schema']='different'
        with self.assertRaisesRegex(ValueError,'schema'):c.compact(source)

    def test_receiver_resync_ambiguity_is_recorded_without_relaxing_ack_gate(self):
        source=report();source['aliases'][0]['serial_ambiguous']=True
        source['peer_aoa_events']=[dict(event='rx_resync',id=4,time_ns=1_400_000_000,
                                      data_seq=10,detail=12,base=10,next_channel=40,buffered=9)]
        value=c.compact(source)
        self.assertEqual(value['serial_ambiguity']['receiver_resync_count'],1)
        self.assertEqual(value['serial_ambiguity']['first_receiver_resync']['detail'],12)
        self.assertEqual(value['serial_ambiguity']['ambiguous_aliases'],1)
        self.assertEqual(value['ack_qualification']['reference_counts']['eligible_captured_active_positive_references'],0)


if __name__=='__main__':unittest.main()
