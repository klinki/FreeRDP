#!/usr/bin/env python3
"""Extract authenticated RDPEUDP2/TLS1.3 graphics messages from a saved pcap.

Offline only. Requires tshark and OpenSSL libcrypto, not a Python crypto package.
Supports one captured UDP tunnel, AES-GCM, and uncompressed DVC framing.
Output contains private desktop contents; keys and plaintext are never logged.
"""
import argparse
import bisect
import ctypes as C
import ctypes.util
from decimal import Decimal
import hashlib
import hmac
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

MAGIC = b'FRGFXR01'
MAX_MESSAGE = 64 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


class GCM:
    """Small authenticated-decryption wrapper around the installed OpenSSL."""
    def __init__(self, path=None):
        path = path or ctypes.util.find_library('crypto')
        require(path, 'libcrypto not found; pass --libcrypto')
        self.lib = lib = C.CDLL(path)
        lib.EVP_CIPHER_CTX_new.restype = C.c_void_p
        lib.EVP_CIPHER_CTX_free.argtypes = [C.c_void_p]
        lib.EVP_aes_128_gcm.restype = C.c_void_p
        lib.EVP_aes_256_gcm.restype = C.c_void_p
        lib.EVP_DecryptInit_ex.argtypes = [C.c_void_p] * 5
        lib.EVP_CIPHER_CTX_ctrl.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_void_p]
        lib.EVP_DecryptUpdate.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_int), C.c_void_p, C.c_int]
        lib.EVP_DecryptFinal_ex.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_int)]

    def decrypt(self, key, nonce, aad, data):
        require(len(data) >= 16, 'short AES-GCM record')
        lib = self.lib
        ctx = lib.EVP_CIPHER_CTX_new()
        require(ctx, 'EVP_CIPHER_CTX_new failed')
        out, count, tail = C.create_string_buffer(len(data) + 16), C.c_int(), C.c_int()
        try:
            cipher = lib.EVP_aes_128_gcm() if len(key) == 16 else lib.EVP_aes_256_gcm()
            require(lib.EVP_DecryptInit_ex(ctx, cipher, None, None, None) == 1, 'GCM init failed')
            require(lib.EVP_CIPHER_CTX_ctrl(ctx, 9, len(nonce), None) == 1, 'GCM IV setup failed')
            require(lib.EVP_DecryptInit_ex(ctx, None, None, key, nonce) == 1, 'GCM key setup failed')
            require(lib.EVP_DecryptUpdate(ctx, None, C.byref(count), aad, len(aad)) == 1, 'GCM AAD failed')
            require(lib.EVP_DecryptUpdate(ctx, out, C.byref(count), data[:-16], len(data) - 16) == 1, 'GCM decrypt failed')
            require(lib.EVP_CIPHER_CTX_ctrl(ctx, 0x11, 16, data[-16:]) == 1, 'GCM tag setup failed')
            if lib.EVP_DecryptFinal_ex(ctx, C.byref(out, count.value), C.byref(tail)) != 1:
                return None
            return out.raw[:count.value + tail.value]
        finally:
            lib.EVP_CIPHER_CTX_free(ctx)


def expand(secret, label, length, digest):
    label = b'tls13 ' + label
    info = struct.pack('>H', length) + bytes([len(label)]) + label + b'\0'
    require(length <= digest().digest_size, 'HKDF expansion too long')
    return hmac.new(secret, info + b'\1', digest).digest()[:length]


def udp_chunk(raw):
    """RDPEUDP2 byte shuffle/header parsing, matching core/rdpeudp.c."""
    data = bytearray(raw)
    require(len(data) >= 8, 'short UDP2 data packet')
    data[0], data[7] = data[7], data[0]
    flags = int.from_bytes(data[1:3], 'little') & 0xfff
    pos = 3

    def take(n):
        nonlocal pos
        require(pos + n <= len(data), 'truncated UDP2 header')
        value = data[pos:pos+n]
        pos += n
        return value

    if flags & 1:
        ack = take(7)
        take(ack[6] & 15)
    if flags & 0x40:
        take(1)
    if flags & 0x100:
        take(3)
    if flags & 0x10:
        take(2)
    require(flags & 4, 'expected UDP2 DATA flag')
    take(2)  # DataSeq
    if flags & 8:
        ack = take(3)
        take((4 if ack[2] & 128 else 0) + (ack[2] & 127))
    seq = int.from_bytes(take(2), 'little')
    return seq, bytes(data[pos:])


def reassemble(rows):
    chunks, highest, duplicates = {}, 0, 0
    for timestamp, sequence, body in rows:
        seq = (highest & ~65535) | sequence
        if seq < highest - 32768:
            seq += 65536
        elif seq > highest + 32768:
            seq -= 65536
        require(seq >= 1, 'capture must start with channel sequence one')
        highest = max(highest, seq)
        if seq in chunks:
            require(chunks[seq][1] == body, 'conflicting UDP retransmission')
            duplicates += 1
        else:
            chunks[seq] = timestamp, body
    require(chunks and min(chunks) == 1, 'UDP stream beginning is missing')
    missing = [n for n in range(1, highest + 1) if n not in chunks]
    require(all(n % 65536 == 0 for n in missing), 'missing nonzero UDP channel sequence')
    # Missing zero labels are only provisionally omitted. Authentication of ALL
    # TLS records below must prove this reconstruction before output is written.
    stream, boundaries, times = bytearray(), [], []
    last_time = 0
    for seq in sorted(chunks):
        timestamp, body = chunks[seq]
        stream.extend(body)
        last_time = max(last_time, timestamp)
        boundaries.append(len(stream))
        times.append(last_time)
    return bytes(stream), boundaries, times, {'chunks': len(chunks), 'identical_retransmissions': duplicates, 'omitted_zero_labels': missing}


def tls_plaintexts(stream, boundaries, times, secrets, crypto, stats):
    pos, active, seq, keylen, digest = 0, None, 0, None, None
    handshake_buffer = bytearray()
    while pos < len(stream):
        require(pos + 5 <= len(stream), 'truncated TLS header')
        kind, version, length = struct.unpack_from('>BHH', stream, pos)
        require(kind in (20, 21, 22, 23) and version in (0x301, 0x303), 'invalid TLS header')
        require(length <= 18432 and pos + 5 + length <= len(stream), 'truncated/oversized TLS record')
        header, data = stream[pos:pos+5], stream[pos+5:pos+5+length]
        pos += 5 + length
        timestamp = times[bisect.bisect_left(boundaries, pos)]
        stats['tls_records'] = stats.get('tls_records', 0) + 1
        if kind == 22:
            require(active is None and keylen is None, 'unexpected clear TLS handshake')
            require(len(data) >= 44 and data[0] == 2, 'expected TLS ServerHello')
            offset = 39 + data[38]
            require(offset + 2 <= len(data), 'short ServerHello')
            suite = int.from_bytes(data[offset:offset+2], 'big')
            require(suite in (0x1301, 0x1302), 'only TLS1.3 AES-GCM is supported')
            keylen = 16 if suite == 0x1301 else 32
            digest = hashlib.sha256 if keylen == 16 else hashlib.sha384
            stats['tls_cipher_suite'] = hex(suite)
            continue
        if kind == 20:
            require(data == b'\1', 'invalid TLS ChangeCipherSpec')
            continue
        require(kind == 23 and keylen, 'unexpected TLS record type/state')

        def decrypt(secret, number):
            key = expand(secret, b'key', keylen, digest)
            iv = expand(secret, b'iv', 12, digest)
            nonce = (int.from_bytes(iv, 'big') ^ number).to_bytes(12, 'big')
            return crypto.decrypt(key, nonce, header, data)

        plain = decrypt(active, seq) if active is not None else None
        if plain is None:
            candidates = ['SERVER_HANDSHAKE_TRAFFIC_SECRET'] if active is None else []
            if not stats.get('application_keys_started'):
                candidates.append('SERVER_TRAFFIC_SECRET_0')
            for label in candidates:
                candidate = secrets.get(label)
                if candidate is None:
                    continue
                plain = decrypt(candidate, 0)
                if plain is not None:
                    active, seq = candidate, 0
                    if label == 'SERVER_TRAFFIC_SECRET_0':
                        stats['application_keys_started'] = True
                    break
        require(plain is not None, 'TLS authentication failed; stream incomplete, wrong keys, or unsupported key transition')
        seq += 1
        stats['authenticated_tls_records'] = stats.get('authenticated_tls_records', 0) + 1
        inner = plain.rstrip(b'\0')
        require(inner and inner[-1] in (21, 22, 23), 'invalid TLS inner content type')
        if inner[-1] == 23:
            require(stats.get('application_keys_started'), 'application data under handshake key')
            yield timestamp, inner[:-1]
        elif inner[-1] == 22:
            handshake_buffer.extend(inner[:-1])
            while len(handshake_buffer) >= 4:
                size = 4 + int.from_bytes(handshake_buffer[1:4], 'big')
                require(size <= MAX_MESSAGE, 'oversized TLS handshake')
                if len(handshake_buffer) < size:
                    break
                if handshake_buffer[0] == 24:
                    require(stats.get('application_keys_started') and size == 5, 'invalid TLS KeyUpdate')
                    active = expand(active, b'traffic upd', digest().digest_size, digest)
                    seq = 0
                del handshake_buffer[:size]
    require(not handshake_buffer, 'incomplete TLS handshake message')
    require(stats.get('application_keys_started'), 'no authenticated application stream')


def tunnel_pdus(records, stats):
    buffer = bytearray()
    for timestamp, data in records:
        buffer.extend(data)
        while len(buffer) >= 4:
            action, length, headerlen = struct.unpack_from('<BHB', buffer)
            require(action in (1, 2) and headerlen >= 4, 'unsupported tunnel header')
            size = headerlen + length
            if len(buffer) < size:
                break
            payload = bytes(buffer[headerlen:size])
            del buffer[:size]
            stats['tunnel_pdus'] = stats.get('tunnel_pdus', 0) + 1
            if action == 1:
                require(payload == bytes(4), 'tunnel create response failed')
            elif payload:
                yield timestamp, payload
    require(not buffer, 'incomplete tunnel PDU at end of capture')


def graphics_messages(pdus, stats):
    channels, pending = {}, {}
    graphics_id = None
    for timestamp, pdu in pdus:
        header = pdu[0]
        command, cbid, sp = header >> 4, header & 3, (header >> 2) & 3
        if command in (5, 8, 9):  # capabilities / soft sync require a separate extractor
            raise ValueError('unsupported DVC capability/soft-sync PDU in UDP tunnel')
        require(cbid < 3, 'invalid DVC channel id width')
        width = 1 << cbid
        require(len(pdu) >= 1 + width, 'short DVC header')
        cid = int.from_bytes(pdu[1:1+width], 'little')
        body = pdu[1+width:]
        if command == 1:
            require(body and body.find(b'\0') == len(body) - 1, 'invalid DVC CREATE')
            # IDs of channels rejected by the client may be reused without a
            # server CLOSE. Only the graphics channel is opened by this replay.
            require(cid != graphics_id and cid not in pending, 'graphics channel ID reused')
            channels[cid] = body[:-1]
            if body[:-1] == b'Microsoft::Windows::RDS::Graphics':
                require(graphics_id is None, 'multiple graphics channels are unsupported')
                graphics_id = cid
                stats['graphics_channel_id'] = cid
        elif command in (2, 3):
            require(cid in channels, 'DVC data before CREATE; capture is incomplete')
            if cid != graphics_id:
                continue
            if command == 2:
                require(sp < 3 and cid not in pending, 'invalid or overlapping DVC DATA_FIRST')
                length_width = 1 << sp
                require(len(body) >= length_width, 'short DVC DATA_FIRST')
                size = int.from_bytes(body[:length_width], 'little')
                require(0 < size <= MAX_MESSAGE, 'oversized DVC message')
                pending[cid] = (size, bytearray(body[length_width:]))
            elif cid in pending:
                pending[cid][1].extend(body)
            else:
                require(body and len(body) <= MAX_MESSAGE, 'empty or oversized graphics message')
                yield timestamp, body
                continue
            size, buffer = pending[cid]
            require(len(buffer) <= size, 'DVC fragments exceed declared message length')
            if len(buffer) == size:
                yield timestamp, bytes(buffer)
                del pending[cid]
        elif command == 4:
            require(cid not in pending, 'channel closed with incomplete graphics message')
            channels.pop(cid, None)
        else:
            raise ValueError(f'unsupported DVC command {command}')
    require(graphics_id is not None, 'graphics channel CREATE not present')
    require(not pending, 'incomplete graphics message at end of capture')


def tshark_rows(capture, display_filter, fields):
    command = ['tshark', '-n', '-r', str(capture), '-Y', display_filter, '-T', 'fields']
    for field in fields:
        command += ['-e', field]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        for line in process.stdout:
            yield line.rstrip('\n').split('\t')
        errors = process.stderr.read()
        require(process.wait() == 0, 'tshark failed: ' + errors.strip())
    finally:
        if process.poll() is None:
            process.terminate()
        process.wait()
        process.stdout.close()
        process.stderr.close()


def extract(args):
    hellos = list(tshark_rows(args.capture, 'udp && tls.handshake.type == 1',
        ['udp.stream', 'tls.handshake.random']))
    require(len(hellos) == 1, 'expected exactly one UDP ClientHello; use one complete session capture')
    flow, client_random = hellos[0]
    client_random = client_random.replace(':', '').lower()
    secrets = {}
    for line in args.keys.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1].lower() == client_random:
            secrets[fields[0]] = bytes.fromhex(fields[2])
    require('SERVER_TRAFFIC_SECRET_0' in secrets, 'no matching UDP application secret')
    fields = ['frame.time_epoch', 'rdpudp.data.channelseqnumber', 'udp.payload']
    rows = tshark_rows(args.capture, f'udp.stream == {int(flow)} && udp.srcport == {args.port} && rdpudp.data.channelseqnumber', fields)

    def chunks():
        total = 0
        for timestamp, expected, raw in rows:
            sequence, body = udp_chunk(bytes.fromhex(raw.replace(':', '')))
            require(sequence == int(expected, 0), 'UDP parser disagrees with tshark sequence')
            total += len(body)
            require(total <= args.max_bytes, 'capture exceeds --max-bytes limit')
            yield int(Decimal(timestamp) * 1_000_000), sequence, body

    stream, boundaries, times, stats = reassemble(chunks())
    stats['tls_stream_bytes'] = len(stream)
    stats['encrypted_stream_sha256'] = hashlib.sha256(stream).hexdigest()
    crypto = GCM(args.libcrypto)
    messages = graphics_messages(tunnel_pdus(tls_plaintexts(stream, boundaries, times, secrets, crypto, stats), stats), stats)
    os.umask(0o077)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    require(not args.output.exists(), 'output already exists; choose a new path')
    temp = args.output.with_suffix(args.output.suffix + '.partial')
    first = None
    created_temp = False
    count, payload_bytes = 0, 0
    try:
        with temp.open('xb') as output:
            created_temp = True
            output.write(MAGIC)
            for timestamp, payload in messages:
                if first is None:
                    first = timestamp
                output.write(struct.pack('<QI', timestamp - first, len(payload)))
                output.write(payload)
                count += 1
                payload_bytes += len(payload)
            require(count > 0, 'no graphics messages extracted')
        os.link(temp, args.output)  # Never overwrite another capture's output.
        temp.unlink()
    except BaseException:
        if created_temp:
            temp.unlink(missing_ok=True)
        raise
    stats.update(schema='freerdp.graphics_replay', version=1, capture=str(args.capture.resolve()),
                 messages=count, graphics_bytes=payload_bytes, duration_us=timestamp-first,
                 capture_sha256=hashlib.sha256(args.capture.read_bytes()).hexdigest(),
                 replay_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(stats, indent=2) + '\n')
    print(json.dumps(stats, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--port', type=int, default=3389)
    parser.add_argument('--libcrypto')
    parser.add_argument('--max-bytes', type=int, default=1024 * 1024 * 1024)
    args = parser.parse_args()
    try:
        extract(args)
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f'Extraction failed: {error}\n')


if __name__ == '__main__':
    main()
