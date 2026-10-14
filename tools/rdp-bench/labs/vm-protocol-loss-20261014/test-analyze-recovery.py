"""Small offline fixtures; no VM traffic, real captures or TLS plaintext used."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC=importlib.util.spec_from_file_location('recovery_audit',Path(__file__).with_name('analyze-recovery.py'))
a=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(a)
ARM=1_000_000_000
UNIX=100_000_000_000


def protect(flags,fields,dummy=False):
    layout=struct.pack('<H',0xf000|flags)+fields
    wire=bytearray(bytes([(min(7,len(layout))<<5)|(16 if dummy else 0)])+layout)
    wire.extend(bytes(max(0,8-len(wire))))
    wire[0],wire[7]=wire[7],wire[0]
    return bytes(wire)


def ack(sequence):
    return protect(a.ACK,struct.pack('<H',sequence)+bytes(5))


def data(sequence,channel=40):
    return protect(a.DATA,struct.pack('<HH',sequence,channel)+b'PRIVATE-CIPHERTEXT-FIXTURE'*50)


def frame(payload,reverse=False,fragment=0):
    src,dst=socket.inet_aton('192.168.64.2'),socket.inet_aton('172.17.0.2')
    sport,dport=3389,50000
    if reverse:src,dst,sport,dport=dst,src,dport,sport
    udp=struct.pack('!HHHH',sport,dport,len(payload)+8,0)+payload
    ip=struct.pack('!BBHHHBBH4s4s',0x45,0,len(udp)+20,1,fragment,64,17,0,src,dst)
    return bytes(12)+b'\x08\x00'+ip+udp


def pcap(path,rows):
    content=b'\xd4\xc3\xb2\xa1'+struct.pack('<HHIIII',2,4,0,0,256,1)
    for ns,wire,captured in rows:
        part=wire[:captured]
        content+=struct.pack('<IIII',ns//1_000_000_000,(ns%1_000_000_000)//1000,len(part),len(wire))+part
    path.write_bytes(content)


def event(kind,t,**kwargs):
    value=dict(kind='event',event=kind,time_ns=t,data_seq=0,channel_seq=0,status=0,detail=0,
               length=0,base=10,highest=9,next_channel=40,buffered=0,peer_age_ms=1,gap_age_ms=0,
               flags=0,header_fragment_hex='')
    value.update(kwargs)
    return value


def tx_header(wire,t,status=0):
    restored=bytearray(wire);restored[0],restored[7]=restored[7],restored[0]
    decoded=a.decode_udp2(wire)
    header=restored[:decoded['header_bytes']]
    values=[]
    for offset in range(0,len(header),32):
        values.append(event('tx_header' if not offset else 'header_continuation',t+offset,
                            status=status if not offset else offset,
                            length=len(wire),flags=decoded['flags'] if not offset else 0,
                            header_fragment_hex=header[offset:offset+32].hex()))
    return values


class RecoveryAnalyzerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def trace(self,events,overwritten=0,initial_channels=None):
        values=[] if overwritten else [event('arm',ARM+1000,status=1)]
        values.extend(events)
        values.sort(key=lambda e:e['time_ns'])
        parent=None
        for index,row in enumerate(values,overwritten):
            row['id']=index
            if row['event'] in ('tx_header','rx_header'):parent=index
            if row['event']=='header_continuation' and parent is not None:row['detail']=parent
        meta=dict(kind='metadata',schema='freerdp-pr13608-recovery-v1',event_bytes=96,capacity=128,
                  total_events=len(values)+overwritten,overwritten_events=overwritten,
                  complete_from_arm=overwritten==0,dropped_events=0,
                  arm_before_ns=ARM,arm_after_ns=ARM+1000,arm_unix_ns=UNIX,
                  wallclock_api='fixture',initial_received_phys_hex='00'*4096,
                  initial_channels=initial_channels or [])
        footer=dict(kind='footer',retained_sequence_valid=True,complete_from_arm=overwritten==0,
                    overwritten_events=overwritten,first_event_ns=values[0]['time_ns'],last_event_ns=values[-1]['time_ns'],
                    state=6,failure='the stream is stuck behind a chunk the server no longer resends' if any(e['event']=='failure' for e in values) else '')
        path=self.root/'trace.jsonl'
        path.write_text('\n'.join(json.dumps(v) for v in (meta,*values,footer))+'\n')
        return path

    def paired(self,*,status=3,tx_status=0,aoa=False,overwritten=0,drops=0,accepted_late=False,repeated=False,initial_channels=None):
        sequence=32777
        offered=self.root/'offered.pcap';received=self.root/'received.pcap'
        wire=data(sequence)
        header=ack(sequence)
        offers=[(UNIX+100_000_000,frame(wire),256),
                (UNIX+(7_000_000_000 if accepted_late else 120_000_000),frame(header,True),256)]
        if repeated:offers.insert(1,(UNIX+105_000_000,frame(wire),256))
        pcap(offered,offers)
        keepalive=protect(a.AOA,struct.pack('<H',1),dummy=True)
        pcap(received,[(UNIX,frame(keepalive),256),(UNIX+7_000_000_000,frame(keepalive),256)])
        events=[event('deliver',ARM+10_000_000,channel_seq=39),
                event('rx_data',ARM+110_000_000,data_seq=sequence,channel_seq=40,status=status,
                      base=10,buffered=1,gap_age_ms=100,length=len(wire)-7)]
        if aoa:events.append(event('rx_aoa',ARM+115_000_000,status=0,detail=sequence+1,buffered=1))
        if accepted_late:
            events.append(event('rx_data',ARM+6_000_000_000,data_seq=sequence,channel_seq=40,status=1,buffered=1))
        events+=tx_header(header,ARM+(7_000_000_000 if accepted_late else 120_000_000),tx_status)
        events.append(event('failure',ARM+18_200_000_000,buffered=100,gap_age_ms=18190,peer_age_ms=1))
        path=self.trace(events,overwritten=overwritten,initial_channels=initial_channels)
        trace=a.trace_summary(path)
        off=a.read_pcap(offered,'192.168.64.2',40)
        post=a.read_pcap(received,'192.168.64.2',40)
        details=a.trace_details(path,trace,40,{sequence})
        health={name:dict(complete=True,kernel_drops=drops,capture_limit_reached=False) for name in ('offered','received')}
        report=a.analyze(off,post,trace,details,40,health)
        return report,trace,details

    def test_standard_and_dummy_headers_do_not_return_ciphertext(self):
        decoded=a.decode_udp2(data(10))
        self.assertEqual((decoded['data_seq'],decoded['channel_seq']),(10,40))
        self.assertNotIn('PRIVATE-CIPHERTEXT',json.dumps(decoded))
        short=data(10)[:256]
        self.assertTrue(a.decode_udp2(short,len(data(10)))['ciphertext_truncated'])
        dummy=protect(a.DATA|a.AOA,struct.pack('<HH',10,11),dummy=True)
        decoded=a.decode_udp2(dummy)
        self.assertTrue(decoded['dummy'])
        self.assertEqual(decoded['data_seq'],11)
        self.assertIsNone(decoded['channel_seq'])
        self.assertEqual(decoded['data_bytes'],0)

    def test_ackvec_rle_wrap_and_below_base_are_distinct(self):
        wire=protect(a.ACKVEC,struct.pack('<HB',65534,3)+bytes([5,0xc4,0x82]))
        decoded=a.decode_udp2(wire)
        self.assertTrue(a.covers(decoded,65534))
        self.assertFalse(a.covers(decoded,65535))
        self.assertTrue(a.covers(decoded,0))
        self.assertTrue(a.covers(decoded,8))
        self.assertFalse(a.covers(decoded,65533))
        self.assertIsNone(a.vector_state(decoded['ackvec'],65533))
        with self.assertRaises(a.DecodeError):
            a.decode_udp2(protect(a.ACKVEC,struct.pack('<HB',1,1)+b'\x80'))

    def test_serial_wrap_late_packet_and_half_space_uncertainty(self):
        serial=a.Serial16()
        self.assertEqual([serial.assign(v) for v in (65534,65535,0,65535)],[65534,65535,65536,65535])
        self.assertFalse(serial.ambiguous)
        self.assertEqual(serial.wraps,1)
        self.assertIsNone(serial.assign(32768))
        self.assertTrue(serial.ambiguous)

    def test_header_continuations_reconstruct_all_ack_bits_and_success_is_zero(self):
        wire=protect(a.ACKVEC,struct.pack('<HB',0,70)+bytes([0x7f])*70)
        for status in (0,1,2):
            with self.subTest(status=status):
                path=self.trace(tx_header(wire,ARM+1_000_000_000,status))
                trace=a.trace_summary(path)
                details=a.trace_details(path,trace,40,{10})
                self.assertTrue(trace['retained_valid'],trace['errors'])
                self.assertEqual(details['header_errors'],[])
                self.assertEqual(len(details['feedback_headers']),1)
                header=details['feedback_headers'][0]
                self.assertEqual(header['packet']['ackvec']['decoded_count'],490)
                self.assertEqual(header['send_success'],status==0)

    def test_snap_truncation_header_truncation_fragment_and_out_of_order_records(self):
        raw=frame(data(10));path=self.root/'fixture.pcap'
        pcap(path,[(UNIX+2_000_000,raw,256),(UNIX+1_000_000,frame(data(9)),256)])
        cap=a.read_pcap(path,'192.168.64.2',40)
        self.assertEqual(cap['error_count'],0)
        self.assertEqual(cap['body_snap_truncations'],2)
        self.assertEqual(cap['timestamp_order_reversals'],1)
        self.assertFalse(cap['serial_ambiguous'])
        pcap(path,[(UNIX,raw,47)])
        self.assertEqual(a.read_pcap(path,'192.168.64.2',40)['error_count'],1)
        pcap(path,[(UNIX,frame(data(10),fragment=0x2000),256)])
        self.assertIn('fragmented',a.read_pcap(path,'192.168.64.2',40)['errors'][0]['error'])

    def test_explicit_rejected_alias_ack_is_qualified_only_with_complete_scopes(self):
        report,trace,details=self.paired()
        self.assertTrue(trace['complete_from_arm'],trace['errors'])
        self.assertEqual(details['header_errors'],[])
        self.assertEqual(report['conclusion']['qualified_unretained_ack_count'],1)
        alias=report['aliases'][0]
        self.assertTrue(alias['no_postloss_match_qualified'])
        self.assertEqual(alias['native_rx'][0]['decision'],'outside packet window')
        self.assertEqual(alias['feedback'][0]['kind'],'cumulative')
        self.assertTrue(alias['feedback'][0]['sequence_epoch_verified'])

    def test_peer_aoa_retirement_is_not_a_false_receipt_or_chunk_delivery(self):
        report,_,_=self.paired(aoa=True)
        self.assertEqual(report['conclusion']['qualified_unretained_ack_count'],0)
        self.assertTrue(report['aliases'][0]['feedback'][0]['peer_aoa_retirement_events'])
        self.assertEqual(report['native_channel_deliveries'],[])
        self.assertTrue(report['conclusion']['watchdog'])
        self.assertEqual(report['conclusion']['peer_age_ms'],1)

    def test_accepted_alias_even_after_match_window_prevents_false_ack(self):
        report,_,_=self.paired(accepted_late=True)
        self.assertEqual(report['conclusion']['qualified_unretained_ack_count'],0)
        self.assertTrue(report['aliases'][0]['feedback'][0]['native_received_before_ack'])

    def test_overwritten_prefix_or_kernel_drop_blocks_absence_and_false_ack(self):
        for kwargs in (dict(overwritten=5),dict(drops=1),dict(repeated=True)):
            with self.subTest(kwargs=kwargs):
                report,_,_=self.paired(**kwargs)
                self.assertEqual(report['conclusion']['qualified_unretained_ack_count'],0)
                if kwargs.get('drops') or kwargs.get('repeated'):
                    self.assertFalse(report['aliases'][0]['no_postloss_match_qualified'])
                if kwargs.get('overwritten'):
                    self.assertFalse(report['native_trace']['complete_from_arm'])

    def test_initial_retained_channel_and_failed_header_send_prevent_false_ack(self):
        for kwargs in (dict(initial_channels=[[40,1000]]),dict(tx_status=1),dict(tx_status=2)):
            with self.subTest(kwargs=kwargs):
                report,_,_=self.paired(**kwargs)
                self.assertEqual(report['conclusion']['qualified_unretained_ack_count'],0)

    def test_longest_completed_hole_and_delivery_interval_are_measured(self):
        events=[event('deliver',ARM+10_000_000,channel_seq=39),
                event('rx_data',ARM+20_000_000,data_seq=11,channel_seq=41,status=1,buffered=1),
                event('rx_aoa',ARM+11_900_000_000,detail=12,buffered=1,gap_age_ms=11890),
                event('deliver',ARM+12_000_000_000,channel_seq=40,next_channel=41),
                event('deliver',ARM+12_100_000_000,channel_seq=41,next_channel=42),
                event('rx_data',ARM+13_000_000_000,data_seq=14,channel_seq=43,status=1,next_channel=42,buffered=1),
                event('deliver',ARM+13_100_000_000,channel_seq=42,next_channel=43)]
        trace=a.trace_summary(self.trace(events))
        gap=trace['longest_buffered_gaps'][0]
        self.assertEqual(gap['channel'],40)
        self.assertTrue(gap['recovered'])
        self.assertEqual(gap['duration_ms'],11980)
        self.assertEqual(gap['max_gap_age_ms'],11890)
        self.assertEqual(trace['max_delivery_interval']['duration_ns'],11_990_000_000)

    def test_client_retry_retire_numeric_labels_are_kept_separate(self):
        events=[event('retry',ARM+1_000_000_000,data_seq=10,channel_seq=40,detail=9,length=600),
                event('retire',ARM+2_000_000_000,data_seq=10,channel_seq=40)]
        path=self.trace(events);trace=a.trace_summary(path)
        details=a.trace_details(path,trace,40,{10})
        self.assertEqual([e['event'] for e in details['client_outbound_retry_retire']],['retry','retire'])
        self.assertEqual(details['rx'],[])
        self.assertEqual(details['deliveries'],[])

    def test_bad_trace_footer_and_orphan_continuation_restrict_verdicts(self):
        path=self.trace([event('header_continuation',ARM+1_000_000_000,status=32,header_fragment_hex='00'*8)])
        trace=a.trace_summary(path)
        details=a.trace_details(path,trace,40,{1})
        self.assertTrue(details['header_errors'])
        records=[json.loads(line) for line in path.read_text().splitlines()]
        records[-1]['overwritten_events']=1
        path.write_text('\n'.join(json.dumps(row) for row in records)+'\n')
        self.assertFalse(a.trace_summary(path)['retained_valid'])

    def test_cli_output_cannot_change_a_sealed_trial(self):
        trial=self.root/'trial';trial.mkdir();(trial/'SHA256SUMS.json').write_text('{}')
        argv=['analyze-recovery.py','--trial',str(trial),'--vm-ip','192.168.64.2','--output',str(trial/'analysis')]
        with patch.object(sys,'argv',argv),contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:a.main()
        self.assertEqual(error.exception.code,2)
        self.assertFalse((trial/'analysis.json').exists())

    def test_three_emitted_header_fingerprints_bind_one_of_two_udp_tunnels(self):
        offered=dict(flows={'a:1':3,'b:2':3},feedback=[])
        headers=[]
        for number in range(3):
            fingerprint=str(number)
            offered['feedback'].append(dict(header_sha256=fingerprint,time_ns=100+number,flow='a:1',packet_index=number+1))
            headers.append(dict(send_success=True,time_unix_ns=100+number,event_id=number,
                                packet={'header_sha256':fingerprint}))
        association=a.associate_flow(offered,dict(feedback_headers=headers),1)
        self.assertTrue(association['verified'])
        self.assertEqual(association['flow'],'a:1')
        self.assertEqual(association['unique_header_matches'],{'a:1':3})

    def test_conflicting_or_unmatched_native_headers_do_not_pool_tunnel_spaces(self):
        offered=dict(flows={'a:1':3,'b:2':3},feedback=[
            dict(header_sha256='one',time_ns=100,flow='a:1',packet_index=1),
            dict(header_sha256='two',time_ns=101,flow='b:2',packet_index=2)])
        headers=[dict(send_success=True,time_unix_ns=100,event_id=1,packet={'header_sha256':'one'}),
                 dict(send_success=True,time_unix_ns=101,event_id=2,packet={'header_sha256':'two'})]
        association=a.associate_flow(offered,dict(feedback_headers=headers),1)
        self.assertFalse(association['verified'])
        self.assertIsNone(association['flow'])
        self.assertFalse(a.associate_flow(offered,dict(feedback_headers=[]),1)['verified'])

    def test_negative_vector_and_below_base_reference_are_not_positive_receipts(self):
        sequence=32777
        for base,entry,kind in ((sequence,0,'explicit_vector_negative'),
                                (sequence+1,0xc1,'below_vector_base_nonpositive')):
            with self.subTest(kind=kind):
                feedback=protect(a.ACKVEC,struct.pack('<HB',base,1)+bytes([entry]))
                path=self.trace([event('rx_data',ARM+110_000_000,data_seq=sequence,channel_seq=40,status=3,buffered=1),
                                 *tx_header(feedback,ARM+120_000_000)])
                offered=self.root/'offered.pcap';received=self.root/'received.pcap'
                pcap(offered,[(UNIX+100_000_000,frame(data(sequence)),256),
                             (UNIX+120_000_000,frame(feedback,True),256)])
                keepalive=protect(a.AOA,struct.pack('<H',1),dummy=True)
                pcap(received,[(UNIX,frame(keepalive),256),(UNIX+7_000_000_000,frame(keepalive),256)])
                trace=a.trace_summary(path);details=a.trace_details(path,trace,40,{sequence})
                report=a.analyze(a.read_pcap(offered,'192.168.64.2',40),
                                 a.read_pcap(received,'192.168.64.2',40),trace,details,40,
                                 {n:dict(complete=True,kernel_drops=0) for n in ('offered','received')})
                self.assertTrue(report['native_flow_association']['verified'])
                reference=report['aliases'][0]['feedback'][0]
                self.assertEqual(reference['kind'],kind)
                self.assertFalse(reference['positive_receipt'])
                self.assertFalse(reference['qualified_unretained_ack'])
                self.assertEqual(report['conclusion']['qualified_unretained_ack_count'],0)


if __name__=='__main__':unittest.main()
