"""Independent, header-only UDP2/pcap decoder for recovery audits.

No TLS decoding, Scapy dependency, or production transport parser is used.
Ciphertext snap truncation is allowed after every visible header is complete.
Capture absence is not packet-loss proof without external capture-health evidence.
"""
from __future__ import annotations
import hashlib
import ipaddress
from pathlib import Path
import json
import struct

ACK, DATA, ACKVEC, AOA, OVERHEAD, DELAYACK = 1, 4, 8, 16, 64, 256
KNOWN_FLAGS = ACK | DATA | ACKVEC | AOA | OVERHEAD | DELAYACK
# Windows captures set this otherwise undocumented bit on DATA. It adds no
# visible field in the independently correlated capture/core layouts. Retain
# it explicitly instead of silently treating every unknown flag as understood.
OBSERVED_OPAQUE_FLAGS = 0x200


class PacketError(ValueError):
    pass


def decode_ack(data):
    if len(data) < 7:
        raise PacketError('truncated ACK fields')
    count, scale = data[6] & 15, data[6] >> 4
    used = 7 + count
    if len(data) < used:
        raise PacketError('truncated ACK additions')
    gap = data[5]
    return {'base': int.from_bytes(data[:2], 'little'), 'semantics': 'cumulative',
            'received_ts24': int.from_bytes(data[2:5], 'little'),
            'send_gap_ms': gap if gap != 255 else None, 'send_gap_valid': gap != 255,
            'num_delayed': count, 'scale': scale,
            'arrival_gaps_us': [value << scale for value in data[7:used]]}, used


def decode_ackvec(data):
    if len(data) < 3:
        raise PacketError('truncated ACKVEC header')
    base = int.from_bytes(data[:2], 'little')
    size, timestamp = data[2] & 127, bool(data[2] & 128)
    start = 7 if timestamp else 3
    end = start + size
    if len(data) < end:
        raise PacketError('truncated ACKVEC entries/timestamp')
    received = []
    for byte in data[start:end]:
        if byte & 128:
            run = byte & 63
            received.extend([bool(byte & 64)] * run)
        else:
            received.extend(bool(byte & (1 << bit)) for bit in range(7))
    if not received:
        raise PacketError('empty ACKVEC coverage')
    result = {'base': base, 'encoded_size': size, 'decoded_count': len(received),
              'positive_sequences': [(base + i) & 65535 for i, value in enumerate(received) if value],
              'negative_sequences': [(base + i) & 65535 for i, value in enumerate(received) if not value],
              'received_states': received, 'encoded_hex': data[:end].hex(),
              'timestamp_present': timestamp}
    if timestamp:
        gap = data[6]
        result.update(received_ts24=int.from_bytes(data[3:6], 'little'),
                      send_gap_ms=gap if gap != 255 else None, send_gap_valid=gap != 255)
    return result, end


def decode_udp2(data, wire_length=None):
    """Return metadata only; wire_length is the original UDP payload length."""
    wire_length = len(data) if wire_length is None else wire_length
    if wire_length < len(data) or wire_length < 8 or len(data) < 8:
        raise PacketError('truncated/invalid protected prefix')
    restored = bytearray(data)
    restored[0], restored[7] = restored[7], restored[0]
    prefix = restored[0]
    packet_type, short = (prefix >> 1) & 15, prefix >> 5
    if prefix & 1 or packet_type not in (0, 8):
        raise PacketError('not a supported UDP2 packet prefix')
    if short < 7:
        if wire_length != 8 or len(data) != 8:
            raise PacketError('invalid short UDP2 datagram length')
        layout = bytes(restored[1:1 + short])
        layout_wire_length = short
    else:
        layout = bytes(restored[1:])
        layout_wire_length = wire_length - 1
    if len(layout) < 2:
        raise PacketError('truncated UDP2 flags')
    header = int.from_bytes(layout[:2], 'little')
    flags, log_window = header & 4095, header >> 12
    if not flags or flags & ~(KNOWN_FLAGS | OBSERVED_OPAQUE_FLAGS) or flags & ACK and flags & ACKVEC:
        raise PacketError('invalid UDP2 flags')
    cursor = 2
    def take(length, field):
        nonlocal cursor
        if cursor + length > len(layout):
            raise PacketError('truncated ' + field)
        value = layout[cursor:cursor + length]
        cursor += length
        return value
    out = {'flags': flags, 'log_window': log_window, 'dummy': packet_type == 8,
           'opaque_flags': flags & OBSERVED_OPAQUE_FLAGS,
           'wire_length': wire_length, 'captured_length': len(data),
           'ack': None, 'ackvec': None, 'aoa': None,
           'data_seq': None, 'channel_seq': None}
    if flags & ACK:
        out['ack'], used = decode_ack(layout[cursor:])
        cursor += used
    if flags & OVERHEAD:
        out['overhead'] = take(1, 'overhead')[0]
    if flags & DELAYACK:
        value = take(3, 'DelayAckInfo')
        out['delay_ack'] = {'max_delayed': value[0], 'timeout_ms': int.from_bytes(value[1:], 'little')}
    if flags & AOA:
        out['aoa'] = int.from_bytes(take(2, 'AckOfAcks'), 'little')
    if flags & DATA:
        out['data_seq'] = int.from_bytes(take(2, 'DataHeader'), 'little')
    if flags & ACKVEC:
        out['ackvec'], used = decode_ackvec(layout[cursor:])
        cursor += used
    if flags & DATA:
        out['channel_seq'] = int.from_bytes(take(2, 'DataBody channel'), 'little')
        out['data_bytes'] = layout_wire_length - cursor
        if out['data_bytes'] < 0:
            raise PacketError('invalid DATA body length')
        out['data_bytes_captured'] = len(layout) - cursor
        out['ciphertext_truncated'] = len(layout) < layout_wire_length
    elif cursor != layout_wire_length:
        raise PacketError('trailing bytes without DATA')
    out['header_bytes'] = cursor + 1
    out['header_sha256'] = hashlib.sha256(bytes([prefix]) + layout[:cursor]).hexdigest()
    return out


def extract_udp(frame, linktype, original_length=None):
    original_length = len(frame) if original_length is None else original_length
    if linktype == 1:
        if len(frame) < 14:
            raise PacketError('truncated Ethernet header')
        offset, protocol = 14, int.from_bytes(frame[12:14], 'big')
        while protocol in (0x8100, 0x88a8, 0x9100):
            if len(frame) < offset + 4:
                raise PacketError('truncated VLAN header')
            protocol = int.from_bytes(frame[offset + 2:offset + 4], 'big'); offset += 4
    elif linktype == 113:
        if len(frame) < 16:
            raise PacketError('truncated Linux SLL header')
        offset, protocol = 16, int.from_bytes(frame[14:16], 'big')
    elif linktype == 276:
        if len(frame) < 20:
            raise PacketError('truncated Linux SLL2 header')
        offset, protocol = 20, int.from_bytes(frame[:2], 'big')
    elif linktype in (101, 228):
        offset, protocol = 0, 0x0800
    else:
        raise PacketError('unsupported pcap linktype ' + str(linktype))
    if protocol != 0x0800:
        return None
    if len(frame) < offset + 20 or frame[offset] >> 4 != 4:
        raise PacketError('truncated/invalid IPv4 header')
    ihl = (frame[offset] & 15) * 4
    if ihl < 20 or len(frame) < offset + ihl:
        raise PacketError('truncated IPv4 options')
    total = int.from_bytes(frame[offset + 2:offset + 4], 'big')
    if total > original_length - offset:
        raise PacketError('declared IPv4 length exceeds original captured frame')
    if frame[offset + 9] != 17:
        return None
    fragment = int.from_bytes(frame[offset + 6:offset + 8], 'big')
    if fragment & 0x3fff:
        raise PacketError('fragmented UDP needs reassembly; not classified')
    udp = offset + ihl
    if total < ihl + 8 or len(frame) < udp + 8:
        raise PacketError('truncated UDP header')
    source_port, dest_port, length = struct.unpack('!HHH', frame[udp:udp + 6])
    if length < 8 or length != total - ihl:
        raise PacketError('invalid UDP/IP lengths or aggregated capture')
    return {'source_ip': str(ipaddress.IPv4Address(frame[offset + 12:offset + 16])),
            'dest_ip': str(ipaddress.IPv4Address(frame[offset + 16:offset + 20])),
            'source_port': source_port, 'dest_port': dest_port,
            'udp_payload': frame[udp + 8:udp + length], 'udp_wire_length': length - 8}


def read_pcap(path):
    """Classic pcap framing; capture-drop counts require tcpdump receipts."""
    path = Path(path)
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    result = {'path': str(path), 'sha256': digest.hexdigest(),
              'packets': [], 'errors': [], 'non_udp_ipv4': 0, 'udp2_unclassified': 0,
              'capture_drop_count': None, 'absence_proof_qualified': False}
    magic = {b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000),
             b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1)}
    with path.open('rb') as stream:
        header = stream.read(24)
        if len(header) != 24 or header[:4] not in magic:
            raise PacketError('not a supported classic pcap header')
        endian, factor = magic[header[:4]]
        major, minor, _zone, _sigfigs, snaplen, network = struct.unpack(endian + 'HHIIII', header[4:])
        if (major, minor) != (2, 4) or not snaplen:
            raise PacketError('invalid pcap version/snaplen')
        result.update(linktype=network & 0xffff, snaplen=snaplen, timestamp_resolution_ns=factor)
        index = 0
        while True:
            record = stream.read(16)
            if not record:
                break
            index += 1
            if len(record) != 16:
                result['errors'].append({'packet': index, 'error': 'truncated pcap record header'}); break
            sec, frac, captured, original = struct.unpack(endian + 'IIII', record)
            if captured > snaplen or captured > original or captured > 16 * 1024 * 1024 or frac * factor >= 1_000_000_000:
                result['errors'].append({'packet': index, 'error': 'invalid pcap record dimensions'}); break
            frame = stream.read(captured)
            if len(frame) != captured:
                result['errors'].append({'packet': index, 'error': 'truncated pcap frame'}); break
            try:
                ip = extract_udp(frame, result['linktype'], original)
                if ip is None:
                    result['non_udp_ipv4'] += 1; continue
                payload = ip.pop('udp_payload')
                # Recognize the initial v1 SYN envelope rather than reporting
                # its prefix as a malformed version-2 DATA packet.
                if len(payload) >= 16:
                    syn_flags = int.from_bytes(payload[6:8], 'big')
                    up, down = struct.unpack('!HH', payload[12:16])
                    if syn_flags & 1 and not syn_flags & ~0x1fff and min(up, down) >= 16 and (
                            ip['udp_wire_length'] >= min(up, down)):
                        result['udp2_unclassified'] += 1; continue
                decoded = decode_udp2(payload, ip['udp_wire_length'])
                decoded.update(ip, packet_index=index, time_ns=sec * 1_000_000_000 + frac * factor,
                               frame_captured_bytes=captured, frame_original_bytes=original)
                result['packets'].append(decoded)
            except PacketError as error:
                result['errors'].append({'packet': index, 'error': str(error)})
    result['record_count'] = index
    return result


def acknowledgement_covers(packet, sequence):
    """Coverage in the active modular half-space; retirement context is separate."""
    if packet.get('ack'):
        return ((packet['ack']['base'] - sequence) & 65535) < 32768
    vector = packet.get('ackvec')
    return bool(vector and sequence in vector['positive_sequences'])


def audit_channel(offered, received, vm_ip, channel, *, client_port=None, capture_health=None):
    def selected(packet, inbound=True):
        ip = packet['source_ip'] if inbound else packet['dest_ip']
        port = packet['dest_port'] if inbound else packet['source_port']
        return ip == vm_ip and (client_port is None or port == client_port)
    offers = [packet for packet in offered['packets'] if selected(packet) and
              packet['channel_seq'] == channel and not packet['dummy']]
    arrivals = [packet for packet in received['packets'] if selected(packet) and
                packet['channel_seq'] == channel and not packet['dummy']]
    acknowledgements = [packet for packet in offered['packets'] if selected(packet, False) and
                        (packet.get('ack') or packet.get('ackvec')) and not packet['dummy']]
    quality = {}
    health_errors = []
    if capture_health is not None and not isinstance(capture_health, dict):
        health_errors.append('capture health must be an object')
        capture_health = None
    for name, capture in (('offered', offered), ('received', received)):
        health = (capture_health or {}).get(name) or {}
        if not isinstance(health, dict):
            health_errors.append(name + ' capture health must be an object')
            health = {}
        drops = health.get('kernel_drops')
        valid_drops = isinstance(drops, int) and not isinstance(drops, bool) and drops >= 0
        quality[name] = {'header_parse_errors': len(capture['errors']),
                         'kernel_drops': drops if valid_drops else None,
                         'complete': health.get('complete') is True,
                         'absence_proof_qualified': not capture['errors'] and
                         health.get('complete') is True and valid_drops and drops == 0}
    attempts = []
    for packet in offers:
        same = [arrival for arrival in arrivals if arrival['header_sha256'] == packet['header_sha256']
                and (arrival['source_ip'], arrival['dest_ip'], arrival['source_port'], arrival['dest_port']) ==
                    (packet['source_ip'], packet['dest_ip'], packet['source_port'], packet['dest_port'])
                and abs(arrival['time_ns'] - packet['time_ns']) <= 5_000_000_000]
        # Do not silently choose among repeated/wrapped labels or copies.
        matches = sorted(same, key=lambda arrival: abs(arrival['time_ns'] - packet['time_ns']))
        first_arrival = matches[0] if len(matches) == 1 else None
        ack_rows = []
        for ack in acknowledgements:
            if (packet['time_ns'] <= ack['time_ns'] <= packet['time_ns'] + 16_000_000_000 and
                    ack['dest_port'] == packet['source_port'] and ack['source_port'] == packet['dest_port'] and
                    acknowledgement_covers(ack, packet['data_seq'])):
                ack_rows.append({'packet_index': ack['packet_index'], 'time_ns': ack['time_ns'],
                                 'kind': 'cumulative' if ack.get('ack') else 'selective_vector',
                                 'base': (ack.get('ack') or ack.get('ackvec'))['base'],
                                 'epoch_and_aoa_audit_required': True})
        attempts.append({'offered_packet': packet['packet_index'], 'offered_time_ns': packet['time_ns'],
                         'data_seq': packet['data_seq'], 'channel_seq': channel,
                         'header_sha256': packet['header_sha256'],
                         'received_match_count': len(matches),
                         'received_packet': first_arrival['packet_index'] if first_arrival else None,
                         'received_time_ns': first_arrival['time_ns'] if first_arrival else None,
                         'captured_ack_coverage': ack_rows,
                         'no_postloss_match_is_loss_proof': False})
    return {'version': 1, 'channel': channel, 'vm_ip': vm_ip, 'client_port': client_port,
            'capture_quality': quality, 'attempts': attempts,
            'capture_health_errors': health_errors,
            'received_channel_packets': len(arrivals), 'offered_channel_packets': len(offers),
            'notes': ['Capture absence alone is not packet-loss or sender-stop proof.',
                      'eth0 pre-impairment and bench_rx post-impairment ordering must be independently verified.',
                      'Cumulative ACK coverage is evaluated in a 16-bit half-space; valid AoA retirement and sequence epochs must be audited separately.',
                      'ACKVEC positives are explicit; no positive claim is invented for sequences below its base.',
                      'Matching uses complete visible headers and a five-second timestamp window; ambiguous repeated matches remain unknown.',
                      'Snap-truncated encrypted bodies do not invalidate complete visible transport headers.']}


def audit_receiver_trace(path, channel=None, *, target_sequences=None, reference_limit=20000):
    """Decode the opt-in core ledger and audit unseen lower-edge advancement.

    A positive event is not qualified if metadata/footer/sequence completeness
    fail. The timeline remains useful diagnostic evidence in that case.
    """
    records = []
    errors = []
    with Path(path).open() as stream:
        for number, line in enumerate(stream, 1):
            try:
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise ValueError('record is not an object')
                records.append(value)
            except (ValueError, TypeError) as error:
                errors.append({'line': number, 'error': str(error)})
    metadata = [row for row in records if row.get('type') == 'metadata']
    footers = [row for row in records if row.get('type') == 'footer']
    events = [row for row in records if row.get('type') == 'event']
    if len(metadata) != 1 or len(footers) != 1:
        errors.append({'error': 'exactly one trace metadata/footer required'})
    meta, footer = (metadata or [{}])[0], (footers or [{}])[0]
    if meta.get('schema_version') != 1 or isinstance(meta.get('schema_version'), bool):
        errors.append({'error': 'unsupported recovery trace schema'})
    complete = bool(meta.get('valid') is True and footer.get('valid') is True and
                    meta.get('armed') is True and meta.get('dropped') == 0 and footer.get('dropped') == 0 and
                    meta.get('count') == footer.get('count') == len(events))
    selected_sequences = None if target_sequences is None else set(target_sequences)
    unseen = {}
    slides, emitted, coverage = [], [], []
    coverage_count = 0
    target = []
    last_time = None
    for index, event in enumerate(events):
        try:
            if event.get('seq') != index or isinstance(event.get('seq'), bool):
                raise PacketError('trace event sequence gap')
            timestamp = event['time_ns']
            if isinstance(timestamp, bool) or not isinstance(timestamp, int) or timestamp < 0 or (
                    last_time is not None and timestamp < last_time):
                raise PacketError('invalid/reversed trace timestamp')
            last_time = timestamp
            kind = event.get('event')
            if kind not in ('arm', 'rx_data', 'rx_aoa', 'auto_slide', 'window_progress',
                            'tx_ack', 'tx_data', 'channel_progress', 'gap', 'failure'):
                raise PacketError('unknown trace event kind')
            for field in ('data_seq', 'channel_seq', 'old_base', 'new_base', 'aoa'):
                if field in event and (isinstance(event[field], bool) or
                                      not isinstance(event[field], int) or not 0 <= event[field] <= 65535):
                    raise PacketError('invalid trace ' + field)
            if kind == 'rx_data':
                sequence = event['data_seq']
                if event.get('detail') in (1, 2, 3, 4):
                    unseen.pop(sequence, None)
                if channel is None or event.get('channel_seq') == channel:
                    target.append(event)
            elif kind == 'auto_slide':
                old, new, count = event['old_base'], event['new_base'], event['bytes']
                if count != ((new - old) & 65535) or not 0 < count < 32768:
                    raise PacketError('invalid auto-slide interval')
                bitmap = bytes.fromhex(event.get('wire_hex', ''))
                window = meta.get('receive_window')
                if isinstance(window, bool) or not isinstance(window, int) or not 0 < window <= 32768:
                    raise PacketError('invalid trace receiver window')
                if len(bitmap) * 8 < min(window, count):
                    raise PacketError('auto-slide prior bitmap incomplete')
                pending = [(old + offset) & 65535 for offset in range(count)
                           if offset >= window or not bitmap[offset // 8] & (1 << (offset % 8))]
                if len(pending) != event.get('detail'):
                    raise PacketError('auto-slide unseen count differs from bitmap')
                slide = {'event_seq': index, 'time_ns': timestamp, 'old_base': old, 'new_base': new,
                         'unseen_sequences': pending, 'unseen_count': len(pending)}
                slides.append(slide)
                for sequence in pending:
                    if selected_sequences is None or sequence in selected_sequences:
                        unseen[sequence] = slide
            elif kind == 'rx_aoa':
                # Even an AoA behind a locally auto-slid base can legitimately
                # retire an older attempt. Audit its wire value, not local progress.
                aoa = event['aoa']
                for sequence in list(unseen):
                    if 0 < ((aoa - sequence) & 65535) < 32768:
                        unseen.pop(sequence)
            elif kind == 'tx_ack':
                if event.get('status') != 1:
                    continue  # Encoding or failed send is not an emitted ACK.
                wire = bytes.fromhex(event.get('wire_hex', ''))
                flags = event['flags']
                if flags & ACK and not flags & ACKVEC:
                    ack, used = decode_ack(wire)
                    packet = {'ack': ack, 'ackvec': None}
                elif flags & ACKVEC and not flags & ACK:
                    vector, used = decode_ackvec(wire)
                    packet = {'ack': None, 'ackvec': vector}
                else:
                    raise PacketError('invalid emitted ACK flags')
                if used != len(wire):
                    raise PacketError('emitted ACK raw fields have trailing bytes')
                base = (packet.get('ack') or packet.get('ackvec'))['base']
                if base != event.get('new_base'):
                    raise PacketError('emitted ACK base differs from decoded wire')
                emitted.append({'event_seq': index, 'time_ns': timestamp, 'packet': packet})
                for sequence, slide in unseen.items():
                    explicit = acknowledgement_covers(packet, sequence)
                    prefix = bool(packet.get('ackvec') and 0 < ((base - sequence) & 65535) < 32768)
                    if explicit or prefix:
                        coverage_count += 1
                        if len(coverage) < reference_limit:
                            coverage.append({'slide_event_seq': slide['event_seq'], 'tx_ack_event_seq': index,
                                             'time_ns': timestamp, 'sequence': sequence, 'base': base,
                                             'claim_kind': 'cumulative' if packet.get('ack') else
                                             ('selective_positive' if explicit else 'below_vector_base')})
        except (KeyError, ValueError, TypeError) as error:
            errors.append({'event_seq': index, 'error': str(error)})
    return {'version': 1, 'metadata': meta, 'footer': footer, 'valid': complete and not errors,
            'errors': errors, 'event_count': len(events), 'unseen_auto_slides': slides,
            'emitted_acks': emitted, 'feedback_after_unseen_slide': coverage, 'target_rx_events': target,
            'feedback_reference_count': coverage_count, 'feedback_reference_limit': reference_limit,
            'feedback_reference_rows_truncated': coverage_count > len(coverage),
            'target_sequences': sorted(selected_sequences) if selected_sequences is not None else None,
            'notes': ['Only status=1 ACK events prove actual transport send success.',
                      'Receiver auto-slide over unseen entries is distinct from peer-AoA retirement.',
                      'Below-vector-base feedback is reported separately from explicit bitmap positives.',
                      'Trace overflow/incomplete framing prevents complete-history absence proof.',
                      'Monotonic trace timestamps need a separate wall-clock alignment for pcap correlation.']}
