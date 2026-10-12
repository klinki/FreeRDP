"""CRC corruption and independently mismatched echo/relay evidence regressions."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from live_calibrate import finalize, packet
from rdpbench.common import read_json, write_json
from rdpbench.link import Link
from windows.udp_calibration import decode_packet


class LiveCalibrationTests(unittest.TestCase):
    def test_crc_covers_body_metadata_and_timestamp(self):
        data = packet(uuid.UUID(int=123), 7, 456, 1232, 11)
        decoded = decode_packet(data)
        self.assertEqual((decoded["seq"], decoded["sent_ns"], decoded["udp_bytes"]), (7, 456, 1232))
        self.assertTrue(decoded["crc_valid"])
        for offset in (8, 25, 30, -1):
            changed = bytearray(data)
            changed[offset] ^= 1
            self.assertFalse(decode_packet(changed)["crc_valid"])
        self.assertIsNone(decode_packet(data[:20]))
        self.assertIsNone(decode_packet(b"badmagic"+data[8:]))

    def fixture(self, output):
        folder = output/"sample"
        folder.mkdir()
        direction = dict(rate_mbps=10, one_way_ms=15, jitter_ms=0, loss=0,
                         queue_ms=30, overhead_bytes=28, duplicate=0, reorder=0)
        profile = dict(c2s=dict(direction), s2c=dict(direction))
        sent_ns = 100_000_000_000
        sample_id = uuid.UUID(int=123).hex
        offers = {"c2s": sent_ns+1_000_000, "s2c": sent_ns+17_000_000}
        ledger, directions = [], {}
        for name in ("c2s", "s2c"):
            link = Link(profile[name], 11+(1000003 if name == "s2c" else 0), name)
            link.start_ns = sent_ns-1_000_000
            due = link.schedule(offers[name], bytes(64))[0][0]
            actual = due+100_000
            link.delivered(due, actual, 64)
            summary = dict(link.summary(), queue_drops=0, random_drops=0, blackout_drops=0,
                           socket_send_errors=0, pending_datagrams=0,
                           packet_accounting=dict(received=1, accounted=1, balanced=True))
            directions[name] = summary
            ledger.extend([dict(type="offer", direction=name, packet_id=1, offered_ns=offers[name], udp_bytes=64),
                           dict(type="dispatch", direction=name, packet_id=1, due_ns=due,
                                actual_dispatch_ns=actual, lateness_ns=actual-due, udp_bytes=64)])
        relay = dict(valid=True, state="restored", sockets_closed=True, error=None,
                     ledger=dict(valid=True, closed=True), directions=directions, udp_errors={},
                     tcp_connections={}, started_ns=sent_ns-1_000_000)
        write_json(folder/"relay-restored.json", relay)
        (folder/"relay.jsonl").write_text("".join(json.dumps(r)+"\n" for r in ledger))
        (folder/"host.jsonl").write_text(json.dumps(dict(type="send", seq=0, sent_ns=sent_ns))+"\n")
        sample = dict(sample_id=sample_id, profile_id="ten_mbit_clean", profile=profile, seed=11,
                      mode="unloaded", evidence="sample", relay_ledger="sample/relay.jsonl",
                      packet_bytes=64, seconds=1, error=None, verified_echo_payload_mbps=.000512,
                      counts=dict(sent_datagrams=1, sent_udp_bytes=64, received_datagrams=1, received_udp_bytes=64),
                      rtt_ms=dict(count=1, p50=32.7, p95=32.7, p99=32.7, max=32.7))
        write_json(output/"host-campaign.json", dict(samples=[sample]))
        remote = output/"receiver.jsonl"
        remote.write_text(json.dumps(dict(sample_id=sample_id, seq=0, sent_ns=sent_ns, udp_bytes=64,
                                         crc_valid=True, received_time_ns=555, echoed_time_ns=655))+"\n")
        summary = output/"receiver-summary.json"
        write_json(summary, dict(valid=True, error=None, clock="unaligned remote clock"))
        return remote, summary

    def audit(self, output, remote, summary):
        with contextlib.redirect_stdout(io.StringIO()):
            status = finalize(output, remote, summary)
        return status, read_json(output/"calibration.json")

    def test_valid_fixture_keeps_remote_clock_unaligned_and_no_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            remote, summary = self.fixture(output)
            status, result = self.audit(output, remote, summary)
            self.assertEqual(status, 0)
            self.assertFalse(result["accepted_calibration_receipt"])
            self.assertIn("unaligned", result["limitations"][1])
            self.assertEqual(result["samples"][0]["receiver_echo_service_ms"]["p50"], .0001)

    def test_deadline_tampering_fails_independent_seeded_audit(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            remote, summary = self.fixture(output)
            path = output/"sample/relay.jsonl"
            ledger = [json.loads(line) for line in path.read_text().splitlines()]
            ledger[1]["due_ns"] += 1
            path.write_text("".join(json.dumps(r)+"\n" for r in ledger))
            status, result = self.audit(output, remote, summary)
            self.assertEqual(status, 1)
            failed = [c["criterion"] for c in result["samples"][0]["criteria"] if not c["pass"]]
            self.assertIn("c2s seeded schedule/deadline/drop ledger audit", failed)

    def test_receiver_timestamp_mismatch_fails_despite_balanced_counts(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            remote, summary = self.fixture(output)
            row = json.loads(remote.read_text())
            row["sent_ns"] += 1
            remote.write_text(json.dumps(row)+"\n")
            status, result = self.audit(output, remote, summary)
            self.assertEqual(status, 1)
            failed = [c["criterion"] for c in result["samples"][0]["criteria"] if not c["pass"]]
            self.assertIn("VM received sequence/timestamp/length identity", failed)


if __name__ == "__main__":
    unittest.main()
