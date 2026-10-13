"""Derived live report regressions; no VM, browser or timed benchmark runs."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location("bench_live_report", ROOT / "live_report.py")
REPORT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPORT
SPEC.loader.exec_module(REPORT)


def envelope(phase="smoke", seed=11):
    return {"schema": "freerdp.live_benchmark", "version": 1, "campaign_id": phase,
            "provenance": {"variants": [{"id": "reference"}, {"id": "candidate"}]},
            "network_profiles": [{"id": "ten", "rate": 10}],
            "calibration": {"approved": False, "note": "old calibration"},
            "trials": [{"trial_id": phase + "-" + variant, "phase": phase,
                        "case_id": "canvas", "profile_id": "ten", "variant": variant,
                        "seed": seed, "comparison_key": "matching", "validity": "PARTIAL_E2E_EVIDENCE",
                        "sut_outcome": "COMPLETE", "strict_eligible": False, "integrity_verified": False,
                        "metrics": {"input_visible_ms.p99": value, "verified_goodput_mbps": None},
                        "probes": {"requested": 2, "injected": 2, "observed": 2,
                                   "unobserved": 0, "missed_injections": 0, "timeouts": 0,
                                   "observed_ids": [1, 2], "censored_ids": []},
                        "reasons": ["graphics routing remains unproved"]}
                       for variant, value in (("reference", 200), ("candidate", 100))]}


class LiveReportTests(unittest.TestCase):
    def write(self, root, name, value):
        path = root / name
        path.write_text(json.dumps(value))
        return path

    def test_preserved_preflights_and_partial_service_keep_distinct_phase_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            live = self.write(root, "smoke.json", envelope())
            failures = self.write(root, "failed.json", {"old-smoke": {"trials": 1, "outcomes": [
                {"trial_id": "failed-setup", "validity": "PARTIAL_E2E_EVIDENCE",
                 "sut_outcome": "INCOMPLETE", "reasons": ["client exited before connection"]}]}})
            before = live.read_bytes(), failures.read_bytes()
            result = REPORT.combine([live], preflights=failures)
            self.assertEqual(result["counts"]["outcomes"], {"COMPLETE": 2, "INCOMPLETE": 1})
            self.assertEqual(result["counts"]["strict_eligible"], 0)
            failed = result["trials"][-1]
            self.assertEqual(failed["phase"], "preflight-old-smoke")
            self.assertIsNone(failed["metrics"]["verified_goodput_mbps"])
            self.assertEqual(failed["probes"], {})
            self.assertEqual(result["trials"][0]["probes"]["observed_ids"], [1, 2])
            by_subset = {c["subset"]: c for c in result["comparisons"]}
            self.assertEqual(by_subset["artifact_intact_timing_valid_complete"]["paired_n"], 0)
            self.assertEqual(by_subset["all_observed_diagnostic"]["paired_n"], 1)
            REPORT.export(result, root / "derived")
            self.assertEqual((live.read_bytes(), failures.read_bytes()), before)

    def test_same_seed_in_different_phases_is_never_pooled(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            smoke, main = envelope(), envelope("main")
            main["trials"][0]["metrics"]["input_visible_ms.p99"] = 2000
            paths = [self.write(root, "smoke.json", smoke), self.write(root, "main.json", main)]
            result = REPORT.combine(paths)
            compared = [c for c in result["comparisons"] if c["subset"] == "all_observed_diagnostic"]
            self.assertEqual(len(compared), 2)
            self.assertEqual({c["phase"]: c["metrics"]["input_visible_ms.p99"]["baseline"]["median"] for c in compared}, {"smoke": 200, "main": 2000})
            self.assertTrue(all(c["paired_n"] == 1 for c in compared))

    def test_latest_calibration_is_count_checked_and_previous_evidence_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            live = self.write(root, "smoke.json", envelope())
            latest = {"sample_count": 2, "strict_passed": 1, "accepted_calibration_receipt": False,
                      "strict_pass": False, "scope": "CRC echo only", "samples": [
                          {"pass": True}, {"pass": False, "failed_criteria": [{"criterion": "queue drops", "detail": 16}]}]}
            calibration = self.write(root, "calibration.json", latest)
            result = REPORT.combine([live], calibration=calibration)
            self.assertEqual(result["calibration"]["failed_sample_count"], 1)
            self.assertEqual(result["calibration"]["history"][0]["evidence"]["note"], "old calibration")
            self.assertEqual(result["calibration"]["failed_records"][0]["failed_criteria"][0]["detail"], 16)
            self.assertFalse(result["calibration"]["approved"])
            latest["strict_passed"] = 2
            calibration.write_text(json.dumps(latest))
            with self.assertRaisesRegex(ValueError, "counts disagree"):
                REPORT.combine([live], calibration=calibration)

    def test_duplicate_trial_changed_profile_and_output_overwrite_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            a = self.write(root, "a.json", envelope())
            b = self.write(root, "b.json", envelope())
            with self.assertRaisesRegex(ValueError, "duplicate trial_id"):
                REPORT.combine([a, b])
            changed = envelope("main")
            changed["network_profiles"][0]["rate"] = 5
            b.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, "profile ten changed"):
                REPORT.combine([a, b])
            with self.assertRaisesRegex(ValueError, "separate from input"):
                REPORT.output_guard(root, [a])

    def test_bridge_explicit_display_failure_is_distinct_from_owned_cancel(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "bridge.jsonl"
            row = {"sut_outcome": "INCOMPLETE", "artifacts": {"root": str(root), "bridge": path.name}}
            path.write_text(json.dumps({"event": {"type": "ended", "hadConnected": False,
                "outcome": "initial_failure", "detail": "Selected display is unavailable or ambiguous. Reselect displays in the launcher."}}) + "\n")
            actual = REPORT.bridge_end_evidence(row)
            self.assertEqual(actual["classification"], "selected_display_unavailable")
            self.assertFalse(actual["event"]["hadConnected"])
            path.write_text(json.dumps({"event": {"type": "ended", "hadConnected": True,
                "outcome": "cancelled", "detail": "Success."}}) + "\n")
            actual = REPORT.bridge_end_evidence(row)
            self.assertEqual(actual["classification"], "owned_cancellation_after_connection")
            self.assertNotEqual(actual["classification"], "selected_display_unavailable")
            self.assertFalse(REPORT.bridge_end_evidence({"sut_outcome": "INCOMPLETE"})["available"])

    def test_interrupted_plan_rows_keep_the_campaign_phase_and_start_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = envelope("text-rerun")
            source["trials"] = [{"trial_id": "interrupted", "variant": "candidate", "seed": 71,
                                 "validity": "INCOMPLETE", "sut_outcome": "INCOMPLETE",
                                 "metrics": {}, "probes": {}, "reasons": ["planned trial has no completed summary"]},
                                {"trial_id": "not-started", "variant": "reference", "seed": 101,
                                 "validity": "INCOMPLETE", "sut_outcome": "INCOMPLETE",
                                 "metrics": {}, "probes": {}, "reasons": ["planned trial has no completed summary"]}]
            folder = root / "interrupted"
            folder.mkdir()
            (folder / "timeline.jsonl").write_text('{"type":"owned_client_cancel"}\n')
            path = self.write(root, "live-results.json", source)
            result = REPORT.combine([path])
            self.assertEqual(result["preferred_phase"], "text-rerun")
            self.assertEqual({row["phase"] for row in result["trials"]}, {"text-rerun"})
            self.assertEqual(result["counts"]["planned_without_summary"], 2)
            self.assertEqual(result["trials"][0]["completion_evidence"]["status"], "interrupted_attempt_without_summary")
            self.assertEqual(result["trials"][1]["completion_evidence"]["status"], "no_recorded_start")
            self.assertEqual(result["trials"][0]["metrics"], {})

    def test_candidate_only_phase_never_inherits_older_reference_comparisons(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = self.write(root, "old.json", envelope("main"))
            source = envelope("candidate-baseline")
            source["trials"] = [row for row in source["trials"] if row["variant"] == "candidate"]
            candidate = self.write(root, "candidate.json", source)
            result = REPORT.combine([old, candidate])
            self.assertEqual(result["preferred_phase"], "candidate-baseline")
            self.assertTrue(any(c["phase"] == "main" for c in result["comparisons"]))
            self.assertFalse(any(c["phase"] == "candidate-baseline" for c in result["comparisons"]))

    def test_coverage_uses_explicit_ids_without_rewriting_phases_or_pooling(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = envelope("baseline")
            source["trials"] = []
            for seed, p95, count, phase in ((11, 1, 1000000, "baseline"), (29, 100, 1, "baseline"), (47, 3, 2, "retry")):
                row = envelope(phase, seed)["trials"][1]
                row["trial_id"] = f"candidate-{seed}"
                row["metrics"].update({"input_visible_ms.p95": p95, "input_visible_ms.p99": p95 * 2,
                                       "input_visible_ms.count": count})
                source["trials"].append(row)
            path = self.write(root, "live.json", source)
            selection = self.write(root, "coverage.json", {"scope": "Helper versions differ; provisional",
                "selected_trial_ids": ["candidate-11", "candidate-29", "candidate-47"],
                "expected": 6, "observed": 6, "complete": 3, "incomplete": 0,
                "groups": [{"profile": "ten", "seeds": [11, 29, 47], "N": 3,
                            "median_trial_p95_ms": 3, "median_trial_p99_ms": 6}]})
            result = REPORT.combine([path], coverage=selection)
            self.assertEqual([row["phase"] for row in result["trials"]], ["baseline", "baseline", "retry"])
            self.assertTrue(all(row["strict_eligible"] is False for row in result["trials"]))
            group = result["coverage"]["groups"][0]
            self.assertEqual(group["metrics"]["input_visible_ms.p95"]["median"], 3)
            self.assertEqual(group["metrics"]["verified_goodput_mbps"]["n"], 0)
            self.assertEqual(result["coverage"]["counts"]["strict_eligible"], 0)

    def test_coverage_rejects_unknown_ids_duplicate_conditions_and_false_totals(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = envelope()
            source["trials"] = [source["trials"][1]]
            path = self.write(root, "live.json", source)
            selected = {"selected_trial_ids": ["smoke-candidate"], "observed": 3}
            coverage = self.write(root, "coverage.json", selected)
            with self.assertRaisesRegex(ValueError, "observed claim differs"):
                REPORT.combine([path], coverage=coverage)
            selected = {"selected_trial_ids": ["absent"]}
            coverage.write_text(json.dumps(selected))
            with self.assertRaisesRegex(ValueError, "absent from the imported"):
                REPORT.combine([path], coverage=coverage)
            duplicate = dict(source["trials"][0], trial_id="repeated-condition")
            source["trials"].append(duplicate)
            path.write_text(json.dumps(source))
            coverage.write_text(json.dumps({"selected_trial_ids": ["smoke-candidate", "repeated-condition"]}))
            with self.assertRaisesRegex(ValueError, "unique candidate"):
                REPORT.combine([path], coverage=coverage)


if __name__ == "__main__":
    unittest.main()
