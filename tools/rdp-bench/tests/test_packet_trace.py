"""Independent transport header fixtures and capture-quality boundary tests."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from rdpbench.packet_trace import (ACK, DATA, ACKVEC, AOA, OVERHEAD, DELAYACK, PacketError,
                                  acknowledgement_covers, audit_channel, audit_receiver_trace, decode_ackvec,
                                  decode_udp2, extract_udp, read_pcap)


def protect(layout, dummy=False):
    short = min(len(layout), 7)
    wire = bytearray(bytes([(short << 5) | (16 if dummy else 0)]) + layout)
    wire.extend(b'\0' * max(0, 8 - len(wire)))
    wire[0], wire[7] = wire[7], wire[0]
    return bytes(wire)


def frame(payload, linktype=1, source='\xc0\xa8\x40\x02', reverse=False):
    src, dst = bytes(ord(x) for x in source), b'\xac\x11\x00\x02'
    sport, dport = (3389, 50000) if not reverse else (50000, 3389)
    if reverse:
        src, dst = dst, src
    udp = struct.pack('!HHHH', sport, dport, len(payload) + 8, 0) + payload
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, len(udp) + 20, 1, 0, 64, 17, 0, src, dst)
    link = {1: b'\0' * 12 + b'\x08\0', 113: b'\0' * 14 + b'\x08\0',
            276: b'\x08\0' + b'\0' * 18}[linktype]
    return link + ip + udp


def pcap(frames, endian='<', nanos=False, linktype=1):
    magic = (b'\x4d\x3c\xb2\xa1' if nanos else b'\xd4\xc3\xb2\xa1')
    if endian == '>':
        magic = magic[::-1]
    output = magic + struct.pack(endian + 'HHIIII', 2, 4, 0, 0, 256, linktype)
    for index, (captured, original) in enumerate(frames):
        output += struct.pack(endian + 'IIII', 100, (index + 1) * (1000 if nanos else 1),
                              len(captured), original) + captured
    return output


class PacketTraceTests(unittest.TestCase):
    def read_fixture(self, content):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'fixture.pcap'
            path.write_bytes(content)
            return read_pcap(path)

    def test_known_protected_data_and_ciphertext_snap_truncation(self):
        # Independently specified prefix swap: full layout 04f0 3412 0201 abcd.
        wire = bytes.fromhex('ab04f034120201e0cd')
        packet = decode_udp2(wire)
        self.assertEqual((packet['flags'], packet['log_window'], packet['data_seq'],
                          packet['channel_seq'], packet['data_bytes']), (DATA, 15, 0x1234, 0x102, 2))
        partial = decode_udp2(wire[:8], len(wire))
        self.assertTrue(partial['ciphertext_truncated'])
        self.assertEqual(partial['data_bytes_captured'], 1)
        self.assertEqual(partial['header_sha256'], packet['header_sha256'])
        windows = decode_udp2(protect(struct.pack('<HHH', 0xf204, 0x1234, 0x102) + b'\xab\xcd'))
        self.assertEqual(windows['opaque_flags'], 0x200)
        self.assertEqual(windows['data_bytes'], 2)
        with self.assertRaises(PacketError):
            decode_udp2(wire[:6], len(wire))

    def test_known_ack_is_cumulative_and_additions_are_timing(self):
        packet = decode_udp2(bytes.fromhex('1201f0ffff5634e0ff220304'))
        self.assertEqual(packet['ack']['base'], 65535)
        self.assertEqual(packet['ack']['received_ts24'], 0x123456)
        self.assertFalse(packet['ack']['send_gap_valid'])
        self.assertIsNone(packet['ack']['send_gap_ms'])
        self.assertEqual(packet['ack']['arrival_gaps_us'], [12, 16])
        self.assertTrue(acknowledgement_covers(packet, 65520))
        self.assertFalse(acknowledgement_covers(packet, 0))
        packet['ack']['base'] = 1
        self.assertTrue(acknowledgement_covers(packet, 65535))
        self.assertFalse(acknowledgement_covers(packet, 32769))

    def test_ackvec_bitmap_rle_timestamp_and_sequence_wrap(self):
        vector, used = decode_ackvec(bytes.fromhex('feff83030201ff05c482'))
        self.assertEqual(used, 10)
        self.assertEqual(vector['positive_sequences'], [65534, 0, 5, 6, 7, 8])
        self.assertEqual(vector['negative_sequences'], [65535, 1, 2, 3, 4, 9, 10])
        self.assertEqual(vector['decoded_count'], 13)
        self.assertEqual(vector['received_ts24'], 0x010203)
        self.assertFalse(vector['send_gap_valid'])
        self.assertFalse(acknowledgement_covers({'ackvec': vector}, 65533))
        # A zero run contributes no state; mixed vectors still carry coverage.
        self.assertEqual(decode_ackvec(bytes.fromhex('0100028001'))[0]['positive_sequences'], [1])
        for malformed in ('010000', '01000180', '0100810000', '01000305c4'):
            with self.subTest(malformed=malformed), self.assertRaises(PacketError):
                decode_ackvec(bytes.fromhex(malformed))

    def test_all_optional_fields_and_header_truncation(self):
        flags = DATA | ACKVEC | AOA | OVERHEAD | DELAYACK
        layout = struct.pack('<H', 0xf000 | flags) + b'\x32' + b'\x08\x14\0'
        layout += struct.pack('<HH', 0x1230, 0x1234) + bytes.fromhex('feff0105')
        layout += struct.pack('<H', 0x0102) + b'ciphertext'
        wire = protect(layout)
        packet = decode_udp2(wire)
        self.assertEqual(packet['aoa'], 0x1230)
        self.assertEqual(packet['delay_ack'], {'max_delayed': 8, 'timeout_ms': 20})
        self.assertEqual(packet['overhead'], 50)
        for captured in range(8, packet['header_bytes']):
            with self.subTest(captured=captured), self.assertRaises(PacketError):
                decode_udp2(wire[:captured], len(wire))
        self.assertTrue(decode_udp2(wire[:packet['header_bytes']], len(wire))['ciphertext_truncated'])

    def test_dummy_short_aoa_and_invalid_flag_prefix(self):
        packet = decode_udp2(bytes.fromhex('0010f07f00000090'))
        self.assertTrue(packet['dummy'])
        self.assertEqual(packet['aoa'], 127)
        self.assertIsNone(packet['data_seq'])
        for layout in (b'\0\xf0', struct.pack('<H', 0xf000 | ACK | ACKVEC), b'\x02\xf0'):
            with self.assertRaises(PacketError):
                decode_udp2(protect(layout))
        malformed = bytearray(bytes.fromhex('0010f07f00000090')); malformed[7] |= 1
        with self.assertRaises(PacketError):
            decode_udp2(malformed)

    def test_ethernet_sll_sll2_both_endian_and_timestamp_resolutions(self):
        payload = bytes.fromhex('ab04f034120201e0cd')
        for link in (1, 113, 276):
            for endian in ('<', '>'):
                for nanos in (False, True):
                    with self.subTest(link=link, endian=endian, nanos=nanos):
                        raw = frame(payload, link)
                        capture = self.read_fixture(pcap([(raw, len(raw))], endian, nanos, link))
                        self.assertEqual(capture['errors'], [])
                        self.assertEqual(capture['packets'][0]['time_ns'], 100_000_001_000)
                        self.assertEqual(capture['packets'][0]['source_ip'], '192.168.64.2')

    def test_packet_snap_truncation_is_not_truncated_header_or_loss(self):
        payload = protect(struct.pack('<HHH', 0xf004, 1, 6297) + b'x' * 1200)
        full = frame(payload)
        capture = self.read_fixture(pcap([(full[:160], len(full))]))
        self.assertEqual(capture['errors'], [])
        self.assertTrue(capture['packets'][0]['ciphertext_truncated'])
        self.assertEqual(capture['packets'][0]['data_bytes'], 1200)
        self.assertFalse(capture['absence_proof_qualified'])
        broken = self.read_fixture(pcap([(full[:46], len(full))]))
        self.assertTrue(broken['errors'])
        self.assertEqual(broken['packets'], [])
        malformed = self.read_fixture(pcap([(full[:160], 160)]))
        self.assertTrue(malformed['errors'])  # Missing bytes cannot be called snap truncation.

    def test_capture_truncated_records_and_fragments_remain_invalid(self):
        payload = bytes.fromhex('ab04f034120201e0cd'); raw = frame(payload)
        capture = self.read_fixture(pcap([(raw, len(raw))])[:-2])
        self.assertIn('truncated pcap frame', capture['errors'][0]['error'])
        fragmented = bytearray(raw); fragmented[20:22] = b'\x20\0'
        with self.assertRaisesRegex(PacketError, 'fragmented'):
            extract_udp(fragmented, 1)
        # The SYN+ACK SourceAck is not ffffffff; distinguish its valid v1
        # envelope from v2 prefix bytes even with snap-truncated padding.
        syn = struct.pack('!IHHIHH', 0x11223344, 64, 0x1005, 7, 1232, 1232)
        syn += b'\0' * (1232 - len(syn))
        syn_frame = frame(syn)
        captured = self.read_fixture(pcap([(syn_frame[:160], len(syn_frame))]))
        self.assertEqual(captured['errors'], [])
        self.assertEqual(captured['udp2_unclassified'], 1)

    def test_channel_audit_retains_offered_received_aliases_and_ack_candidates(self):
        data = protect(struct.pack('<HHH', 0xf004, 1, 6297) + b'x')
        retry = protect(struct.pack('<HHH', 0xf004, 2, 6297) + b'x')
        ack = protect(struct.pack('<HH', 0xf001, 2) + b'\0' * 5)
        offered_frames = [frame(data), frame(retry), frame(ack, reverse=True)]
        offered = self.read_fixture(pcap([(value, len(value)) for value in offered_frames]))
        received = self.read_fixture(pcap([(offered_frames[1], len(offered_frames[1]))]))
        # Preserve identical capture time for the actually received second attempt.
        received['packets'][0]['time_ns'] = offered['packets'][1]['time_ns']
        report = audit_channel(offered, received, '192.168.64.2', 6297)
        self.assertEqual(report['offered_channel_packets'], 2)
        self.assertEqual(report['received_channel_packets'], 1)
        self.assertEqual([row['received_match_count'] for row in report['attempts']], [0, 1])
        self.assertTrue(all(row['captured_ack_coverage'] for row in report['attempts']))
        self.assertTrue(all(not row['no_postloss_match_is_loss_proof'] for row in report['attempts']))
        self.assertFalse(report['capture_quality']['received']['absence_proof_qualified'])
        health = {'offered': {'complete': True, 'kernel_drops': 0},
                  'received': {'complete': True, 'kernel_drops': 3}}
        report = audit_channel(offered, received, '192.168.64.2', 6297, capture_health=health)
        self.assertFalse(report['capture_quality']['received']['absence_proof_qualified'])

    def trace_fixture(self, *, intervening=None, status=1, valid=True):
        bitmap = bytes([2]) + b'\0' * 15  # Original DataSeq 2 arrived; 1 and others did not.
        events = [dict(event='arm', old_base=1, new_base=1, bytes=128, wire_hex='00' * 16),
                  dict(event='rx_data', data_seq=2, channel_seq=2, detail=1),
                  dict(event='auto_slide', old_base=1, new_base=173, bytes=172,
                       detail=171, wire_hex=bitmap.hex())]
        if intervening:
            events.append(intervening)
        events.append(dict(event='tx_ack', flags=ACK, status=status, new_base=300,
                           wire_hex='2c010000000000'))
        for index, event in enumerate(events):
            event.update(type='event', seq=index, time_ns=1000 + index)
        metadata = dict(type='metadata', schema_version=1, valid=valid, armed=True,
                        capacity=32, count=len(events), dropped=0 if valid else 1, receive_window=128)
        footer = dict(type='footer', valid=valid, count=len(events), dropped=metadata['dropped'])
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'trace.jsonl'
            path.write_text('\n'.join(json.dumps(row) for row in [metadata, *events, footer]) + '\n')
            return audit_receiver_trace(path, 1, target_sequences=[1])

    def test_trace_proves_unseen_slide_then_emitted_cumulative_receipt(self):
        trace = self.trace_fixture()
        self.assertTrue(trace['valid'], trace['errors'])
        self.assertEqual(trace['unseen_auto_slides'][0]['unseen_count'], 171)
        claim = trace['feedback_after_unseen_slide'][0]
        self.assertEqual((claim['sequence'], claim['base'], claim['claim_kind']), (1, 300, 'cumulative'))
        self.assertEqual(trace['target_rx_events'], [])
        self.assertEqual(self.trace_fixture(status=0)['feedback_reference_count'], 0)

    def test_trace_aoa_retirement_and_actual_receipt_prevent_false_claim(self):
        # AoA 150 is behind the locally auto-slid base 173 but retires DataSeq 1.
        trace = self.trace_fixture(intervening=dict(event='rx_aoa', old_base=173, new_base=173,
                                                   aoa=150, bytes=0, detail=0, wire_hex='00' * 16))
        self.assertTrue(trace['valid'], trace['errors'])
        self.assertEqual(trace['feedback_reference_count'], 0)
        trace = self.trace_fixture(intervening=dict(event='rx_data', data_seq=1, channel_seq=1, detail=1))
        self.assertEqual(trace['feedback_reference_count'], 0)
        self.assertEqual(len(trace['target_rx_events']), 1)

    def test_trace_overflow_prevents_qualified_complete_history(self):
        trace = self.trace_fixture(valid=False)
        self.assertFalse(trace['valid'])
        self.assertEqual(trace['metadata']['dropped'], 1)
        self.assertTrue(trace['feedback_after_unseen_slide'])  # Positive diagnostic evidence retained.


if __name__ == '__main__':
    unittest.main()
