"""Dashboard data regressions: pairing, missing measurements and safe exports."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("bench_dashboard", ROOT / "dashboard.py")
DASH = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DASH
SPEC.loader.exec_module(DASH)


def trial(ident, variant, seed, p99, **changes):
    row = {"trial_id": ident, "phase": "main", "case_id": "loaded", "profile_id": "ten_mbit",
           "variant": variant, "seed": seed, "comparison_key": "same", "validity": "PROVISIONAL_UNCALIBRATED",
           "sut_outcome": "COMPLETE", "integrity_verified": True,
           "metrics": {"client.probe_rtt_ms.p99": p99, "c2s.verified_goodput_mbps": 8},
           "probes": {"expected": 20, "injected": 20, "observed": 20, "missed": 0, "timeouts": 0}}
    row.update(changes)
    return DASH.normalize_trial(row)


DECLARATION = [{"phase": "main", "case_id": "loaded", "comparison_key": "same", "baseline_variant": "old",
                "candidate_variant": "new", "metric_names": ["client.probe_rtt_ms.p99", "c2s.verified_goodput_mbps"]}]


class DashboardTests(unittest.TestCase):
    def test_committed_loss_subsets_keep_reliability_exclusions(self):
        result = DASH.campaign(ROOT / "results/loss-stalls-20261012.json", "loss")
        self.assertEqual(len(result["trials"]), 56)
        main = [r for r in result["comparisons"] if r["phase"] == "main"]
        self.assertEqual(sorted(r["paired_n"] for r in main if r["subset"] == DASH.STRICT), [3, 4, 5, 5])
        self.assertEqual([r["paired_n"] for r in main if r["subset"] == DASH.ALL], [5, 5, 5, 5])
        self.assertEqual(sum(r["sut_outcome"] == "FAIL" for r in result["trials"]), 7)
        self.assertEqual(result["calibration"]["failed_sample_count"], 7)
        self.assertIsNotNone(result["auxiliary"]["failed_trace_smoke_v2"])

    def test_pairing_uses_whole_trial_values_not_sample_count_weights(self):
        rows = [trial("o1", "old", 11, 10), trial("n1", "new", 11, 8),
                trial("o2", "old", 29, 1000), trial("n2", "new", 29, 100)]
        rows[0]["metrics"]["client.probe_rtt_ms.count"] = 1000000
        rows[2]["metrics"]["client.probe_rtt_ms.count"] = 1
        strict = next(r for r in DASH.comparisons(rows, DECLARATION) if r["subset"] == DASH.STRICT)
        metric = strict["metrics"]["client.probe_rtt_ms.p99"]
        self.assertEqual(metric["baseline"]["median"], 505)
        self.assertEqual(metric["candidate"]["median"], 54)
        self.assertEqual(metric["paired_delta"]["median"], -451)
        rows[-1]["metrics"]["c2s.verified_goodput_mbps"] = None
        strict = next(r for r in DASH.comparisons(rows, DECLARATION) if r["subset"] == DASH.STRICT)
        self.assertEqual(strict["paired_n"], 2)
        self.assertEqual(strict["metrics"]["c2s.verified_goodput_mbps"]["n"], 1)
        self.assertEqual(strict["metrics"]["c2s.verified_goodput_mbps"]["missing_seeds"], [29])

    def test_artifact_invalid_and_incomplete_probes_cannot_enter_strict_pair(self):
        a = trial("o1", "old", 11, 200, validity="INVALID_ARTIFACT", strict_eligible=True)
        self.assertTrue(all(v is None for v in a["metrics"].values()))
        b = trial("n1", "new", 11, 80)
        self.assertEqual([r["paired_n"] for r in DASH.comparisons([a, b], DECLARATION)], [0, 0])
        a = trial("o1", "old", 11, 200, probes={"expected": 20, "injected": 19, "observed": 19, "missed": 1})
        result = DASH.comparisons([a, b], DECLARATION)
        self.assertEqual(next(r["paired_n"] for r in result if r["subset"] == DASH.STRICT), 0)
        self.assertEqual(next(r["paired_n"] for r in result if r["subset"] == DASH.ALL), 1)
        self.assertIn("Recorded probe service is incomplete", a["reasons"])

    def test_live_unknown_metrics_and_scopes_do_not_manufacture_goodput(self):
        row = DASH.normalize_trial({"trial_id": "live-1", "validity": "LIVE_OBSERVATIONAL", "sut_outcome": "COMPLETE",
                                    "metrics": {"traffic": {"udp_rx_Bps_mean": 1200000}, "verified_goodput_mbps": 9,
                                                "input_visible_ms": {"p95": None, "p99": None}}}, live=True)
        self.assertIsNone(row["metrics"]["verified_goodput_mbps"])
        self.assertEqual(row["metrics"]["traffic.udp_rx_Bps_mean"], 1200000)
        self.assertIsNone(row["metrics"]["input_visible_ms.p99"])
        self.assertTrue(all(v is None for v in row["probes"].values()))
        with self.assertRaises(ValueError):
            DASH.normalize_trial({"trial_id": "bad", "metrics": {"RTT": float("nan")}}, live=True)

    def test_changed_configuration_or_phase_and_duplicate_seed_are_rejected(self):
        rows = [trial("o1", "old", 11, 200), trial("n1", "new", 11, 80, comparison_key="changed"),
                trial("n2", "new", 11, 80, phase="smoke")]
        self.assertEqual([r["paired_n"] for r in DASH.comparisons(rows, DECLARATION)], [0, 0])
        rows.extend([trial("n3", "new", 11, 80), trial("n4", "new", 11, 80)])
        with self.assertRaisesRegex(ValueError, "duplicate paired seed"):
            DASH.comparisons(rows, DECLARATION)

    def test_live_envelope_pairs_missing_metrics_without_fabricated_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.json"
            live = {"schema": "freerdp.live_benchmark", "version": 1,
                    "comparison_variants": {"baseline": "old", "candidate": "new"},
                    "trials": [{"trial_id": ident, "variant": variant, "seed": 11,
                                "case_id": "visible-input", "comparison_key": "matching-vm-config",
                                "validity": "LIVE_OBSERVATIONAL", "sut_outcome": "COMPLETE",
                                "metrics": {"input_visible_ms": {"p95": None, "p99": None},
                                            "verified_goodput_mbps": None, "progress.observed_hz": 15}}
                               for ident, variant in (("vm-old", "old"), ("vm-new", "new"))]}
            path.write_text(json.dumps(live))
            report = DASH.campaign(path, "live")
            strict = next(c for c in report["comparisons"] if c["subset"] == DASH.STRICT)
            self.assertEqual(strict["paired_n"], 1)
            for name, metric in strict["metrics"].items():
                if name == "progress.observed_hz":
                    continue
                self.assertEqual(metric["n"], 0)
                self.assertIsNone(metric["baseline"]["median"])
                self.assertIsNone(metric["candidate"]["median"])
                self.assertEqual(metric["missing_seeds"], [11])
            live["schema"] = "unrecognized"
            path.write_text(json.dumps(live))
            with self.assertRaisesRegex(ValueError, "schema freerdp.live_benchmark"):
                DASH.campaign(path, "live")

    def test_unmeasured_live_setup_is_retained_but_never_an_observed_pair(self):
        row = DASH.normalize_trial({"trial_id": "setup-failed", "phase": "main", "case_id": "loaded",
                                    "variant": "new", "seed": 11, "comparison_key": "same",
                                    "validity": "PARTIAL_E2E_EVIDENCE", "sut_outcome": "INCOMPLETE",
                                    "metrics": {"verified_goodput_mbps": None}}, live=True)
        self.assertFalse(row["measurement_evidence_available"])
        self.assertFalse(row["all_observed_eligible"])
        self.assertIn("No recorded numeric measurement evidence", row["reasons"])
        matched = DASH.comparisons([trial("measured-old", "old", 11, 100), row], DECLARATION)
        self.assertEqual([c["paired_n"] for c in matched], [0, 0])

    def test_export_is_portable_embedded_and_protects_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path = root / "input/report.json"
            input_path.parent.mkdir()
            input_path.write_text("{}")
            with self.assertRaises(ValueError):
                DASH.output_guard(input_path.parent, [input_path])
            with self.assertRaises(ValueError):
                DASH.output_guard(root, [input_path])
            data = DASH.build_data(ROOT / "results/loss-stalls-20261012.json", ROOT / "results/peer-ack-20261011.json")
            data["campaigns"][0]["notes"].append("</script><script>unsafe()</script>")
            output = DASH.output_guard(root / "dashboard", [input_path])
            index = DASH.export(data, output)
            html = index.read_text()
            self.assertNotIn("</script><script>unsafe()", html)
            self.assertIn("\\u003c/script>", html)
            self.assertNotIn("__BENCHMARK_DATA__", html)
            exported = json.loads((output / "data.json").read_text())
            live = next(c for c in exported["campaigns"] if c["id"] == "live")
            self.assertEqual(live["trials"], [])
            self.assertEqual(live["comparisons"], [])
            self.assertTrue((output / "trials.csv").is_file())
            self.assertEqual(input_path.read_text(), "{}")


if __name__ == "__main__":
    unittest.main()
