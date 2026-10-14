#!/usr/bin/env python3
"""Read-only, header-only audit of PR13608 recovery traces and paired classic PCAPs.

Outputs never contain/decrypt TLS payload. Native retry/retire concern the client's
send queue; they must not be confused with Windows-to-client missing chunks.
"""
from __future__ import annotations
import argparse
import bisect
from collections import Counter, defaultdict, deque
import hashlib
import ipaddress
import json
from pathlib import Path
import struct
import math

ACK, DATA, ACKVEC, AOA, OVERHEAD, DELAYACK = 1, 4, 8, 16, 64, 256
KNOWN_FLAGS = ACK | DATA | ACKVEC | AOA | OVERHEAD | DELAYACK | 0x200
RX_STATUS = {0: 'dummy/no chunk', 1: 'accepted chunk', 2: 'old packet label',
             3: 'outside packet window', 4: 'outside channel window',
             5: 'chunk store failed', 6: 'duplicate/already known chunk'}
EVENTS = {'arm', 'rx_data', 'rx_aoa', 'rx_resync', 'deliver', 'store_collision',
          'tx_header', 'rx_header', 'header_continuation', 'retry', 'retire', 'failure'}


class DecodeError(ValueError):
    pass


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for value in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(value)
    return h.hexdigest()


def integer(value, maximum=None):
    return (isinstance(value, int) and not isinstance(value, bool) and value >= 0
            and (maximum is None or value <= maximum))


def near16(raw, anchor):
    if anchor is None:
        return None
    delta = (raw - anchor) & 65535
    if delta == 32768:
        return None
    return anchor + (delta if delta < 32768 else delta - 65536)


class Serial16:
    """Nearest high-water epoch; wide reverse/half-space jumps stay ambiguous."""
    def __init__(self):
        self.highest = None
        self.ambiguous = False
        self.wraps = 0

    def assign(self, raw):
        extended = raw if self.highest is None else near16(raw, self.highest)
        if extended is None or (self.highest is not None and extended < self.highest - 4096):
            self.ambiguous = True
        if extended is not None and (self.highest is None or extended > self.highest):
            if self.highest is not None:
                self.wraps += extended // 65536 - self.highest // 65536
            self.highest = extended
        return extended


def decode_udp2(data, wire_length=None, *, protected=True):
    """Decode the complete visible header, allowing body-only snap truncation."""
    wire_length = len(data) if wire_length is None else wire_length
    if wire_length < 8 or wire_length < len(data) or len(data) < (8 if protected else 3):
        raise DecodeError('truncated protected prefix/header')
    restored = bytearray(data)
    if protected:
        restored[0], restored[7] = restored[7], restored[0]
    prefix = restored[0]
    packet_type, short = (prefix >> 1) & 15, prefix >> 5
    if prefix & 1 or packet_type not in (0, 8):
        raise DecodeError('unsupported UDP2 prefix')
    if short < 7:
        if wire_length != 8:
            raise DecodeError('short prefix in non-eight-byte datagram')
        available, logical_length = min(len(restored), 1 + short), 1 + short
    else:
        available, logical_length = len(restored), wire_length
    cursor = 1
    def take(length, label):
        nonlocal cursor
        if cursor + length > available:
            raise DecodeError('truncated visible ' + label)
        value = restored[cursor:cursor+length]
        cursor += length
        return value
    flags_word = int.from_bytes(take(2, 'flags'), 'little')
    flags = flags_word & 4095
    if not flags or flags & ~KNOWN_FLAGS or flags & ACK and flags & ACKVEC:
        raise DecodeError('unknown/inconsistent UDP2 flags')
    result = dict(flags=flags, log_window=flags_word >> 12, dummy=packet_type == 8,
                  ack=None, ackvec=None, aoa=None, data_seq=None, channel_seq=None,
                  observed_opaque_flags=flags & 0x200, wire_length=wire_length)
    if flags & ACK:
        value = take(7, 'ACK')
        extra = value[6] & 15
        additions = take(extra, 'ACK timing additions')
        result['ack'] = dict(base=int.from_bytes(value[:2], 'little'),
                             received_ts24=int.from_bytes(value[2:5], 'little'),
                             send_gap_ms=None if value[5] == 255 else value[5],
                             timing_additions=len(additions))
    if flags & OVERHEAD:
        result['overhead_bytes'] = take(1, 'overhead')[0]
    if flags & DELAYACK:
        value = take(3, 'delayed ACK settings')
        result['delay_ack'] = dict(max_delayed=value[0], timeout_ms=int.from_bytes(value[1:], 'little'))
    if flags & AOA:
        result['aoa'] = int.from_bytes(take(2, 'AoA'), 'little')
    if flags & DATA:
        result['data_seq'] = int.from_bytes(take(2, 'DataSeq'), 'little')
    if flags & ACKVEC:
        value = take(3, 'ACK vector')
        control = value[2]
        timestamp = take(4, 'ACK vector timestamp') if control & 128 else None
        entries = take(control & 127, 'ACK vector entries')
        count = sum((byte & 63) if byte & 128 else 7 for byte in entries)
        if not count or count >= 32768:
            raise DecodeError('empty/ambiguous ACK vector span')
        result['ackvec'] = dict(base=int.from_bytes(value[:2], 'little'), decoded_count=count,
                                entries_hex=entries.hex(), timestamp_present=timestamp is not None)
    if flags & DATA and not result['dummy']:
        result['channel_seq'] = int.from_bytes(take(2, 'ChannelSeq'), 'little')
    if not flags & DATA and cursor != logical_length:
        raise DecodeError('unexpected bytes after feedback header')
    result.update(header_bytes=cursor, header_sha256=hashlib.sha256(restored[:cursor]).hexdigest(),
                  data_bytes=max(0, logical_length-cursor) if flags & DATA and not result['dummy'] else 0,
                  ciphertext_truncated=available < logical_length)
    return result


def vector_state(vector, sequence):
    offset = (sequence - vector['base']) & 65535
    if offset >= vector['decoded_count']:
        return None
    for byte in bytes.fromhex(vector['entries_hex']):
        count = byte & 63 if byte & 128 else 7
        if offset < count:
            return bool(byte & 64) if byte & 128 else bool(byte & (1 << offset))
        offset -= count
    return None


def covers(packet, sequence):
    if packet.get('ack'):
        return ((packet['ack']['base'] - sequence) & 65535) < 32768
    return bool(packet.get('ackvec') and vector_state(packet['ackvec'], sequence) is True)


def ipv4_udp(frame, linktype, original):
    if linktype == 1:
        if len(frame) < 14:
            raise DecodeError('truncated Ethernet')
        offset, protocol = 14, int.from_bytes(frame[12:14], 'big')
        while protocol in (0x8100, 0x88a8, 0x9100):
            if len(frame) < offset + 4:
                raise DecodeError('truncated VLAN')
            protocol = int.from_bytes(frame[offset+2:offset+4], 'big'); offset += 4
    elif linktype == 113:
        if len(frame) < 16:
            raise DecodeError('truncated SLL')
        offset, protocol = 16, int.from_bytes(frame[14:16], 'big')
    elif linktype == 276:
        if len(frame) < 20:
            raise DecodeError('truncated SLL2')
        offset, protocol = 20, int.from_bytes(frame[:2], 'big')
    elif linktype in (101, 228):
        offset, protocol = 0, 0x0800
    else:
        raise DecodeError('unsupported link type')
    if protocol != 0x0800:
        return None
    if len(frame) < offset + 20 or frame[offset] >> 4 != 4:
        raise DecodeError('truncated/invalid IPv4')
    ihl = (frame[offset] & 15) * 4
    total = int.from_bytes(frame[offset+2:offset+4], 'big')
    if ihl < 20 or len(frame) < offset + ihl or total < ihl or total > original-offset:
        raise DecodeError('invalid IPv4 dimensions/options')
    if frame[offset+9] != 17:
        return None
    if int.from_bytes(frame[offset+6:offset+8], 'big') & 0x3fff:
        raise DecodeError('fragmented IPv4 UDP; reassembly not attempted')
    udp = offset + ihl
    if total < ihl + 8 or len(frame) < udp+8:
        raise DecodeError('truncated UDP')
    source, dest, length = struct.unpack('!HHH', frame[udp:udp+6])
    if length != total-ihl or length < 8:
        raise DecodeError('invalid UDP/IP length or offload aggregation')
    return dict(source_ip=str(ipaddress.IPv4Address(frame[offset+12:offset+16])),
                dest_ip=str(ipaddress.IPv4Address(frame[offset+16:offset+20])),
                source_port=source, dest_port=dest,
                body=frame[udp+8:udp+length], wire_length=length-8)


def read_pcap(path, vm_ip, channel, *, client_port=None, window_ns=None):
    """Retain selected headers/anchors; encrypted bytes are immediately discarded."""
    result = dict(path=str(Path(path).resolve()), sha256=sha256(path), errors=[], error_count=0,
                  record_count=0, parsed_udp2=0, syn_envelopes=0, body_snap_truncations=0,
                  target=[], feedback=[], aoa=[], anchors={}, flows={}, serial_ambiguous=False,
                  timestamp_order_reversals=0,retention_window_ns=window_ns)
    spaces, channel_spaces = {}, {}
    with Path(path).open('rb') as stream:
        header = stream.read(24)
        magic = {b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000),
                 b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1)}
        if len(header) != 24 or header[:4] not in magic:
            raise DecodeError('unsupported/truncated classic PCAP header')
        endian, resolution = magic[header[:4]]
        major, minor, _, _, snaplen, link = struct.unpack(endian+'HHIIII', header[4:])
        if (major, minor) != (2, 4) or not snaplen:
            raise DecodeError('invalid PCAP header')
        result.update(snaplen=snaplen, linktype=link & 0xffff, resolution_ns=resolution,
                      first_time_ns=None, last_time_ns=None)
        previous = None
        while True:
            record = stream.read(16)
            if not record:
                break
            result['record_count'] += 1
            index = result['record_count']
            try:
                if len(record) != 16:
                    raise DecodeError('truncated PCAP record header')
                sec, frac, captured, original = struct.unpack(endian+'IIII', record)
                if captured > min(snaplen, original, 16*1024*1024) or frac*resolution >= 1_000_000_000:
                    raise DecodeError('invalid PCAP record dimensions')
                frame = stream.read(captured)
                if len(frame) != captured:
                    raise DecodeError('truncated PCAP frame')
                timestamp = sec*1_000_000_000 + frac*resolution
                result['first_time_ns'] = timestamp if result['first_time_ns'] is None else min(timestamp, result['first_time_ns'])
                result['last_time_ns'] = timestamp if result['last_time_ns'] is None else max(timestamp, result['last_time_ns'])
                if previous is not None and timestamp < previous:
                    result['timestamp_order_reversals'] += 1
                previous = timestamp
                udp = ipv4_udp(frame, result['linktype'], original)
                if udp is None:
                    continue
                inbound = udp['source_ip'] == vm_ip and udp['source_port'] == 3389
                outbound = udp['dest_ip'] == vm_ip and udp['dest_port'] == 3389
                if not inbound and not outbound:
                    continue
                flow = (udp['dest_ip'], udp['dest_port']) if inbound else (udp['source_ip'], udp['source_port'])
                if client_port is not None and flow[1] != client_port:
                    continue
                flowid = f'{flow[0]}:{flow[1]}'
                result['flows'][flowid] = result['flows'].get(flowid, 0) + 1
                data = udp.pop('body'); wire_length = udp.pop('wire_length')
                if len(data) >= 16:
                    syn_flags = int.from_bytes(data[6:8], 'big')
                    up, down = struct.unpack('!HH', data[12:16])
                    if syn_flags & 1 and not syn_flags & ~0x1fff and up == down and 16 <= up <= 1500 and wire_length >= up:
                        result['syn_envelopes'] += 1; continue
                packet = decode_udp2(data, wire_length)
                packet.update(udp, packet_index=index, time_ns=timestamp, flow=flowid,
                              direction='server_to_client' if inbound else 'client_to_server')
                result['parsed_udp2'] += 1
                result['body_snap_truncations'] += packet['ciphertext_truncated']
                in_window=window_ns is None or window_ns[0]<=timestamp<=window_ns[1]
                data_space = spaces.setdefault(flowid, Serial16())
                channel_space = channel_spaces.setdefault(flowid, Serial16())
                if inbound and packet['data_seq'] is not None:
                    packet['data_extended'] = data_space.assign(packet['data_seq'])
                    result['anchors'].setdefault(flowid, []).append([timestamp, data_space.highest])
                    if packet['channel_seq'] is not None:
                        packet['channel_extended'] = channel_space.assign(packet['channel_seq'])
                        if packet['channel_seq'] == channel and in_window:
                            result['target'].append(packet)
                packet['data_anchor'] = data_space.highest
                if inbound and packet['aoa'] is not None and in_window:
                    result['aoa'].append(packet)
                if outbound and (packet['ack'] or packet['ackvec']) and in_window:
                    result['feedback'].append(packet)
                result['serial_ambiguous'] |= data_space.ambiguous or channel_space.ambiguous
            except (DecodeError, ValueError, struct.error) as error:
                result['error_count'] += 1
                if len(result['errors']) < 100:
                    result['errors'].append(dict(packet=index, error=str(error)))
                if len(record) < 16 or stream.tell() >= Path(path).stat().st_size:
                    break
    result['serial_wraps'] = {flow: dict(data=value.wraps, channel=channel_spaces[flow].wraps)
                              for flow, value in spaces.items()}
    for name in ('target','feedback','aoa'):
        result[name].sort(key=lambda packet:packet['time_ns'])
    for values in result['anchors'].values():
        values.sort()
    return result


def trace_rows(path):
    with Path(path).open() as stream:
        for number, line in enumerate(stream, 1):
            try:
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise DecodeError('trace row is not an object')
                yield number, row
            except (ValueError, TypeError) as error:
                yield number, {'kind': 'parse_error', 'error': str(error)}


def trace_summary(path):
    meta, footer, errors, counts = {}, {}, [], Counter()
    metadata_n = footer_n = count = 0
    first = last = first_id = last_id = None
    recent_deliveries = deque(maxlen=32)
    failure = final = arm_event = None
    active_gap=None
    longest_gaps=[]
    max_delivery_interval=None
    previous_delivery=None
    def close_gap(end, recovered):
        nonlocal active_gap
        if active_gap is None: return
        active_gap.update(end_ns=end,duration_ms=(end-active_gap['start_ns'])/1e6,recovered=recovered)
        longest_gaps.append(active_gap)
        longest_gaps.sort(key=lambda gap:gap['duration_ms'],reverse=True)
        del longest_gaps[20:]
        active_gap=None
    channel_space = Serial16()
    for line, row in trace_rows(path):
        kind = row.get('kind')
        if kind == 'metadata':
            metadata_n += 1; meta = row
        elif kind == 'footer':
            footer_n += 1; footer = row
        elif kind == 'event':
            count += 1; counts[row.get('event')] += 1
            timestamp, ident = row.get('time_ns'), row.get('id')
            valid = integer(timestamp) and integer(ident) and row.get('event') in EVENTS
            valid &= all(integer(row.get(name), 65535) for name in ('data_seq','channel_seq','status','base','highest','next_channel','flags'))
            valid &= all(integer(row.get(name)) for name in ('detail','length','buffered','peer_age_ms','gap_age_ms'))
            if not valid or last is not None and (timestamp < last or ident != last_id + 1):
                if len(errors) < 100: errors.append(dict(line=line, error='invalid trace event/sequence/timestamp'))
                continue
            if first is None: first, first_id = timestamp, ident
            last, last_id, final = timestamp, ident, row
            row['channel_epoch'] = channel_space.assign(row['next_channel'])
            if row['event'] == 'arm' and row['id'] == 0:
                arm_event = row
            if row['event'] == 'deliver' and row['status'] == 0:
                recent_deliveries.append(row)
                if previous_delivery is not None:
                    delta=timestamp-previous_delivery['time_ns']
                    if max_delivery_interval is None or delta>max_delivery_interval['duration_ns']:
                        max_delivery_interval=dict(duration_ns=delta,start=previous_delivery,end=row)
                previous_delivery=row
            if row['event'] == 'failure': failure = row
            if active_gap is not None and (row['buffered']==0 or row['next_channel']!=active_gap['channel']):
                close_gap(timestamp,True)
            if row['buffered']>0:
                if active_gap is None:
                    active_gap=dict(channel=row['next_channel'],start_ns=timestamp,first_event_id=ident,
                                    max_gap_age_ms=row['gap_age_ms'],max_buffered=row['buffered'],
                                    last_delivery_before=previous_delivery,started_before_arm=row['event']=='arm')
                active_gap.update(max_gap_age_ms=max(active_gap['max_gap_age_ms'],row['gap_age_ms']),
                                  max_buffered=max(active_gap['max_buffered'],row['buffered']),
                                  peer_age_ms_at_end=row['peer_age_ms'])
        else:
            if len(errors) < 100: errors.append(dict(line=line, error=row.get('error','unknown trace row kind')))
    if active_gap is not None and last is not None: close_gap(last,False)
    if metadata_n != 1 or footer_n != 1 or meta.get('schema') != 'freerdp-pr13608-recovery-v1' or meta.get('event_bytes') != 96:
        errors.append(dict(error='missing/unsupported trace metadata/footer'))
    if (not integer(meta.get('total_events')) or not integer(meta.get('overwritten_events')) or
            meta.get('total_events',0)-meta.get('overwritten_events',0) != count or
            first_id != meta.get('overwritten_events') or last_id != meta.get('total_events',0)-1 or
            footer.get('retained_sequence_valid') is not True or footer.get('first_event_ns') != first or footer.get('last_event_ns') != last):
        errors.append(dict(error='trace retained range/footer mismatch'))
    if (footer.get('overwritten_events') != meta.get('overwritten_events') or
            footer.get('complete_from_arm') != meta.get('complete_from_arm')):
        errors.append(dict(error='trace completeness metadata/footer mismatch'))
    try:
        bitmap = bytes.fromhex(meta.get('initial_received_phys_hex',''))
        if len(bitmap) != 4096 or not isinstance(meta.get('initial_channels'),list) or any(
                not isinstance(v,list) or len(v)!=2 or not integer(v[0],65535) or not integer(v[1],0xffffffff)
                for v in meta['initial_channels']):
            raise DecodeError('invalid initial receipt/channel snapshot')
    except (TypeError,ValueError) as error:
        errors.append(dict(error=str(error)))
    alignment = None
    before, after, unix = (meta.get(key) for key in ('arm_before_ns','arm_after_ns','arm_unix_ns'))
    if all(integer(x) for x in (before,after,unix)) and after >= before and unix > 0:
        alignment = dict(offset_min_ns=unix-after, offset_max_ns=unix-before,
                         bracket_ns=after-before, wallclock_api=meta.get('wallclock_api'))
    else:
        errors.append(dict(error='invalid trace wallclock arm bracket'))
    complete = bool(not errors and meta.get('complete_from_arm') is True and footer.get('complete_from_arm') is True
                    and meta.get('overwritten_events') == 0 and meta.get('dropped_events') == 0)
    return dict(path=str(Path(path).resolve()), sha256=sha256(path), metadata=meta, footer=footer,
                errors=errors, retained_valid=not errors, complete_from_arm=complete,
                event_count=count, event_types=dict(counts), first_event_ns=first, last_event_ns=last,
                failure=failure, final_event=final, last_deliveries=list(recent_deliveries),
                arm_event=arm_event,
                longest_buffered_gaps=longest_gaps,max_delivery_interval=max_delivery_interval,
                channel_serial_ambiguous=channel_space.ambiguous, alignment=alignment)


def wall_time(event, summary):
    alignment = summary.get('alignment')
    return event['time_ns'] + (alignment['offset_min_ns']+alignment['offset_max_ns'])//2 if alignment else None


def trace_details(path, summary, channel, sequences, limit=20000, *, window_ns=None):
    rx, aoa, headers, collisions, deliveries, outgoing = [], [], [], [], [], []
    errors = []
    pending = None
    selected_count = 0
    first_hole = None
    failure = summary['failure'] or summary['final_event']
    scope_start = max(summary['first_event_ns'] or 0,
                      failure['time_ns']-failure['gap_age_ms']*1_000_000-10_000_000_000) if failure else 0
    if window_ns is not None: scope_start=window_ns[0]
    def retain(values, value):
        nonlocal selected_count
        selected_count += 1
        if len(values) < limit: values.append(value)
    def finish():
        nonlocal pending
        if not pending: return
        parent, data = pending
        pending = None
        try:
            packet = decode_udp2(bytes(data), parent['length'], protected=False)
            if packet['flags'] != parent['flags']:
                raise DecodeError('trace/header flags differ')
            if packet['ack'] or packet['ackvec']:
                # Negative vectors and below-base references are essential
                # evidence for a still-missing alias and legitimate retirement.
                retain(headers, dict(event_id=parent['id'], time_ns=parent['time_ns'],
                                     time_unix_ns=wall_time(parent,summary), direction=parent['event'],
                                     status=parent['status'], send_success=parent['event']=='tx_header' and parent['status']==0,
                                     packet=packet))
        except (ValueError, TypeError) as error:
            if len(errors)<100: errors.append(dict(event_id=parent['id'], error=str(error)))
    for _, event in trace_rows(path):
        if event.get('kind') != 'event' or not integer(event.get('time_ns')): continue
        if (event.get('event') not in EVENTS or not integer(event.get('id')) or
                not all(integer(event.get(name),65535) for name in ('data_seq','channel_seq','status','base','highest','next_channel','flags')) or
                not all(integer(event.get(name)) for name in ('detail','length','buffered','peer_age_ms','gap_age_ms'))):
            if len(errors)<100: errors.append(dict(event_id=event.get('id'),error='invalid selected trace event'))
            continue
        if event['time_ns'] < scope_start: continue
        if window_ns is not None and event['time_ns']>window_ns[1]:
            finish(); break
        kind = event.get('event')
        if kind != 'header_continuation': finish()
        if kind in ('tx_header','rx_header'):
            try: pending = (event, bytearray.fromhex(event.get('header_fragment_hex','')))
            except ValueError: errors.append(dict(event_id=event.get('id'),error='invalid header hex'))
        elif kind == 'header_continuation':
            try:
                if not pending or event['detail'] != pending[0]['id'] or event['status'] != len(pending[1]):
                    raise DecodeError('orphan/overlapping header continuation')
                pending[1].extend(bytes.fromhex(event['header_fragment_hex']))
            except (ValueError,KeyError) as error:
                if len(errors)<100: errors.append(dict(event_id=event.get('id'),error=str(error)))
                pending = None
        if event.get('next_channel') == channel and event.get('buffered',0)>0 and first_hole is None:
            first_hole = event
        if kind == 'rx_data' and event.get('channel_seq') == channel:
            retain(rx,event)
        elif kind in ('rx_aoa','rx_resync'):
            if not aoa or (kind,event.get('status'),event.get('detail')) != (aoa[-1]['event'],aoa[-1]['status'],aoa[-1]['detail']):
                retain(aoa,event)
        elif kind == 'deliver' and event.get('channel_seq') == channel:
            retain(deliveries,event)
        elif kind == 'store_collision' and channel in (event.get('data_seq'),event.get('channel_seq')):
            retain(collisions,event)
        elif kind in ('retry','retire'):
            retain(outgoing,event)
    finish()
    return dict(scope_start_ns=scope_start, rx=rx, aoa=aoa, feedback_headers=headers,
                deliveries=deliveries, collisions=collisions, client_outbound_retry_retire=outgoing,
                first_observed_buffered_hole=first_hole, header_errors=errors,
                selected_record_count=selected_count, rows_truncated=any(len(x)>=limit for x in
                    (rx,aoa,headers,deliveries,collisions,outgoing)))


def capture_quality(capture, health):
    health = health if isinstance(health,dict) else {}
    drops = health.get('kernel_drops')
    clean = (health.get('complete') is True and integer(drops) and drops==0 and
             health.get('capture_limit_reached') is not True and health.get('wire_evidence_valid') is not False)
    return dict(complete=health.get('complete') is True,kernel_drops=drops if integer(drops) else None,
                header_error_count=capture['error_count'],serial_ambiguous=capture['serial_ambiguous'],
                absence_qualified=clean and not capture['error_count'] and not capture['serial_ambiguous'],
                body_snap_truncations=capture['body_snap_truncations'])


def initial_channel_present(meta, channel):
    values=meta.get('initial_channels')
    return bool(isinstance(values,list) and any(isinstance(v,list) and len(v)==2 and v[0]==channel for v in values))


def initial_receipt_possible(trace, sequence):
    arm=trace.get('arm_event')
    if not arm or not arm['status']&1:
        return False
    span=(arm['highest']-arm['base'])&65535
    if span>=32768 or ((sequence-arm['base'])&65535)>span:
        return False
    try:
        bitmap=bytes.fromhex(trace['metadata'].get('initial_received_phys_hex',''))
        index=sequence%32768
        return len(bitmap)==4096 and bool(bitmap[index//8]&(1<<(index%8)))
    except (ValueError,TypeError):
        return False


def associate_flow(offered, details, slop_ns):
    """Bind this native object using emitted, visible header fingerprints only."""
    indexed=defaultdict(list)
    for packet in offered['feedback']:
        indexed[packet['header_sha256']].append(packet)
    counts=Counter()
    ambiguous=0
    examples=[]
    for header in details['feedback_headers']:
        if not header['send_success'] or header['time_unix_ns'] is None:
            continue
        matches=[p for p in indexed[header['packet']['header_sha256']] if
                 abs(p['time_ns']-header['time_unix_ns'])<=slop_ns]
        flows={p['flow'] for p in matches}
        if len(flows)==1:
            flow=next(iter(flows));counts[flow]+=1
            if len(examples)<20:examples.append(dict(event_id=header['event_id'],flow=flow,
                                                     captured_packets=[p['packet_index'] for p in matches]))
        elif len(flows)>1:
            ambiguous+=1
    flow=next(iter(counts)) if len(counts)==1 else None
    required=1 if len(offered['flows'])==1 else 3
    verified=flow is not None and counts[flow]>=required
    return dict(verified=verified,flow=flow if verified else None,
                unique_header_matches=dict(counts),ambiguous_header_matches=ambiguous,
                required_unique_matches=required,examples=examples,
                method='successful native TX ACK/header fingerprint + arm wallclock envelope',
                scope='one native UDP object; distinct tunnel flows are never pooled')


def analyze(offered, received, trace, details, channel, health=None, *, match_ms=5000, clock_slop_ms=20):
    health = health if isinstance(health,dict) else {}
    quality = {name:capture_quality(cap,health.get(name)) for name,cap in (('offered',offered),('received',received))}
    flows = set(offered['flows'])|set(received['flows'])
    flow_unambiguous = len(flows)==1
    target_epochs = {p.get('channel_extended') for p in offered['target']}
    serial_ambiguous = (offered['serial_ambiguous'] or received['serial_ambiguous'] or len(target_epochs)>1 or
                        trace['channel_serial_ambiguous'] or any(e['event']=='rx_resync' for e in details['aoa']))
    complete_native = trace['complete_from_arm'] and not details['header_errors'] and not details['rows_truncated']
    alignment = trace.get('alignment')
    slop = int(clock_slop_ms*1_000_000) + (alignment['bracket_ns']//2 if alignment else 0)
    association=associate_flow(offered,details,slop)
    selected_flow=association['flow']
    flow_unambiguous=association['verified']
    anchor_times={flow:[v[0] for v in values] for flow,values in offered['anchors'].items()}
    def anchor_at(flow,timestamp):
        values=offered['anchors'].get(flow,[])
        index=bisect.bisect_right(anchor_times.get(flow,[]),timestamp+slop)-1
        return values[index][1] if index>=0 else None
    attempts = []
    selected_offers=[p for p in offered['target'] if selected_flow is None or p['flow']==selected_flow]
    for packet in selected_offers:
        timestamp = packet['time_ns']
        matches = [p for p in received['target'] if p['flow']==packet['flow'] and
                   p['header_sha256']==packet['header_sha256'] and abs(p['time_ns']-timestamp)<=match_ms*1_000_000]
        later_same_label = [p for p in selected_offers if p is not packet and p['flow']==packet['flow'] and p['data_seq']==packet['data_seq']]
        ambiguous = serial_ambiguous or bool(later_same_label) or len(matches)>1
        rx = [e for e in details['rx'] if e['data_seq']==packet['data_seq'] and
              wall_time(e,trace) is not None and timestamp-slop<=wall_time(e,trace) and
              near16(e['data_seq'],anchor_at(packet['flow'],wall_time(e,trace)))==packet.get('data_extended')]
        positive = [e for e in rx if e['status'] in (1,6)]
        rejected = [e for e in rx if e['status'] in (3,4,5)]
        feedback = []
        for header in details['feedback_headers']:
            if not header['send_success'] or header['time_unix_ns'] is None or header['time_unix_ns']<timestamp-slop:
                continue
            ack = header['packet']
            receipt_positive=covers(ack,packet['data_seq'])
            state=vector_state(ack['ackvec'],packet['data_seq']) if ack.get('ackvec') else None
            below=bool(ack.get('ackvec') and 0<((ack['ackvec']['base']-packet['data_seq'])&65535)<32768)
            if not receipt_positive and state is None and not below: continue
            ack_base=(ack.get('ack') or ack['ackvec'])['base']
            ack_extended=near16(ack_base,anchor_at(packet['flow'],header['time_unix_ns']))
            data_extended=packet.get('data_extended')
            epoch_verified=bool(ack_extended is not None and data_extended is not None and
                (0<=ack_extended-data_extended<32768 if ack.get('ack') or below else
                 ack_extended+((packet['data_seq']-ack_base)&65535)==data_extended))
            retired = [e for e in details['aoa'] if e['event']=='rx_aoa' and e['status']==0 and
                       header['time_ns']>=e['time_ns'] and wall_time(e,trace)>=timestamp-slop and
                       0<((e['detail']-packet['data_seq'])&65535)<32768 and
                       near16(e['detail'],anchor_at(packet['flow'],wall_time(e,trace))) is not None and
                       data_extended is not None and
                       0<near16(e['detail'],anchor_at(packet['flow'],wall_time(e,trace)))-data_extended<32768]
            captured = [a for a in offered['feedback'] if a['flow']==packet['flow'] and
                        a['header_sha256']==ack['header_sha256'] and abs(a['time_ns']-header['time_unix_ns'])<=slop]
            before = [e for e in positive if e['time_ns']<=header['time_ns']]
            explicit_rejection = [e for e in rejected if e['time_ns']<=header['time_ns']]
            delivered = [e for e in details['deliveries'] if e['status']==0 and e['time_ns']<=header['time_ns']]
            initial_channel = initial_channel_present(trace['metadata'],channel)
            initial_receipt = initial_receipt_possible(trace,packet['data_seq'])
            scope_covers = trace['alignment'] is not None and timestamp>=trace['metadata']['arm_unix_ns']+slop
            suspect = bool(receipt_positive and explicit_rejection and not before and not delivered and not initial_channel and not initial_receipt and not retired and complete_native and
                           flow_unambiguous and not ambiguous and scope_covers and epoch_verified and all(q['absence_qualified'] for q in quality.values()))
            feedback.append(dict(native_event_id=header['event_id'], time_unix_ns=header['time_unix_ns'],
                                 kind='cumulative' if ack.get('ack') else 'explicit_vector_positive' if state is True else
                                      'explicit_vector_negative' if state is False else 'below_vector_base_nonpositive',
                                 positive_receipt=receipt_positive,
                                 base=(ack.get('ack') or ack['ackvec'])['base'],
                                 base_extended=ack_extended,sequence_epoch_verified=epoch_verified,
                                 native_received_before_ack=[e['id'] for e in before],
                                 native_rejected_before_ack=[e['id'] for e in explicit_rejection],
                                 native_channel_delivered_before_ack=[e['id'] for e in delivered],
                                 initial_channel_retained=initial_channel,
                                 initial_data_receipt_possible=initial_receipt,
                                 peer_aoa_retirement_events=[e['id'] for e in retired],
                                 wire_capture_matches=[p['packet_index'] for p in captured],
                                 qualified_unretained_ack=suspect))
        bounded = (received['first_time_ns'] is not None and received['first_time_ns']<=timestamp and
                   received['last_time_ns']>=timestamp+match_ms*1_000_000 and
                   (received.get('retention_window_ns') is None or
                    received['retention_window_ns'][1]>=timestamp+match_ms*1_000_000))
        absence = not matches and quality['offered']['absence_qualified'] and quality['received']['absence_qualified'] and bounded and flow_unambiguous and not ambiguous
        attempts.append(dict(offered_packet=packet['packet_index'], offered_time_ns=timestamp,
                             flow=packet['flow'],
                             data_seq=packet['data_seq'], data_extended=packet.get('data_extended'),
                             channel_seq=channel, channel_extended=packet.get('channel_extended'),
                             received_packets=[p['packet_index'] for p in matches],
                             received_match_ambiguous=len(matches)>1, serial_ambiguous=ambiguous,
                             no_postloss_match_qualified=absence,
                             native_rx=[dict(e, decision=RX_STATUS.get(e['status'],'unknown'),
                                        before=dict(base=e['detail']&65535,next_channel=(e['detail']>>16)&65535,
                                                    highest=(e['detail']>>32)&65535)) for e in rx],
                             feedback=feedback))
    failure = trace['failure']
    last_delivery = trace['last_deliveries'][-1] if trace['last_deliveries'] else None
    watchdog = bool(failure and failure['buffered']>0 and failure['gap_age_ms']>16000 and
                    'stream is stuck' in trace['footer'].get('failure',''))
    suspect_n = sum(f['qualified_unretained_ack'] for a in attempts for f in a['feedback'])
    recovered=bool(details['deliveries'] and details['first_observed_buffered_hole'])
    longest=trace['longest_buffered_gaps'][0] if trace['longest_buffered_gaps'] else None
    gap_end_unix=wall_time(dict(time_ns=longest['end_ns']),trace) if longest and alignment else None
    classification = ('qualified_ack_of_rejected_unretained_alias' if suspect_n else
                      'watchdog_fired_with_recent_peer_activity' if watchdog and failure['peer_age_ms']<16000 else
                      'watchdog_fired_after_stream_gap' if watchdog else
                      'observed_channel_recovery' if recovered else 'diagnostic_inconclusive')
    return dict(schema='freerdp.pr13608.recovery_audit',version=1,performance_eligible=False,
                scope='visible UDP2 headers and native recovery state; no TLS/ciphertext decoding',
                channel=channel,flow_unambiguous=flow_unambiguous,flows=sorted(flows),
                native_flow_association=association,
                capture_quality=quality,native_trace=dict(retained_valid=trace['retained_valid'],
                    complete_from_arm=trace['complete_from_arm'],overwritten_events=trace['metadata'].get('overwritten_events'),
                    errors=trace['errors'],header_errors=details['header_errors'],rows_truncated=details['rows_truncated'],
                    alignment=alignment,clock_slop_ms=clock_slop_ms),
                timeline=dict(failure=failure,failure_reason=trace['footer'].get('failure'),
                    last_delivery=last_delivery,first_observed_buffered_hole=details['first_observed_buffered_hole'],
                    longest_buffered_gaps=trace['longest_buffered_gaps'],max_delivery_interval=trace['max_delivery_interval'],
                    gap_end_unix_ns=gap_end_unix,
                    gap_end_within_received_capture=bool(gap_end_unix is not None and received['last_time_ns'] is not None and
                                                         gap_end_unix<=received['last_time_ns']+slop),
                    gap_progress_estimate_ns=failure['time_ns']-failure['gap_age_ms']*1_000_000 if failure else None,
                    peer_last_activity_estimate_ns=failure['time_ns']-failure['peer_age_ms']*1_000_000 if failure else None),
                aliases=attempts,native_channel_deliveries=details['deliveries'],
                native_rx_for_channel=details['rx'],
                native_slot_collisions=details['collisions'],peer_aoa_events=details['aoa'],
                initial_channel_present=initial_channel_present(trace['metadata'],channel),
                client_outbound_retry_retire=details['client_outbound_retry_retire'],
                conclusion=dict(classification=classification,qualified_unretained_ack_count=suspect_n,
                    offered_aliases=len(attempts),postloss_alias_observations=sum(bool(a['received_packets']) for a in attempts),
                    watchdog=watchdog,peer_age_ms=failure['peer_age_ms'] if failure else None,
                    gap_age_ms=failure['gap_age_ms'] if failure else None),
                observed_inter_alias_offer_ms=[(b['offered_time_ns']-a['offered_time_ns'])/1e6 for a,b in zip(attempts,attempts[1:])],
                notes=['Native retry/retire concern client outbound chunks, not Windows aliases.',
                    'ACKVEC below-base labels are not explicit positives; AoA retirement is separate from reception.',
                    'Peer AoA retires an old DataSeq attempt, not the ChannelSeq stream chunk; another alias may still be due.',
                    'Absence through a 16-second stream watchdog does not prove Windows abandoned a chunk; sender retry timers are not observable here.',
                    'Nearest half-space epochs are conservative; resync, wide reverse jumps, duplicate labels and overwritten native prefixes restrict verdicts.',
                    'Epoch assignment treats reverse jumps beyond4096 as ambiguous; this is an analysis safeguard, not the protocol receive-window size.',
                    'Wallclock association uses the arm API bracket plus the declared clock-slop envelope; independent drift/clock-step validation is still required.',
                    'Complete visible headers remain usable with encrypted-body-only snap truncation.',
                    'A qualified no-postloss-match is bounded capture absence, not proof of a specific netem random-drop decision.',
                    'Wire ACK observations prove emission/observation, not Windows acceptance or application rendering.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trial',type=Path,required=True)
    parser.add_argument('--vm-ip',required=True)
    parser.add_argument('--client-port',type=int)
    parser.add_argument('--channel',type=lambda x:int(x,0))
    parser.add_argument('--match-ms',type=float,default=5000)
    parser.add_argument('--clock-slop-ms',type=float,default=20)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    ipaddress.IPv4Address(args.vm_ip)
    paths={name:args.trial/name for name in ('offered.pcap','received.pcap','recovery-trace.jsonl','capture-health.json')}
    targets=[args.output.with_suffix(s).resolve() for s in ('.json','.md')]
    if any(target in [p.resolve() for p in paths.values()] or any((parent/'SHA256SUMS.json').exists() for parent in target.parents)
           for target in targets): parser.error('output must not overwrite inputs or enter sealed artifact folders')
    if args.client_port is not None and not 1<=args.client_port<=65535: parser.error('invalid client port')
    if not all(math.isfinite(x) and x>0 for x in (args.match_ms,args.clock_slop_ms)): parser.error('timing bounds must be positive finite numbers')
    trace=trace_summary(paths['recovery-trace.jsonl'])
    final=trace['failure'] or trace['final_event']
    gaps=trace['longest_buffered_gaps']
    focus=next((gap for gap in gaps if gap['channel']==args.channel),None) if args.channel is not None else gaps[0] if gaps else None
    channel=args.channel if args.channel is not None else (trace['failure']['next_channel'] if trace['failure'] else focus['channel'] if focus else final['next_channel'] if final else None)
    if not integer(channel,65535): parser.error('cannot select a valid channel')
    if trace['failure']:
        focus=next((gap for gap in gaps if gap['channel']==channel and not gap['recovered']),focus)
    window_native=(max(trace['first_event_ns'] or 0,focus['start_ns']-5_000_000_000),focus['end_ns']+5_000_000_000) if focus else None
    window_wall=None
    if window_native is not None and trace['alignment']:
        window_wall=(window_native[0]+trace['alignment']['offset_min_ns']-int(args.clock_slop_ms*1_000_000),
                     window_native[1]+trace['alignment']['offset_max_ns']+int(args.clock_slop_ms*1_000_000))
    offered=read_pcap(paths['offered.pcap'],args.vm_ip,channel,client_port=args.client_port,window_ns=window_wall)
    received=read_pcap(paths['received.pcap'],args.vm_ip,channel,client_port=args.client_port,window_ns=window_wall)
    details=trace_details(paths['recovery-trace.jsonl'],trace,channel,{p['data_seq'] for p in offered['target']},window_ns=window_native)
    health=json.loads(paths['capture-health.json'].read_text()) if paths['capture-health.json'].is_file() else {}
    report=analyze(offered,received,trace,details,channel,health,match_ms=args.match_ms,clock_slop_ms=args.clock_slop_ms)
    report['focused_gap']=focus
    report['focused_window_native_ns']=window_native
    report['inputs']={name:dict(path=str(path.resolve()),sha256=sha256(path)) for name,path in paths.items() if path.is_file()}
    report['captures']={name:{key:cap[key] for key in ('record_count','parsed_udp2','syn_envelopes','snaplen','linktype','error_count','errors','serial_wraps','first_time_ns','last_time_ns')}
                        for name,cap in (('offered',offered),('received',received))}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    lines=['# PR13608 missing-channel recovery audit','',f"Channel `{channel:04x}`: {report['conclusion']['classification']}.",
           '',f"Offered aliases: {len(report['aliases'])}; postloss alias observations: {report['conclusion']['postloss_alias_observations']}.",
           '', '| DataSeq | Postloss matches | Native RX statuses | Emitted ACK claims | Qualified unretained claims |',
           '|---|---:|---|---:|---:|']
    lines += [f"| `{a['data_seq']:04x}` | {len(a['received_packets'])} | {','.join(str(e['status']) for e in a['native_rx'])} | {len(a['feedback'])} | {sum(f['qualified_unretained_ack'] for f in a['feedback'])} |" for a in report['aliases']]
    lines += ['', 'Capture/native qualifications: '+json.dumps(report['capture_quality'])+'; '+json.dumps(report['native_trace']), '', *['- '+n for n in report['notes']]]
    args.output.with_suffix('.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps(report['conclusion']))
    return 1 if offered['error_count'] or received['error_count'] or not trace['retained_valid'] or details['header_errors'] else 0


if __name__=='__main__':
    raise SystemExit(main())
