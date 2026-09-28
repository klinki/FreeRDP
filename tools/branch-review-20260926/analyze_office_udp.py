#!/usr/bin/env python3
"""Replay saved RDPEUDP2 traffic and authenticate its reconstructed TLS stream.

Requires a matching native library, tshark, and OpenSSL. Packet caches contain
private encrypted traffic; reports contain only sequence metadata and hashes.
Plaintext and TLS secrets are never written or printed. No network I/O.
"""
import argparse
import ctypes as C
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys

HEADER = struct.Struct('<dIiiH')


def load_extractor():
    path = Path(__file__).resolve().parents[1] / 'graphics-replay' / 'extract.py'
    spec = importlib.util.spec_from_file_location('graphics_extract', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def unwrap(value, reference):
    candidate = (reference & ~65535) | value
    if candidate < reference - 32768:
        candidate += 65536
    elif candidate > reference + 32768:
        candidate -= 65536
    return candidate


def packets(path):
    with path.open('rb') as source:
        while header := source.read(HEADER.size):
            if len(header) != HEADER.size:
                raise ValueError('truncated packet cache header')
            timestamp, frame, dseq, cseq, length = HEADER.unpack(header)
            raw = source.read(length)
            if len(raw) != length:
                raise ValueError('truncated packet cache body')
            yield timestamp, frame, dseq, cseq, raw


def prepare(args, extract):
    hellos = dict((int(flow), random.replace(':', '').lower()) for flow, random in
        extract.tshark_rows(args.capture, 'udp && tls.handshake.type == 1',
                           ['udp.stream', 'tls.handshake.random']))
    (args.output / 'hellos.json').write_text(json.dumps(hellos))
    fields = ['udp.stream', 'frame.time_epoch', 'frame.number',
              'rdpudp.data.seqnum', 'rdpudp.data.channelseqnumber', 'udp.payload']
    handles = {}
    try:
        for flow, ts, frame, dseq, cseq, raw in extract.tshark_rows(
                args.capture, 'udp.srcport == 3389 && rdpudp.packetType', fields):
            flow = int(flow)
            if flow not in handles:
                handles[flow] = (args.output / f'flow-{flow}.packets').open('wb')
            raw = bytes.fromhex(raw.replace(':', ''))
            handles[flow].write(HEADER.pack(float(ts), int(frame),
                int(dseq, 0) if dseq else -1, int(cseq, 0) if cseq else -1, len(raw)))
            handles[flow].write(raw)
    finally:
        for handle in handles.values():
            handle.close()


def native_library(path):
    lib = C.CDLL(str(path.resolve()))
    boolean = C.c_bool if sys.platform == 'darwin' else C.c_int32

    class State(C.Structure):
        _fields_ = [('recvDataBase', C.c_uint16), ('lastAckSent', C.c_uint16),
                    ('expectedChannelSeq', C.c_uint16), ('haveRecvData', boolean),
                    ('haveSeenAoa', boolean), ('haveRealData', boolean),
                    ('recvStreamLen', C.c_size_t)]

    lib.rdpeudp_test_new.restype = C.c_void_p
    lib.rdpeudp_test_free.argtypes = [C.c_void_p]
    lib.rdpeudp_test_set_time.argtypes = [C.c_void_p, C.c_uint64]
    lib.rdpeudp_test_feed.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t]
    lib.rdpeudp_test_feed.restype = boolean
    lib.rdpeudp_test_recv_state.argtypes = [C.c_void_p, C.POINTER(State)]
    lib.rdpeudp_test_recv_state.restype = boolean
    lib.rdpeudp_test_check_health.argtypes = [C.c_void_p]
    lib.rdpeudp_test_check_health.restype = boolean
    lib.rdpeudp_test_recv_data.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]
    lib.rdpeudp_test_recv_data.restype = boolean
    return lib, State


def analyze(path, lib, state_type, extract, secrets, crypto):
    ctx = lib.rdpeudp_test_new()
    if not ctx:
        raise RuntimeError('native fixture allocation failed')
    state = state_type()
    result = {'flow': int(path.stem.split('-')[1]), 'packets': 0, 'wraps': [],
              'channel_zero_packets': 0, 'dataseq_zero_packets': 0}
    rows, metadata, data_sequences = [], {}, set()
    highest_channel, highest_data, old_length = 0, 0, 0
    try:
        for timestamp, frame, dseq, cseq, raw in packets(path):
            result['packets'] += 1
            at = {'frame': frame, 'time_utc': datetime.fromtimestamp(timestamp, timezone.utc).isoformat(),
                  'data_sequence': dseq, 'channel_sequence': cseq}
            if dseq >= 0:
                full_data = unwrap(dseq, highest_data)
                highest_data = max(highest_data, full_data)
                data_sequences.add(full_data)
                result['dataseq_zero_packets'] += dseq == 0
            if cseq >= 0:
                sequence, body = extract.udp_chunk(raw)
                if sequence != cseq:
                    raise ValueError('parser disagrees with tshark')
                full_channel = unwrap(cseq, highest_channel)
                if full_channel // 65536 > highest_channel // 65536:
                    result['wraps'].append(at | {'last_channel_frame': metadata.get(65535, {}).get('frame')})
                highest_channel = max(highest_channel, full_channel)
                metadata[cseq] = at
                result['channel_zero_packets'] += cseq == 0
                rows.append((round(timestamp * 1_000_000), cseq, body))
            lib.rdpeudp_test_set_time(ctx, round(timestamp * 1000))
            if not lib.rdpeudp_test_feed(ctx, raw, len(raw)):
                raise ValueError('native feed failed')
            if not lib.rdpeudp_test_recv_state(ctx, C.byref(state)):
                raise ValueError('native snapshot failed')
            if result['wraps']:
                wrap = result['wraps'][-1]
                if wrap['frame'] == frame:
                    wrap['native_expected_after'] = state.expectedChannelSeq
                if 'native_recovery_frame' not in wrap and 0 < state.expectedChannelSeq < 32768:
                    wrap['native_recovery_frame'] = frame
                    start = datetime.fromisoformat(wrap['time_utc']).timestamp()
                    wrap['native_recovery_delay_ms'] = round((timestamp - start) * 1000, 3)
            if state.recvStreamLen != old_length:
                result['native_last_progress'] = at
                old_length = state.recvStreamLen
            if not lib.rdpeudp_test_check_health(ctx) and 'native_first_failure' not in result:
                result['native_first_failure'] = at | {'expected_channel': state.expectedChannelSeq}
        result['native_bytes'] = state.recvStreamLen
        result['native_expected_channel'] = state.expectedChannelSeq
        result['native_dataseq_base'] = state.recvDataBase
        digest = hashlib.sha256()
        for offset in range(0, state.recvStreamLen, 1024 * 1024):
            size = min(1024 * 1024, state.recvStreamLen - offset)
            buffer = C.create_string_buffer(size)
            if not lib.rdpeudp_test_recv_data(ctx, offset, buffer, size):
                raise ValueError('native stream readback failed')
            digest.update(buffer.raw)
        result['native_sha256'] = digest.hexdigest()
    finally:
        lib.rdpeudp_test_free(ctx)
    if data_sequences:
        missing = [i for i in range(min(data_sequences), max(data_sequences) + 1)
                   if i not in data_sequences]
        result['missing_dataseq_count'] = len(missing)
        result['missing_dataseq_sample'] = missing[:25]
        result['last_missing_dataseq_sample'] = missing[-12:]
    if rows:
        try:
            stream, ends, times, stats = extract.reassemble(rows)
            result['reassembly'] = stats
            result['reassembled_bytes'] = len(stream)
            result['reassembled_sha256'] = hashlib.sha256(stream).hexdigest()
            result['native_matches_reassembled'] = result['native_sha256'] == result['reassembled_sha256']
            result['tls'] = tls_stats = {}
            for timestamp, plaintext in extract.tls_plaintexts(stream, ends, times, secrets, crypto, tls_stats):
                result['last_authenticated_application_time_utc'] = datetime.fromtimestamp(
                    timestamp / 1_000_000, timezone.utc).isoformat()
            result['tls_authenticated_to_end'] = True
        except ValueError as error:
            result['authentication_or_reassembly_error'] = str(error)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse-packets', action='store_true')
    parser.add_argument('--libcrypto')
    parser.add_argument('--expect-wraps', type=int,
                        help='Fail unless this many wraps recover with authenticated matching bytes')
    args = parser.parse_args()
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=True)
    extract = load_extractor()
    if not args.reuse_packets:
        prepare(args, extract)
    hellos = json.loads((args.output / 'hellos.json').read_text())
    all_secrets = {}
    for line in args.keys.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and not fields[0].startswith('#'):
            all_secrets.setdefault(fields[1].lower(), {})[fields[0]] = bytes.fromhex(fields[2])
    lib, state_type = native_library(args.library)
    crypto = extract.GCM(args.libcrypto)
    results = []
    for path in sorted(args.output.glob('flow-*.packets'), key=lambda p: int(p.stem.split('-')[1])):
        flow = path.stem.split('-')[1]
        result = analyze(path, lib, state_type, extract,
                         all_secrets.get(hellos.get(flow), {}), crypto)
        results.append(result)
        (args.output / 'report.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(result), flush=True)
    if args.expect_wraps is not None:
        wrapped = [r for r in results if r['wraps']]
        if sum(len(r['wraps']) for r in wrapped) != args.expect_wraps:
            raise ValueError('unexpected captured wrap count')
        if not all(r.get('tls_authenticated_to_end') and r.get('native_matches_reassembled')
                   and 'native_first_failure' not in r for r in wrapped):
            raise ValueError('captured wrap failed native recovery or TLS authentication')
        print(f'PASS: all {args.expect_wraps} wraps match authenticated streams')


if __name__ == '__main__':
    main()
