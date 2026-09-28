#!/usr/bin/env python3
"""Boundary and integrity checks; no captured private data required."""
import os
import struct
import unittest

import extract as e


CREATE = b'\x10\x07Microsoft::Windows::RDS::Graphics\0'


class ExtractTests(unittest.TestCase):
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
