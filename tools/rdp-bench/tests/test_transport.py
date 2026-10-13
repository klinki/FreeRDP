import argparse
import contextlib
import datetime
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from rdpbench.calibration import accepted, engine_sources
from rdpbench.common import audit, quantile, read_json, seal, sha256, write_json
from rdpbench.endpoint import H, Parser, record
from rdpbench.link import Link
from rdpbench.native import Native
from rdpbench.report import bootstrap_pair, collect, compare, dashboard
from rdpbench.type1 import one_trial, run_suite, validate_profile


class TransportTests(unittest.TestCase):
    def profile(self, **overrides):
        return dict(rate_mbps=10, one_way_ms=15, jitter_ms=0, loss=0,
                    queue_ms=30, overhead_bytes=28, **overrides)

    def test_ten_mbit_serialization(self):
        link = Link(self.profile(), 11, "c2s")
        self.assertEqual(link.offer(0, b"x" * 1222)[0][0], 16_000_000)
        self.assertEqual(link.offer(0, b"x" * 1222)[0][0], 17_000_000)

    def test_queue_is_bounded(self):
        link = Link(self.profile(), 11, "c2s")
        for _ in range(100):
            link.offer(0, b"x" * 1222)
        self.assertGreater(link.count["queue_drops"], 0)
        self.assertLessEqual(link.next_free, 32_000_000)

    def test_repeatable_impairment_decisions(self):
        profile = self.profile()
        profile.update(loss=.1, jitter_ms=2, duplicate=.1, reorder=.1)
        first, second = Link(profile, 42, "c2s"), Link(profile, 42, "c2s")
        self.assertEqual(
            [first.offer(i * 2_000_000, b"x") for i in range(1000)],
            [second.offer(i * 2_000_000, b"x") for i in range(1000)],
        )

    def test_blackout_end_exclusive(self):
        profile = self.profile()
        profile["blackouts_ms"] = [[10, 20]]
        link = Link(profile, 1, "s2c")
        self.assertFalse(link.offer(10_000_000, b"x"))
        self.assertTrue(link.offer(30_000_000, b"x"))

    def test_total_loss(self):
        profile = self.profile()
        profile["loss"] = 1
        self.assertEqual(Link(profile, 1, "c2s").offer(0, b"x"), [])

    def test_records_split_at_every_byte(self):
        parser = Parser()
        result = []
        data = record(0, 7, 123, 456, b"abc") + record(2, 8, 456, 789, b"P" * 64)
        for byte in data:
            result.extend(parser.feed(bytes([byte])))
        self.assertEqual(result, [(0, 7, 123, 456, b"abc"), (2, 8, 456, 789, b"P" * 64)])
        self.assertFalse(parser.buf)

    def test_goodput_crosses_measurement_start_and_stop(self):
        parser = Parser()
        data = record(0, 0, 0, 0, b"abcdefghij")
        parser.feed(data[:H.size + 3], measured=False)  # warmup/header: excluded
        parser.feed(data[H.size + 3:H.size + 8], measured=True)
        self.assertEqual(parser.measured_payload_bytes, 0)  # not yet verified
        parser.feed(data[H.size + 8:], measured=False)  # integrity drain: excluded
        self.assertEqual(parser.measured_payload_bytes, 5)

    def test_goodput_excludes_headers_and_probes(self):
        parser = Parser()
        parser.feed(record(1, 1, 2, 3, b"P" * 64), measured=True)
        parser.feed(record(2, 1, 2, 3, b"P" * 64), measured=True)
        parser.feed(record(0, 0, 0, 0, b"abc"), measured=True)
        self.assertEqual(parser.measured_payload_bytes, 3)

    def test_corruption_rejected_before_goodput_credit(self):
        data = bytearray(record(0, 1, 2, 3, b"abc"))
        data[-1] ^= 1
        parser = Parser()
        with self.assertRaisesRegex(ValueError, "CRC"):
            parser.feed(data, measured=True)
        self.assertEqual(parser.measured_payload_bytes, 0)

    def test_oversized_record_rejected(self):
        import struct
        data = bytearray(record(0, 1, 2, 3, b"a"))
        data[28:32] = struct.pack("!I", 2**30)
        with self.assertRaises(ValueError):
            Parser().feed(data)

    def test_percentile_and_missing_values(self):
        self.assertEqual(quantile(list(range(1, 101)), .95), 95)
        self.assertEqual(quantile(list(range(1, 101)), .99), 99)
        self.assertIsNone(quantile([], .99))

    def test_invalid_probability_and_nonfinite_settings(self):
        for name, value in [("loss", 2), ("loss", float("nan")), ("rate_mbps", float("inf")), ("rate_mbps", True)]:
            direction = self.profile()
            direction[name] = value
            with self.assertRaises(ValueError):
                validate_profile({"c2s": direction, "s2c": direction})

    def test_artifact_tampering_and_extra_files(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / "a"
            file.write_text("x")
            seal(folder)
            self.assertEqual(audit(folder), [])
            file.write_text("y")
            self.assertTrue(audit(folder))
            seal(folder)
            (Path(folder) / "b").write_text("new")
            self.assertTrue(audit(folder))

    def test_failed_socket_initialization_is_sealed(self):
        with tempfile.TemporaryDirectory() as folder:
            config = read_json(ROOT / "configs/type1.smoke.json")
            profiles = read_json(ROOT / "configs/profiles.json")["profiles"]
            with patch("rdpbench.type1.free_port", side_effect=PermissionError("denied")):
                result = one_trial({"id": "candidate"}, config["cases"][0], profiles["lan"], config, 11, Path(folder) / "trial")
            self.assertEqual(result["sut_outcome"], "FAIL")
            self.assertEqual(result["validity"], "INVALID_HARNESS")
            self.assertIsNone(result["metrics"]["c2s.verified_goodput_mbps"])
            self.assertEqual(audit(Path(folder) / "trial"), [])

    def test_cancellation_seals_trial_and_stops(self):
        with tempfile.TemporaryDirectory() as folder:
            config = read_json(ROOT / "configs/type1.smoke.json")
            profiles = read_json(ROOT / "configs/profiles.json")["profiles"]
            with patch("rdpbench.type1.free_port", side_effect=KeyboardInterrupt):
                with self.assertRaises(KeyboardInterrupt):
                    one_trial({"id": "candidate"}, config["cases"][0], profiles["lan"], config, 11, Path(folder) / "trial")
            self.assertEqual(audit(Path(folder) / "trial"), [])
            self.assertEqual(read_json(Path(folder) / "trial/summary.json")["sut_outcome"], "FAIL")

    def test_missing_adapter_never_uses_plain_udp(self):
        with patch("rdpbench.native.C.CDLL", return_value=object()):
            with self.assertRaisesRegex(RuntimeError, "missing benchmark adapter"):
                Native("library.so", "127.0.0.1", 1234, "127.0.0.1", 5678)

    def test_direct_cannot_ignore_shaping_or_blackouts(self):
        with tempfile.TemporaryDirectory() as folder:
            config = read_json(ROOT / "configs/type1.smoke.json")
            profiles = read_json(ROOT / "configs/profiles.json")["profiles"]
            for profile in (profiles["ten_mbit_clean"], dict(c2s=dict(profiles["loopback"]["c2s"], blackouts_ms=[[0, 10]]), s2c=profiles["loopback"]["s2c"])):
                with self.assertRaises(ValueError):
                    one_trial({"id": "candidate"}, config["cases"][0], profile, config, 11, Path(folder) / "trial", direct=True)
            self.assertFalse((Path(folder) / "trial").exists())

    def test_identifier_rejected_before_writing(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                one_trial({"id": "../escape"}, {"id": "case"}, {}, {}, 1, Path(folder) / "trial")
            self.assertFalse((Path(folder) / "trial").exists())

    def test_campaign_failure_exit_and_fixed_peer_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            config = read_json(ROOT / "configs/type1.smoke.json")
            config["cases"] = [config["cases"][0]]
            write_json(folder / "suite.json", config)
            write_json(folder / "variants.json", {"variants": [
                {"id": "a", "library": "a.so", "env": {"TUNING": "on"}},
                {"id": "b", "library": "b.so", "env": {"TUNING": "off"}},
            ]})
            args = argparse.Namespace(suite=str(folder / "suite.json"), profiles=str(ROOT / "configs/profiles.json"),
                variants=str(folder / "variants.json"), peer_library="reference.so", peer_env=None, calibration=None,
                netns=None, direct=False, case=None, driver="native", output=str(folder / "runs"))
            peers = []
            def fake_trial(variant, case, profile, config, seed, trial, *rest):
                peers.append((variant["peer_library"], variant["peer_env"]))
                return {"trial_id": trial.name, "validity": "PROVISIONAL_UNCALIBRATED",
                        "sut_outcome": "FAIL" if variant["id"] == "b" else "COMPLETE", "reasons": []}
            with patch("rdpbench.type1.one_trial", side_effect=fake_trial), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_suite(args), 1)
            self.assertEqual(peers[0], peers[1])
            self.assertEqual(peers[0][1], {})
            self.assertFalse(read_json(folder / "runs/campaign.json")["pass"])

    def test_calibration_requires_fingerprints_peer_and_evidence(self):
        self.assertFalse(accepted(None, "host", "direct", ROOT, 10, 100)[0])
        with tempfile.TemporaryDirectory() as folder:
            evidence = []
            for name in ("calibration", "overhead"):
                file = Path(folder) / name
                file.write_text("retained measured evidence")
                evidence.append({"path": str(file), "sha256": sha256(file)})
            receipt = {"approved": True, "approved_by": "operator", "host": "host", "engine": "direct",
                "valid_until_utc": (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=1)).isoformat(),
                "max_rate_mbps": 100, "max_packet_rate": 1000, "peer_sha256": "reference",
                "engine_files": {name: sha256(path) for name, path in engine_sources(ROOT).items()}, "evidence": evidence}
            self.assertTrue(accepted(receipt, "host", "direct", ROOT, 10, 100, "reference")[0])
            self.assertFalse(accepted(receipt, "host", "direct", ROOT, 10, 100, "different")[0])
            self.assertFalse(accepted(receipt, "host", "direct", ROOT, 10, 501, "reference")[0])

    def test_paired_bootstrap(self):
        self.assertEqual(bootstrap_pair([10]*5, [8]*5, 1000)["ci95_delta"], [-2, -2])

    def test_tampered_history_visible(self):
        with tempfile.TemporaryDirectory() as folder, tempfile.TemporaryDirectory() as output:
            write_json(Path(folder) / "summary.json", {"trial_id": "1", "metrics": {}})
            seal(folder)
            write_json(Path(folder) / "summary.json", {"trial_id": "1", "metrics": {"fake": 1}})
            self.assertEqual(collect(folder)[0]["validity"], "INVALID_ARTIFACT")
            self.assertEqual(dashboard(folder, output), 1)
            self.assertTrue((Path(output) / "index.html").exists())

    def test_uncalibrated_results_excluded_from_comparison(self):
        with tempfile.TemporaryDirectory() as folder:
            write_json(Path(folder) / "summary.json", {"trial_id": "1", "variant": "candidate",
                "validity": "PROVISIONAL_UNCALIBRATED", "sut_outcome": "COMPLETE", "metrics": {"m": 1}})
            seal(folder)
            result = compare(folder, "baseline", "candidate", "m", Path(folder) / "comparison.json")
            self.assertFalse(result["comparisons"])
            self.assertEqual(len(result["excluded"]), 1)

    def test_missing_summary_does_not_disappear(self):
        with tempfile.TemporaryDirectory() as folder:
            write_json(Path(folder) / "manifest.json", {"trial_id": "1"})
            seal(folder)
            self.assertEqual(collect(folder)[0]["validity"], "INVALID_ARTIFACT")

    def test_missing_trial_does_not_disappear(self):
        with tempfile.TemporaryDirectory() as folder:
            write_json(Path(folder) / "campaign.json", {"trials": [{"trial_id": "removed"}]})
            self.assertEqual(collect(folder)[0]["trial_id"], "removed")
            self.assertEqual(collect(folder)[0]["validity"], "INVALID_ARTIFACT")

    def test_unpaired_valid_trial_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            write_json(Path(folder) / "summary.json", {"trial_id": "1", "variant": "baseline",
                "case_id": "case", "comparison_key": "same", "seed": 11,
                "validity": "VALIDATED_LAB", "sut_outcome": "COMPLETE", "metrics": {"m": 1}})
            seal(folder)
            result = compare(folder, "baseline", "candidate", "m", Path(folder) / "comparison.json")
            self.assertFalse(result["comparisons"])
            self.assertEqual(len(result["excluded"]), 1)

    def test_real_paired_comparison(self):
        with tempfile.TemporaryDirectory() as folder, tempfile.TemporaryDirectory() as output:
            for seed in range(5):
                for variant, value in [("baseline", 10), ("candidate", 12)]:
                    trial = Path(folder) / f"{variant}-{seed}"
                    write_json(trial / "summary.json", {"trial_id": trial.name, "variant": variant,
                        "case_id": "case", "comparison_key": "same", "seed": seed,
                        "validity": "VALIDATED_LAB", "sut_outcome": "COMPLETE", "metrics": {"m": value}})
                    seal(trial)
            result = compare(folder, "baseline", "candidate", "m", Path(output) / "comparison.json")
            self.assertEqual(result["comparisons"][0]["median_delta"], 2)
            self.assertEqual(result["comparisons"][0]["decision"], "CHANGE_DETECTED")


if __name__ == "__main__":
    unittest.main()
