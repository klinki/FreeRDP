"""Loss tracing stays honest about native stalls, relay confounds and failed pairs."""
import json,socket,sys,tempfile,time,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from loss_stall_sweep import prepare
from loss_stall_report import attribute,native_recoveries,METRICS,trace_trial,apply_trace_eligibility
from queue_report import pair
from rdpbench.endpoint import CallTrace
from rdpbench.link import Link,Relay,RelayTrace,packet_metadata
from rdpbench.feedback import ACK,DATA,ACKVEC,AOA,packet_kind
from rdpbench.common import read_json,write_json,rows as json_rows
from rdpbench.type1 import one_trial

def protect(flags,body):
    layout=(0x5000|flags).to_bytes(2,'little')+body
    wire=bytearray(bytes([min(7,len(layout))<<5])+layout+bytes(max(0,7-len(layout))))
    wire[0],wire[7]=wire[7],wire[0];return bytes(wire)

class LossTracingTests(unittest.TestCase):
    def test_visible_headers_keep_data_bytes_untouched(self):
        payload=b'secret application bytes never retained'
        packet=protect(ACK|AOA|DATA,b'\x03\0'+b'\0'*5+b'\x04\0'+b'\x05\0'+b'\x06\0'+payload)
        before=bytes(packet);value=packet_metadata(packet)
        self.assertEqual(value,dict(kind='data_with_ack',udp_bytes=len(packet),log_window=5,
                                    ack_base=3,aoa=4,data_seq=5,channel_seq=6))
        self.assertEqual(packet,before);self.assertNotIn('secret',json.dumps(value))
        vector=protect(DATA|ACKVEC,b'\x05\0'+b'\x03\0\x01\x7f'+b'\x06\0'+payload)
        self.assertEqual(packet_metadata(vector)['channel_seq'],6)
        self.assertEqual(packet_metadata(vector)['ackvec_base'],3)
        self.assertEqual(packet_metadata(b'invalid')['kind'],'unclassified')

    def test_bounded_call_and_relay_trace_never_silently_drop_evidence(self):
        trace=CallTrace();trace.CAPACITY=1
        trace.call('bulk',10,16420,0,180_000_000)
        trace.call('read',None,0,180_000_001,180_001_001)
        self.assertEqual(trace.last_slow_call['record_id'],10)
        self.assertEqual(trace.last_call['call_kind'],'read')
        trace.add('probe_not_injected',id=1)
        self.assertFalse(trace.summary()['valid']);self.assertEqual(trace.summary()['overflow'],1)
        relay=RelayTrace();relay.CAPACITY=1;relay.add('offer');relay.add('dispatch')
        self.assertFalse(relay.summary()['valid']);self.assertEqual(relay.summary()['overflow'],1)

    def test_real_relay_traces_metadata_kind_and_preserves_wire_payload(self):
        """Exercise the actual threaded offer/heap/dispatch path over loopback UDP."""
        profile=dict(rate_mbps=10,one_way_ms=0,jitter_ms=0,loss=0,queue_ms=30,overhead_bytes=28)
        wire=protect(DATA,b'\x05\0\x06\0private payload bytes')
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as left,socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as right:
                left.bind(('127.0.0.1',0));right.bind(('127.0.0.1',0));right.settimeout(2)
                relay=Relay(left.getsockname()[1],right.getsockname()[1],{'c2s':profile,'s2c':profile},11,
                            loss_ledger=folder/'relay-loss.jsonl')
                relay.start(time.monotonic_ns())
                try:
                    left.sendto(wire,('127.0.0.1',relay.ports[0]))
                    received,_=right.recvfrom(65535)
                    self.assertEqual(received,wire)
                finally:shape=relay.close()
            self.assertIsNone(shape['error']);self.assertTrue(shape['loss_trace']['valid'])
            records=list(json_rows(folder/'relay-loss.jsonl'))
            self.assertEqual([r['type'] for r in records],['offer','dispatch'])
            self.assertEqual([r['kind'] for r in records],['data_without_ack']*2)
            self.assertEqual(records[1]['data_seq'],5);self.assertEqual(records[1]['channel_seq'],6)
            self.assertNotIn('private payload',json.dumps(records))
            self.assertEqual(records[1]['lateness_ns'],max(0,records[1]['actual_dispatch_ns']-records[1]['due_ns']))
            self.assertEqual(shape['c2s']['received_datagrams'],1);self.assertEqual(shape['c2s']['delivered_datagrams'],1)
            # Audit this real relay ledger using empty application/native test fixtures;
            # no fabricated native events or production responsiveness claim is made.
            (folder/'calls.jsonl').write_text('');(folder/'native.jsonl').write_text('')
            write_json(folder/'shaper.json',shape)
            write_json(folder/'endpoint-0.result.json',{'counters':{'probes_not_injected':0},
                'loss_trace':{'enabled':True,'valid':True,'count':0,'call_ledger':'calls.jsonl','native_ledger':'native.jsonl',
                              'native':{'available':True,'valid':True,'metadata':{'count':0}}}})
            write_json(folder/'endpoint-1.result.json',{'loss_trace':{'native':{'available':False,'valid':False,'reasons':['fixture has no peer ABI']}}})
            audited=trace_trial({'validity':'PROVISIONAL_UNCALIBRATED','artifact_directory':str(folder)})
            self.assertTrue(audited['valid'],audited['reasons'])
            self.assertFalse(audited['fixed_peer_native_trace']['available'])
            records[1]['data_seq']=99
            (folder/'relay-loss.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
            self.assertIn('relay event differs from its offered packet',
                          trace_trial({'validity':'PROVISIONAL_UNCALIBRATED','artifact_directory':str(folder)})['reasons'])

    @staticmethod
    def fixture(lag=200_000):
        calls=[dict(type='native_call',call_kind='bulk',record_id=17,bytes=16420,
                    start_ns=40_000_000,end_ns=240_000_000,duration_ns=200_000_000),
               dict(type='probe_not_injected',id=18,scheduled_ns=30_000_000,observed_ns=240_000_001)]
        native=[dict(time_ns=0,event_name='data_send',data_seq=3,channel_seq=4),
                dict(time_ns=40_000_000,event_name='enqueue_block',data_seq=0,channel_seq=0),
                dict(time_ns=45_000_000,event_name='nack_gap',data_seq=3,channel_seq=4,detail=3),
                dict(time_ns=200_000_000,event_name='retry_timer',data_seq=3,channel_seq=4,detail=200),
                dict(time_ns=230_000_000,event_name='retire',data_seq=34,channel_seq=4)]
        relay=[dict(type='drop',direction='c2s',packet_id=1,data_seq=3,channel_seq=4,offered_ns=1_000_000,reason='random_drops'),
               dict(type='dispatch',direction='s2c',packet_id=2,due_ns=44_000_000,actual_dispatch_ns=44_000_000+lag,lateness_ns=lag)]
        return calls,native,relay

    def test_timer_loss_stall_requires_native_admission_and_timely_relay(self):
        calls,native,relay=self.fixture();value=attribute(calls,native,relay)
        self.assertEqual(value['stall_classes'],{'native_admission_timer_recovery':1})
        self.assertEqual(value['miss_classes'],{'native_admission_timer_recovery':1})
        recovery=value['recoveries'][0]
        self.assertEqual(recovery['relay_drop_reason'],'random_drops')
        self.assertEqual(recovery['gap_to_retry_ms'],155)
        self.assertEqual(recovery['retry_to_retire_ms'],30)

    def test_late_relay_prevents_claiming_native_only_cause(self):
        value=attribute(*self.fixture(25_000_000))
        self.assertEqual(value['miss_classes'],{'relay_timing_confounded':1})
        self.assertEqual(value['stalls'][0]['relay_max_lateness_ms'],25)
        self.assertEqual(len(value['stalls'][0]['native_timer_recovery_evidence']),1)
        calls,native,relay=self.fixture(25_000_000)
        relay[1]['due_ns']=35_000_000;relay[1]['actual_dispatch_ns']=60_000_000
        self.assertEqual(attribute(calls,native,relay)['stall_classes'],{'relay_timing_confounded':1})

    def test_no_native_or_relay_evidence_stays_unknown(self):
        calls,native,relay=self.fixture()
        value=attribute(calls,[],[])
        self.assertEqual(value['stalls'][0]['classification'],'insufficient_relay_evidence')
        self.assertEqual(value['miss_classes'],{'insufficient_evidence':1})
        value=attribute(calls,[],relay)
        self.assertEqual(value['stalls'][0]['classification'],'unexplained_native_call')
        self.assertEqual(native_recoveries(native,[])[0]['relay_drop_reason'],None)
        value=attribute(calls,native,[relay[1]])
        self.assertEqual(value['stalls'][0]['classification'],'native_admission_timer_wait_unattributed')

    def test_fast_retry_and_channel_wrap_are_chronological(self):
        calls,native,relay=self.fixture();native[3]['event_name']='retry_fast';native[3]['time_ns']=50_000_000
        value=attribute(calls,native,relay)
        self.assertNotIn('native_admission_timer_recovery',value['stall_classes'])
        self.assertEqual(value['recovery_counts'],{'retry_fast':1})
        self.assertEqual(value['recoveries'][0]['attempt_to_retry_ms'],50)

    def test_campaign_pins_40_main_8_diagnostic_4_smoke_4_overhead_trials(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);candidate=root/'candidate.dylib';reference=root/'reference.dylib'
            candidate.write_bytes(b'candidate');reference.write_bytes(b'reference')
            configs,variants=prepare(root/'campaign',candidate,reference,{'commit':'new'},
                                     {'commit':'old','resolved_native_source_commit':'resolved'})
            counts={phase:len(c['cases'])*len(c['seeds'])*len(variants[phase]) for phase,c in configs.items()}
            self.assertEqual(counts,dict(smoke=4,main=40,**{'diagnostic-29':4,'diagnostic-101':4,'overhead':4}))
            self.assertEqual(configs['diagnostic-29']['warmup_s'],5)
            self.assertEqual(configs['diagnostic-29']['measure_s'],30)
            self.assertEqual(configs['diagnostic-101']['measure_s'],15)
            self.assertFalse(configs['main']['cases'][0]['loss_trace'])
            self.assertTrue(configs['smoke']['cases'][0]['loss_trace'])
            self.assertEqual(variants['main'][0]['commit'],'resolved')
            modes={v['id']:v['env']['RDP_BENCH_LOSS_RECOVERY_MODE'] for v in variants['main']}
            self.assertEqual(modes['candidate-q48'],'fast');self.assertEqual(modes['reference-q64'],'timer')
            saved=read_json(root/'campaign'/'loss-sweep-plan.json')
            self.assertEqual(saved['fixed_peer_environment']['RDP_BENCH_SEND_QUEUE_BYTES'],'131072')
            candidate.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'provenance changed'):prepare(root/'campaign',candidate,reference)

    def test_failed_pair_is_retained_but_excluded_from_complete_comparison(self):
        def trial(variant,seed,valid,outcome):
            return dict(trial_id=variant+str(seed),case_id='case',comparison_key='same',variant=variant,seed=seed,
                        validity=valid,sut_outcome=outcome,artifact_intact_timing_valid_complete=valid!='INVALID_HARNESS' and outcome=='COMPLETE',
                        validated_lab_complete=False,metrics={'client.probe_rtt_ms.p99':100+seed})
        trials=[trial('reference-q48',11,'PROVISIONAL_UNCALIBRATED','FAIL'),
                trial('candidate-q48',11,'PROVISIONAL_UNCALIBRATED','COMPLETE'),
                trial('reference-q48',29,'PROVISIONAL_UNCALIBRATED','COMPLETE'),
                trial('candidate-q48',29,'INVALID_HARNESS','COMPLETE')]
        comparisons,_=pair(trials,'reference-q48','candidate-q48',('client.probe_rtt_ms.p99',))
        bysubset={r['subset']:r for r in comparisons}
        self.assertEqual(len(bysubset['all_observed_diagnostic']['pairs']),2)
        self.assertEqual(bysubset['artifact_intact_timing_valid_complete']['pairs'],[])
        self.assertEqual(len(bysubset['artifact_intact_timing_valid_complete']['excluded_pairs']),2)

    def test_manifest_records_forced_actual_trace_environment(self):
        config=read_json(ROOT/'configs/type1.smoke.json');profile=read_json(ROOT/'configs/profiles.json')['profiles']['ten_mbit_clean']
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory)/'trial'
            with patch('rdpbench.type1.free_port',side_effect=PermissionError('fixture')):
                one_trial({'id':'fast-q48','env':{'RDP_BENCH_LOSS_TRACE':'0'},'peer_env':{'RDP_BENCH_LOSS_TRACE':'0'}},
                          {'id':'traced','profile':'ten_mbit_clean','workload':'upload_mixed','loss_trace':True},profile,config,11,folder)
            value=read_json(folder/'manifest.json')
            self.assertEqual(value['effective_variant_environment']['RDP_BENCH_LOSS_TRACE'],'1')
            self.assertEqual(value['invariants']['peer_environment']['RDP_BENCH_LOSS_TRACE'],'1')

    def test_missing_sealed_files_are_withheld_before_any_trace_read(self):
        value=trace_trial({'validity':'INVALID_ARTIFACT','artifact_directory':'/missing/trial',
                          'invariants':{'case':{'loss_trace':True}}})
        self.assertTrue(value['enabled']);self.assertFalse(value['valid'])
        self.assertIn('withheld',value['reasons'][0])

    def test_invalid_requested_trace_cannot_enter_strict_pair(self):
        def trial(variant,requested,trace):
            return apply_trace_eligibility(dict(trial_id=variant,case_id='case',comparison_key='same',variant=variant,seed=29,
                validity='PROVISIONAL_UNCALIBRATED',sut_outcome='COMPLETE',artifact_intact_timing_valid_complete=True,
                validated_lab_complete=True,invariants={'case':{'loss_trace':requested}},loss_trace=trace,
                metrics={'client.probe_rtt_ms.p99':100}))
        reference=trial('timer-q48',True,{'enabled':True,'valid':False,'reasons':['relay offer count mismatch']})
        candidate=trial('fast-q48',True,{'enabled':True,'valid':True})
        comparisons,_=pair([reference,candidate],'timer-q48','fast-q48',('client.probe_rtt_ms.p99',))
        bysubset={r['subset']:r for r in comparisons}
        self.assertEqual(len(bysubset['all_observed_diagnostic']['pairs']),1)
        self.assertEqual(bysubset['artifact_intact_timing_valid_complete']['pairs'],[])
        self.assertEqual(bysubset['validated_lab_complete_only']['pairs'],[])
        self.assertEqual(reference['trace_audit_reasons'],['relay offer count mismatch'])
        absent=trial('timer-q48',True,{'enabled':False,'valid':None,'reasons':['loss trace disabled']})
        self.assertFalse(absent['artifact_intact_timing_valid_complete'])
        reference=trial('reference-q48',False,{'enabled':False,'valid':None})
        candidate=trial('candidate-q48',False,{'enabled':False,'valid':None})
        comparisons,_=pair([reference,candidate],'reference-q48','candidate-q48',('client.probe_rtt_ms.p99',))
        self.assertEqual(len(next(r for r in comparisons if r['subset']=='artifact_intact_timing_valid_complete')['pairs']),1)

if __name__=='__main__':unittest.main()
