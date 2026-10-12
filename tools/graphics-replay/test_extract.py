#!/usr/bin/env python3
"""Boundary and integrity checks; no captured private data required."""
import os
import contextlib
import ctypes as C
import io
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import extract as e


CREATE = b'\x10\x07Microsoft::Windows::RDS::Graphics\0'
CLIENT_RANDOM = '11' * 32


def authenticated_stream():
    """Public TLS 1.3 fixture encrypted independently by OpenSSL."""
    payload = b'fixture graphics bytes'
    plaintext = (struct.pack('<BHB', 2, len(CREATE), 4) + CREATE +
                 struct.pack('<BHB', 2, len(payload) + 2, 4) + b'\x30\x07' + payload + b'\x17')
    header = b'\x17\x03\x03' + struct.pack('>H', len(plaintext) + 16)
    # Fixed public vector for traffic secret bytes(range(32)), SHA-256,
    # TLS_AES_128_GCM_SHA256, application record sequence zero.
    key = bytes.fromhex('9c9783cf77ea32d44f369da41f19f3cc')
    nonce = bytes.fromhex('2f41c846a431a163814bcd71')
    lib = e.GCM(os.environ.get('REPLAY_LIBCRYPTO')).lib
    lib.EVP_EncryptInit_ex.argtypes = [C.c_void_p] * 5
    lib.EVP_EncryptUpdate.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_int),
                                    C.c_void_p, C.c_int]
    lib.EVP_EncryptFinal_ex.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_int)]
    ctx = lib.EVP_CIPHER_CTX_new()
    if not ctx:
        raise RuntimeError('fixture GCM allocation failed')
    encrypted, count, tail, tag = C.create_string_buffer(len(plaintext) + 16), C.c_int(), C.c_int(), C.create_string_buffer(16)
    try:
        for status in (lib.EVP_EncryptInit_ex(ctx, lib.EVP_aes_128_gcm(), None, None, None),
                       lib.EVP_CIPHER_CTX_ctrl(ctx, 9, len(nonce), None),
                       lib.EVP_EncryptInit_ex(ctx, None, None, key, nonce),
                       lib.EVP_EncryptUpdate(ctx, None, C.byref(count), header, len(header)),
                       lib.EVP_EncryptUpdate(ctx, encrypted, C.byref(count), plaintext, len(plaintext))):
            if status != 1:
                raise RuntimeError('fixture GCM encryption failed')
        written = count.value
        if (lib.EVP_EncryptFinal_ex(ctx, C.byref(encrypted, written), C.byref(tail)) != 1 or
                lib.EVP_CIPHER_CTX_ctrl(ctx, 0x10, len(tag), tag) != 1):
            raise RuntimeError('fixture GCM finalization failed')
        ciphertext = encrypted.raw[:written + tail.value] + tag.raw
    finally:
        lib.EVP_CIPHER_CTX_free(ctx)
    hello = b'\x02\x00\x00\x28\x03\x03' + bytes(32) + b'\x00\x13\x01\x00\x00\x00'
    return b'\x16\x03\x03' + struct.pack('>H', len(hello)) + hello + header + ciphertext, payload


def udp_packet(body):
    packet = bytearray(b'\x00' + struct.pack('<HHH', 0x5004, 1, 1) + body)
    packet[0], packet[7] = packet[7], packet[0]
    return bytes(packet)


class ExtractTests(unittest.TestCase):
    def run_fixture_extract(self, hellos, body):
        stream, payload = authenticated_stream()
        raw = udp_packet(stream if body is None else body(stream))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture, keys, output = root / 'capture.pcapng', root / 'keys.txt', root / 'replay.bin'
            capture.write_bytes(b'public synthetic capture fixture')
            keys.write_text(f'SERVER_TRAFFIC_SECRET_0 {CLIENT_RANDOM} {bytes(range(32)).hex()}\n')
            args = SimpleNamespace(capture=capture, keys=keys, output=output, port=3389,
                                   libcrypto=os.environ.get('REPLAY_LIBCRYPTO'), max_bytes=1024)

            def rows(path, display_filter, fields):
                self.assertEqual(path, capture)
                if display_filter == 'udp && tls.handshake.type == 1':
                    return iter(hellos)
                self.assertIn('udp.stream == 4 && udp.srcport == 3389', display_filter)
                return iter([['12.5', '1', raw.hex()]])

            previous_umask = os.umask(0o077)
            try:
                with patch.object(e, 'tshark_rows', side_effect=rows), contextlib.redirect_stdout(io.StringIO()):
                    e.extract(args)
                self.assertEqual(output.read_bytes(), e.MAGIC + struct.pack('<QI', 0, len(payload)) + payload)
                self.assertEqual(capture.read_bytes(), b'public synthetic capture fixture')
                self.assertTrue(output.with_suffix('.bin.json').is_file())
            except ValueError:
                self.assertFalse(output.exists())
                self.assertFalse(output.with_suffix('.bin.partial').exists())
                raise
            finally:
                os.umask(previous_umask)

    def test_retransmitted_client_hello_extracts_authenticated_stream(self):
        self.run_fixture_extract([['4', CLIENT_RANDOM]] * 3, None)

    def test_multiple_unique_client_hello_sessions_are_rejected(self):
        for hellos in ([], [['4', CLIENT_RANDOM], ['5', CLIENT_RANDOM]],
                       [['4', CLIENT_RANDOM], ['4', '22' * 32]]):
            with self.subTest(hellos=hellos), patch.object(e, 'tshark_rows', return_value=iter(hellos)):
                args = SimpleNamespace(capture=Path('unused.pcapng'))
                with self.assertRaisesRegex(ValueError, 'one unique UDP ClientHello session'):
                    e.extract(args)

    def test_retransmitted_client_hello_does_not_bypass_tls_authentication(self):
        def corrupt(stream):
            return stream[:-1] + bytes([stream[-1] ^ 1])

        with self.assertRaisesRegex(ValueError, 'TLS authentication failed'):
            self.run_fixture_extract([['4', CLIENT_RANDOM]] * 2, corrupt)

    def test_reordering_retransmission_and_timestamps(self):
        data, ends, times, stats = e.reassemble([(10, 1, b'a'), (30, 3, b'c'),
                                                (40, 2, b'b'), (50, 1, b'a')])
        self.assertEqual(data, b'abc')
        self.assertEqual(ends, [1, 2, 3])
        self.assertEqual(times, [10, 40, 40])
        self.assertEqual(stats['identical_retransmissions'], 1)

    def test_gaps_conflicting_duplicates_and_missing_start_fail(self):
        for rows in [[(1, 1, b'a'), (2, 3, b'c')],
                     [(1, 1, b'a'), (2, 1, b'b')], [(1, 2, b'b')]]:
            with self.assertRaises(ValueError):
                e.reassemble(rows)

    def test_wrap_zero_is_only_provisional(self):
        rows = [(n, n, b'x') for n in range(1, 65536)] + [(65537, 1, b'y')]
        stream, _, _, stats = e.reassemble(rows)
        self.assertEqual(stats['omitted_zero_labels'], [65536])
        self.assertEqual(stream[-1:], b'y')

    def test_udp_header_bounds(self):
        for raw in [b'', bytes(7), bytes(8), bytes([0xff]) * 8]:
            with self.assertRaises(ValueError):
                e.udp_chunk(raw)

    def test_graphics_fragments_and_reused_rejected_channel(self):
        pdus = [(0, CREATE), (1, b'\x10\x0aUnsupported\0'),
                (2, b'\x10\x0aAnother\0'), (3, b'\x20\x07\x06abc'),
                (4, b'\x30\x07def'), (5, b'\x30\x07xyz')]
        self.assertEqual(list(e.graphics_messages(pdus, {})), [(4, b'abcdef'), (5, b'xyz')])

    def test_fragment_failures(self):
        for pdus in [[(0, b'\x30\x07x')], [(0, CREATE), (1, b'\x20\x07\x06abc')],
                     [(0, CREATE), (1, b'\x20\x07\x02abc')],
                     [(0, CREATE), (1, CREATE)]]:
            with self.assertRaises(ValueError):
                list(e.graphics_messages(pdus, {}))

    def test_tunnel_split_at_every_byte(self):
        payload = b'\x30\x07abcdefgh'
        data = struct.pack('<BHB', 2, len(payload), 4) + payload
        self.assertEqual(list(e.tunnel_pdus([(n, bytes([v])) for n, v in enumerate(data)], {})),
                         [(len(data)-1, payload)])
        with self.assertRaises(ValueError):
            list(e.tunnel_pdus([(0, data[:-1])], {}))

    def test_authenticated_decryption_nist_vector_and_corruption(self):
        crypto = e.GCM(os.environ.get('REPLAY_LIBCRYPTO'))
        ciphertext = bytes.fromhex('0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf')
        self.assertEqual(crypto.decrypt(bytes(16), bytes(12), b'', ciphertext), bytes(16))
        damaged = ciphertext[:-1] + bytes([ciphertext[-1] ^ 1])
        self.assertIsNone(crypto.decrypt(bytes(16), bytes(12), b'', damaged))
        self.assertIsNone(crypto.decrypt(bytes(16), bytes(12), b'wrong aad', ciphertext))


if __name__ == '__main__':
    unittest.main()
