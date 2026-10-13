"""Visible UDP2 layout, controlled feedback deadlines and campaign invariants."""
import os
import socket
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from ack_timing_sweep import CAPS, prepare
from rdpbench.common import audit, read_json
from rdpbench.feedback import ACK, ACKVEC, AOA, DATA, DELAYACK, OVERHEAD, FeedbackTiming, packet_kind, validate_feedback
from rdpbench.link import Link, Relay
from rdpbench.native import Native
from rdpbench.type1 import one_trial


def protect(layout, dummy=False):
    length = len(layout)
    prefix = (min(7, length) << 5) | (16 if dummy else 0)
    packet = bytearray(bytes([prefix]) + layout + bytes(max(0, 7 - length)))
    packet[0], packet[7] = packet[7], packet[0]
    return bytes(packet)


def packet(flags, parts=b"", *, dummy=False):
    return protect((0xF000 | flags).to_bytes(2, "little") + parts, dummy)


class FeedbackTests(unittest.TestCase):
    profile = dict(rate_mbps=10, one_way_ms=15, jitter_ms=0, loss=0,
                   queue_ms=30, overhead_bytes=28)
    ack = packet(ACK, b"\x01\0\x11\x22\x33\0\0")
    ackvec = packet(ACKVEC, b"\x01\0\x01\xff")
    data = packet(DATA, b"\x01\0\x01\0payload")

    def test_protected_ack_and_vector_advisories_are_selected(self):
        self.assertEqual(packet_kind(self.ack), "ack_only")
        self.assertEqual(packet_kind(self.ackvec), "ackvec_only")
        advisory = packet(ACK | OVERHEAD | DELAYACK | AOA,
                          b"\x01\0\x11\x22\x33\0\0" + b"\x12" + b"\x01\x14\0" + b"\x01\0")
        self.assertEqual(packet_kind(advisory), "ack_only")
        delayed = packet(ACK, b"\x01\0\x11\x22\x33\0\x02\x04\x05")
        self.assertEqual(packet_kind(delayed), "ack_only")
        with_timestamp = packet(ACKVEC, b"\x01\0\x81\x11\x22\x33\x44\xff")
        self.assertEqual(packet_kind(with_timestamp), "ackvec_only")

    def test_data_piggyback_dummy_and_padded_control_are_not_selected(self):
        self.assertEqual(packet_kind(self.data), "data_without_ack")
        self.assertEqual(packet_kind(packet(ACK | DATA, b"\x01\0\x11\x22\x33\0\0\x01\0\x01\0x")), "data_with_ack")
        self.assertEqual(packet_kind(packet(DATA | ACKVEC, b"\x01\0\x01\0\x01\xff\x01\0x")), "data_with_ack")
        self.assertEqual(packet_kind(packet(AOA, b"\x01\0")), "control")
        self.assertEqual(packet_kind(packet(ACK, b"\x01\0\x11\x22\x33\0\0", dummy=True)), "dummy")

    def test_malformed_unknown_truncated_and_unprotected_bytes_are_not_selected(self):
        invalid = [b"", self.ack[:7], self.ack[:-1], self.ack + b"x",
                   packet(ACK | ACKVEC, b"\0" * 12), packet(2),
                   packet(ACK, b"\x01\0\x11\x22\x33\0\x02"),
                   packet(ACKVEC, b"\x01\0\x81\0"),
                   (0xF000 | ACK).to_bytes(2, "little") + b"\0" * 7]
        bad_prefix = bytearray(self.ack)
        bad_prefix[7] |= 1
        invalid.append(bytes(bad_prefix))
        bad_padding = bytearray(packet(AOA, b"\x01\0"))
        bad_padding[6] = 1
        invalid.append(bytes(bad_padding))
        for value in invalid:
            self.assertEqual(packet_kind(value), "unclassified", value.hex())

    def test_classifier_does_not_change_original_bytearray(self):
        value = bytearray(self.ack)
        original = bytes(value)
        self.assertEqual(packet_kind(value), "ack_only")
        self.assertEqual(bytes(value), original)

    def test_ack_vector_coverage_matches_native_decoder_limits(self):
        for encoded in (b"", b"\x80", b"\xc0\x80"):
            self.assertEqual(packet_kind(packet(ACKVEC, b"\x01\0" + bytes([len(encoded)]) + encoded)),
                             "unclassified")
        # Larger legitimate Windows windows must remain ACKs. Coverage is
        # bounded by the wire fields, not the former 1024-packet local cap.
        for encoded in (b"\xbf" * 16 + b"\x90", b"\xbf" * 17, b"\xbf" * 127):
            self.assertEqual(packet_kind(packet(ACKVEC, b"\x01\0" + bytes([len(encoded)]) + encoded)),
                             "ackvec_only")
        self.assertEqual(packet_kind(packet(ACKVEC, b"\x01\0\x01\0")), "ackvec_only")

    def test_deadline_offset_preserves_data_bytes_and_network_serialization(self):
        baseline = Link(self.profile, 11, "s2c")
        treated = Link(self.profile, 11, "s2c", 20)
        for index, data in enumerate((self.ack, self.data, self.ackvec, self.data)):
            now = 1_000_000 + index * 500_000
            expected = baseline.offer(now, data)
            actual = treated.schedule(now, data)
            selected = data in (self.ack, self.ackvec)
            self.assertEqual(actual, [(due + (20_000_000 if selected else 0), payload,
                                       (due, packet_kind(payload)) if selected else None) for due, payload in expected])
        self.assertEqual(treated.next_free, baseline.next_free)
        self.assertEqual(treated.p, baseline.p)
        self.assertEqual(treated.count, baseline.count)

    def test_seeded_loss_duplicate_reordering_decisions_are_unchanged(self):
        profile = dict(self.profile, loss=.1, duplicate=.1, reorder=.1, jitter_ms=1)
        baseline, treated = Link(profile, 47, "s2c"), Link(profile, 47, "s2c", 10)
        for index in range(1000):
            data = self.ack if index % 2 else self.data
            now = index * 2_000_000
            expected, actual = baseline.offer(now, data), treated.schedule(now, data)
            self.assertEqual([(due - (10_000_000 if base is not None else 0), payload)
                              for due, payload, base in actual], expected)
        self.assertEqual(baseline.count, treated.count)

    def test_added_feedback_delay_is_separate_from_dispatch_lateness(self):
        link = Link(self.profile, 11, "s2c", 20)
        due, data, base_due = link.schedule(0, self.ack)[0]
        link.delivered(due, due + 500_000, len(data), base_due)
        summary = link.summary()
        self.assertEqual(summary["lateness_ms"]["p99"], .5)
        self.assertEqual(summary["ack_feedback"]["added_delivery_ms"]["p99"], 20.5)
        self.assertEqual(summary["ack_feedback"]["scheduler_overshoot_ms"]["p99"], .5)
        self.assertEqual(summary["ack_feedback"]["selected_datagrams"], 1)
        record = link.feedback.records[0]
        self.assertEqual(record["kind"], "ack_only")
        self.assertEqual(record["feedback_due_ns"] - record["network_due_ns"], 20_000_000)
        self.assertEqual(record["actual_dispatch_ns"] - record["feedback_due_ns"], 500_000)

    def test_zero_delay_observes_same_packets_and_disabled_path_does_not_classify(self):
        zero, disabled = Link(self.profile, 11, "s2c", 0), Link(self.profile, 11, "s2c")
        expected = disabled.schedule(0, self.ack)[0]
        actual = zero.schedule(0, self.ack)[0]
        self.assertEqual(actual[:2], expected[:2])
        self.assertIsNone(expected[2])
        self.assertIsNotNone(actual[2])
        self.assertEqual(zero.summary()["ack_feedback"]["classification"]["ack_only"], 1)
        self.assertNotIn("ack_feedback", disabled.summary())

    def test_timing_reservoir_keeps_exact_count_and_maximum(self):
        feedback = FeedbackTiming(10)
        feedback.CAPACITY = 2
        for value in (11, 12, 100):
            feedback.observe(0, value * 1_000_000, 99)
        summary = feedback.summary()["added_delivery_ms"]
        self.assertTrue(summary["sampled"])
        self.assertEqual(summary["delivered_count"], 3)
        self.assertEqual(summary["sample_count"], 2)
        self.assertEqual(summary["max"], 100)
        self.assertEqual(feedback.summary()["ledger_overflow_datagrams"], 1)

    def test_feedback_validation_and_direction_selection(self):
        for value in ({}, {"s2c_delay_ms": 10}, {"c2s_delay_ms": 0, "s2c_delay_ms": True},
                      {"c2s_delay_ms": 0, "s2c_delay_ms": float("nan")},
                      {"c2s_delay_ms": 0, "s2c_delay_ms": -1}):
            with self.assertRaises(ValueError):
                validate_feedback(value)
        with patch("rdpbench.link.selectors.DefaultSelector"):
            with patch("rdpbench.link.socket.socket"):
                relay = Relay(1234, 5678, {"c2s": self.profile, "s2c": self.profile}, 11,
                              {"c2s_delay_ms": 5, "s2c_delay_ms": 20})
        self.assertEqual([link.feedback.delay_ms for link in relay.links], [5, 20])

    def test_treatment_is_in_comparison_key_and_failed_trial_remains_sealed(self):
        config = read_json(ROOT / "configs/type1.smoke.json")
        profile = read_json(ROOT / "configs/profiles.json")["profiles"]["ten_mbit_clean"]
        with tempfile.TemporaryDirectory() as temporary:
            manifests = []
            for delay in (0, 10):
                folder = Path(temporary) / str(delay)
                case = {"id": "same", "profile": "ten_mbit_clean", "workload": "upload_mixed",
                        "ack_feedback": {"c2s_delay_ms": 0, "s2c_delay_ms": delay}}
                with patch("rdpbench.type1.free_port", side_effect=PermissionError("fixture")):
                    result = one_trial({"id": "q48"}, case, profile, config, 11, folder)
                self.assertEqual(result["sut_outcome"], "FAIL")
                self.assertEqual(audit(folder), [])
                manifests.append(read_json(folder / "manifest.json"))
            self.assertNotEqual(manifests[0]["comparison_key"], manifests[1]["comparison_key"])
            self.assertEqual(manifests[1]["invariants"]["ack_feedback"]["s2c_delay_ms"], 10)

    def test_treatment_cannot_be_silently_ignored_by_direct_mode(self):
        config = read_json(ROOT / "configs/type1.smoke.json")
        profile = read_json(ROOT / "configs/profiles.json")["profiles"]["loopback"]
        case = {"id": "same", "profile": "loopback", "workload": "probe",
                "ack_feedback": {"c2s_delay_ms": 0, "s2c_delay_ms": 0}}
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "trial"
            with self.assertRaisesRegex(ValueError, "Python relay"):
                one_trial({"id": "q48"}, case, profile, config, 11, folder, direct=True)
            self.assertFalse(folder.exists())

    def test_campaign_preparation_preserves_both_profiles_and_distinct_zero_control(self):
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary) / "fake.so"
            library.write_bytes(b"fixture")
            output = Path(temporary) / "plan"
            configs = prepare(output, library, {})
            self.assertEqual({phase: len(config["cases"]) * len(config["seeds"]) * len(CAPS)
                              for phase, config in configs.items()},
                             {"smoke": 8, "baseline": 40, "ack-timing": 48, "control": 12})
            self.assertTrue(all("ack_feedback" not in case for case in configs["baseline"]["cases"]))
            self.assertEqual({case["ack_feedback"]["s2c_delay_ms"] for case in configs["ack-timing"]["cases"]}, {0, 5, 10, 20})
            self.assertEqual({case["profile"] for case in configs["ack-timing"]["cases"]}, {"ten_mbit", "ten_mbit_clean"})
            prepare(output, library, {})
            library.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "changed binary/source/provenance"):
                prepare(output, library, {})

    @unittest.skipUnless(os.environ.get("RDP_BENCH_TEST_LIBRARY"), "set RDP_BENCH_TEST_LIBRARY for real native ACK validation")
    def test_classifier_reads_actual_native_ack_and_data_wire_bytes(self):
        library = os.environ["RDP_BENCH_TEST_LIBRARY"]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(("127.0.0.1", 0))
            peer.settimeout(2)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as temporary:
                temporary.bind(("127.0.0.1", 0))
                port = temporary.getsockname()[1]
            native = Native(library, "127.0.0.1", port, "127.0.0.1", peer.getsockname()[1])
            try:
                peer.sendto(packet(DATA, b"\x01\0\x01\0Q"), ("127.0.0.1", port))
                self.assertEqual(native.read(50), b"Q")
                actual_ack = peer.recv(65535)
                self.assertEqual(packet_kind(actual_ack), "ack_only", actual_ack.hex())
                native.send(b"X")
                actual_data = peer.recv(65535)
                # The receipt was already emitted above. A subsequent write must
                # not repeat a stale ACK just because the peer once sent DATA.
                self.assertEqual(packet_kind(actual_data), "data_without_ack", actual_data.hex())
                # Retire X, then receive another channel byte while the peer
                # explicitly permits a one-second, 15-addition ACK batch. This
                # leaves a genuine pending receipt to piggyback on the next write.
                peer.sendto(packet(ACK, b"\x01\0\0\0\0\0\0"), ("127.0.0.1", port))
                peer.sendto(packet(DATA | DELAYACK,
                                   b"\x0f\xe8\x03" + b"\x02\0\x02\0R"),
                            ("127.0.0.1", port))
                self.assertEqual(native.read(50), b"R")
                native.send(b"Y")
                actual_piggyback = peer.recv(65535)
                self.assertEqual(packet_kind(actual_piggyback), "data_with_ack", actual_piggyback.hex())
                restored = bytearray(actual_piggyback)
                restored[0], restored[7] = restored[7], restored[0]
                self.assertEqual(int.from_bytes(restored[3:5], "little"), 2)
                self.assertEqual(restored[-1:], b"Y")
            finally:
                native.close()


if __name__ == "__main__":
    unittest.main()
