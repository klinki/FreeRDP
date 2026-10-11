import contextlib
import csv
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from rdpbench.common import read_json, seal, write_json

SPEC = importlib.util.spec_from_file_location("baseline_report", ROOT / "baseline_report.py")
reporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reporter)


class BaselineReportTests(unittest.TestCase):
    def trial(self, root, ident, *, validity="PROVISIONAL_UNCALIBRATED", outcome="COMPLETE",
              integrity=True, metrics=None, failures=None, reasons=None, key="same"):
        folder = Path(root) / ident
        summary = {"trial_id": ident, "case_id": "ten-down", "variant": "local", "comparison_key": key,
                   "validity": validity, "sut_outcome": outcome, "integrity_verified": integrity,
                   "sut_failures": failures or [], "reasons": reasons or [], "metrics": metrics or {}}
        write_json(folder / "summary.json", summary)
        write_json(folder / "manifest.json", {"trial_id": ident, "profile_id": "ten_mbit",
                                              "invariants": {"case": {"id": "ten-down", "workload": "download"}}})
        seal(folder)
        return summary

    def campaign(self, root, trials):
        write_json(Path(root) / "campaign.json", {"version": 1, "trials": trials,
                                                  "pass": all(x.get("sut_outcome") == "COMPLETE" for x in trials)})

    def test_medians_of_trial_percentiles_with_repeat_ranges_and_coverage(self):
        with tempfile.TemporaryDirectory() as root:
            rows = [self.trial(root, f"trial-{i}", metrics={"s2c.verified_goodput_mbps": goodput,
                    "client.probe_rtt_ms.p95": percentile, "client.probe_rtt_ms.p99": percentile + 10,
                    "client.probe_rtt_ms.count": samples})
                    for i, (goodput, percentile, samples) in enumerate(((9, 10, 1), (7, 30, 1000), (None, 20, 10)))]
            self.campaign(root, rows)
            report = reporter.aggregate_campaign(root)
            case = report["cases"][0]
            p95 = case["all_observed"]["metrics"]["client.probe_rtt_ms.p95"]
            self.assertEqual((p95["median"], p95["min"], p95["max"]), (20, 10, 30))
            goodput = case["all_observed"]["metrics"]["s2c.verified_goodput_mbps"]
            self.assertEqual(goodput["median"], 8)
            self.assertEqual((goodput["contributing_trials"], goodput["missing_trials"]), (2, 1))
            self.assertEqual(case["profile_id"], "ten_mbit")
            self.assertEqual(case["workload"], "download")
            self.assertEqual(case["counts"]["expected_trials"], 3)
            self.assertEqual(case["validated_lab_complete_only"]["trial_count"], 0)
            self.assertEqual(case["artifact_intact_timing_valid_complete"]["trial_count"], 3)

    def test_failures_remain_in_diagnostics_and_subsets_are_explicit(self):
        with tempfile.TemporaryDirectory() as root:
            rows = [self.trial(root, "accepted", validity="VALIDATED_LAB", metrics={"s2c.verified_goodput_mbps": 8}),
                    self.trial(root, "provisional", metrics={"s2c.verified_goodput_mbps": 10}),
                    self.trial(root, "sut-failure", outcome="FAIL", metrics={"s2c.verified_goodput_mbps": 2},
                               failures=["probe deadlines unserved/unobserved"]),
                    self.trial(root, "timing-failure", validity="INVALID_HARNESS", metrics={"s2c.verified_goodput_mbps": 4},
                               reasons=["relay timing exceeded tolerance"]),
                    self.trial(root, "integrity-failure", outcome="FAIL", integrity=False,
                               metrics={"s2c.verified_goodput_mbps": 999}, failures=["application data incomplete after drain"])]
            self.campaign(root, rows)
            report = reporter.aggregate_campaign(root)
            case = report["cases"][0]
            self.assertEqual(case["all_observed"]["trial_count"], 5)
            self.assertEqual(case["all_observed"]["metrics"]["s2c.verified_goodput_mbps"]["median"], 6)
            self.assertEqual(case["validated_lab_complete_only"]["trial_ids"], ["accepted"])
            self.assertEqual(case["artifact_intact_timing_valid_complete"]["trial_ids"], ["accepted", "provisional"])
            self.assertEqual(case["counts"]["sut_failed_trials"], 2)
            self.assertEqual(case["counts"]["invalid_harness_trials"], 1)
            self.assertEqual(case["counts"]["integrity_failed_trials"], 1)
            self.assertEqual(case["invalid_harness_reasons"][0]["reason"], "relay timing exceeded tolerance")
            self.assertEqual(case["integrity_failures"][0]["trial_ids"], ["integrity-failure"])

    def test_audit_failure_preserves_original_harness_reasons(self):
        with tempfile.TemporaryDirectory() as root:
            row = self.trial(root, "tampered", validity="INVALID_HARNESS", reasons=["relay late"],
                             metrics={"client.probe_rtt_ms.p95": 100})
            self.campaign(root, [row])
            (Path(root) / "tampered" / "extra").write_text("unsealed", encoding="utf-8")
            report = reporter.aggregate_campaign(root)
            trial = report["trials"][0]
            self.assertEqual(trial["validity"], "INVALID_ARTIFACT")
            self.assertEqual(trial["original_validity"], "INVALID_HARNESS")
            self.assertEqual(trial["harness_reasons"], ["relay late"])
            self.assertIn("unsealed: extra", trial["artifact_reasons"])
            self.assertEqual(report["counts"]["invalid_harness_trials"], 1)
            self.assertEqual(report["counts"]["invalid_artifact_trials"], 1)
            self.assertEqual(report["cases"][0]["all_observed"]["metrics"]["client.probe_rtt_ms.p95"]["median"], 100)

    def test_missing_planned_trial_is_retained_with_metadata_and_missing_metrics(self):
        with tempfile.TemporaryDirectory() as root:
            row = {"trial_id": "missing", "case_id": "ten-down", "variant": "local", "comparison_key": "same",
                   "validity": "INVALID_HARNESS", "sut_outcome": "FAIL", "reasons": ["recorded runner failure"],
                   "sut_failures": ["probe deadlines unserved/unobserved"],
                   "metrics": {"s2c.verified_goodput_mbps": 999}}
            self.campaign(root, [row])
            report = reporter.aggregate_campaign(root)
            case = report["cases"][0]
            self.assertEqual(report["counts"]["expected_trials"], 1)
            self.assertEqual(report["counts"]["observed_trials"], 1)
            self.assertEqual(case["case_id"], "ten-down")
            self.assertEqual(case["all_observed"]["metrics"]["s2c.verified_goodput_mbps"]["median"], None)
            self.assertEqual(case["all_observed"]["metrics"]["s2c.verified_goodput_mbps"]["missing_trials"], 1)
            self.assertEqual(case["validated_lab_complete_only"]["trial_count"], 0)
            self.assertEqual(case["counts"]["invalid_artifact_trials"], 1)
            self.assertEqual(case["counts"]["sut_failed_trials"], 1)
            self.assertEqual(case["invalid_harness_reasons"][0]["reason"], "recorded runner failure")
            self.assertEqual(report["trials"][0]["diagnostic_summary_source"], "campaign_record")

    def test_probe_counts_sum_without_turning_unknowns_into_zero(self):
        with tempfile.TemporaryDirectory() as root:
            rows = [self.trial(root, f"trial-{i}", metrics={
                    "client.probes_expected": 100, "client.probes_injected": 90,
                    "client.probes_observed": 80, "client.probes_not_injected": 10,
                    "client.probe_timeouts": 10}) for i in range(2)]
            rows.append(self.trial(root, "unknown"))
            self.campaign(root, rows)
            counts = reporter.aggregate_campaign(root)["cases"][0]["all_observed"]["probe_counts"]["client"]
            self.assertEqual({key: value["sum"] for key, value in counts.items() if key in ("expected", "injected", "observed", "missed", "timeouts")},
                             {"expected": 200, "injected": 180, "observed": 160, "missed": 20, "timeouts": 20})
            self.assertEqual(counts["expected"]["missing_trials"], 1)
            self.assertEqual(counts["rtt_samples"]["sum"], None)

    def test_comparison_keys_are_never_mixed(self):
        with tempfile.TemporaryDirectory() as root:
            rows = [self.trial(root, "a", key="first", metrics={"s2c.verified_goodput_mbps": 5}),
                    self.trial(root, "b", key="second", metrics={"s2c.verified_goodput_mbps": 10})]
            self.campaign(root, rows)
            self.assertEqual(len(reporter.aggregate_campaign(root)["cases"]), 2)

    def test_cli_emits_json_csv_markdown_and_cannot_mutate_campaign(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as output:
            rows = [self.trial(root, "trial", metrics={"s2c.verified_goodput_mbps": 9,
                                                      "client.probe_rtt_ms.p95": 30})]
            self.campaign(root, rows)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(reporter.main(["--input", root, "--output", output]), 0)
            result = read_json(Path(output) / "aggregate.json")
            self.assertEqual(result["counts"]["observed_trials"], 1)
            with (Path(output) / "aggregate.csv").open(newline="", encoding="utf-8") as stream:
                csv_rows = list(csv.DictReader(stream))
            goodput = next(r for r in csv_rows if r["subset"] == "all_observed" and r["metric"] == "s2c.verified_goodput_mbps")
            self.assertEqual(goodput["median"], "9")
            self.assertEqual(goodput["unit"], "Mbit/s")
            empty = next(r for r in csv_rows if r["subset"] == "validated_lab_complete_only" and r["metric"] == "s2c.verified_goodput_mbps")
            self.assertEqual(empty["median"], "")
            markdown = (Path(output) / "report.md").read_text(encoding="utf-8")
            self.assertIn("never pooled percentiles", markdown)
            self.assertIn("Provisional local calibration", markdown)
            self.assertIn("calibration can remain provisional", markdown)
            with self.assertRaisesRegex(ValueError, "outside the input campaign"):
                reporter.write_report(result, Path(root) / "report")

    def test_nonfinite_bool_and_missing_metrics_are_not_numeric_samples(self):
        rows = [{"metrics": {"client.probe_rtt_ms.p95": x}}
                for x in (float("nan"), float("inf"), True, None, 20)]
        result = reporter.metric_summary(rows, "client.probe_rtt_ms.p95")
        self.assertEqual(result["median"], 20)
        self.assertEqual(result["contributing_trials"], 1)
        self.assertEqual(result["missing_trials"], 4)


if __name__ == "__main__":
    unittest.main()
