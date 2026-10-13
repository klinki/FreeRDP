"""Compact reports preserve invalidity, pairing and sparse causal timestamps."""
import csv
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from loss_stall_compact import (PAIR_METRICS, auxiliary_status, build_report,
                               guard_output, trace_summary, write_report)


def trial(variant, seed=29, validity="PROVISIONAL_UNCALIBRATED", outcome="COMPLETE", phase="main", key="same"):
    metrics = {name: 8 if "goodput" in name else 120 for name in PAIR_METRICS}
    metrics.update({"client.probes_expected": 100, "client.probes_injected": 100,
                    "client.probes_observed": 100, "client.probes_not_injected": 0,
                    "client.probe_timeouts": 0, "client.queue_queue_limit_bytes": 49152,
                    "server.queue_queue_limit_bytes": 131072,
                    "client.queue_effective_window_packets": 31, "server.queue_effective_window_packets": 31})
    return {"phase": phase, "trial_id": f"{phase}-{variant}-{seed}", "case_id": "impaired-upload-mixed",
            "profile_id": "ten_mbit", "workload": "upload_mixed", "variant": variant, "seed": seed,
            "comparison_key": key, "validity": validity, "original_validity": validity,
            "sut_outcome": outcome, "integrity_verified": True, "queue_cap_bytes": 49152,
            "commit": "revision", "binary_sha256": "sut-sha", "metrics": metrics,
            "artifact_intact_timing_valid_complete": validity != "INVALID_HARNESS" and outcome == "COMPLETE",
            "invariants": {"peer_sha256": "fixed-peer-sha", "case": {"loss_trace": False}},
            "manifest": {"build_metadata": {"build_options": {"unused": "not repeated"}, "commit": "revision"}},
            "loss_trace": {"enabled": False, "available": False, "valid": None},
            "sut_failures": ["failure independent of missed probe count"] if outcome == "FAIL" else [],
            "harness_reasons": ["relay too late"] if validity == "INVALID_HARNESS" else [],
            "artifact_reasons": ["SHA changed"] if validity == "INVALID_ARTIFACT" else []}


def aggregate(rows):
    return {"version": 1, "scope": "raw native UDP2 transport", "trials": rows,
            "phases": [{"phase": phase, "expected_trials": sum(r["phase"] == phase for r in rows), "complete": True}
                       for phase in sorted({r["phase"] for r in rows})],
            "provenance": {"configurations": {phase: {} for phase in {r["phase"] for r in rows}},
                           "candidate_sha256": "new-sha", "reference_sha256": "old-sha"}, "notes": []}


class CompactTests(unittest.TestCase):
    def report(self, rows):
        return build_report(aggregate(rows), Path("/campaign"))

    def test_hash_invalid_values_are_withheld_even_if_source_boolean_is_wrong(self):
        old = trial("reference-q48", validity="INVALID_ARTIFACT")
        # Model a corrupted retained aggregate containing apparently good numbers:
        # invalid-artifact status takes priority over those numbers and booleans.
        old["artifact_intact_timing_valid_complete"] = True
        report = self.report([old, trial("candidate-q48")])
        for comparison in report["comparisons"]:
            self.assertEqual(comparison["paired_n"], 0)
            self.assertEqual(comparison["metrics"][PAIR_METRICS[0]]["n"], 0)
        saved = report["trials"][0]
        self.assertIsNone(saved["metrics"][PAIR_METRICS[0]])
        self.assertTrue(all(value is None for value in saved["actual_configuration"].values()))
        self.assertFalse(saved["loss_trace"]["valid"])
        self.assertIn("SHA changed", saved["artifact_reasons"])
        self.assertEqual(report["counts"]["artifact_invalid"], 1)

    def test_no_missed_probes_does_not_hide_raw_service_failure(self):
        rows = [trial("reference-q48", outcome="FAIL"), trial("candidate-q48")]
        report = self.report(rows)
        self.assertEqual(report["counts"]["service_failures"], 1)
        self.assertEqual(report["counts"]["probes"]["probes_not_injected"]["sum"], 0)
        self.assertEqual(report["trials"][0]["sut_failures"], ["failure independent of missed probe count"])
        by_subset = {c["subset"]: c for c in report["comparisons"]}
        self.assertEqual(by_subset["all_observed_diagnostic"]["paired_n"], 1)
        self.assertEqual(by_subset["artifact_intact_timing_valid_complete"]["paired_n"], 0)

    def test_matching_needs_same_seed_config_phase_and_trace_audit(self):
        rows = [trial("reference-q48", 11), trial("candidate-q48", 11),
                trial("reference-q48", 29), trial("candidate-q48", 29, key="changed"),
                trial("timer-q48", phase="diagnostic-29"), trial("fast-q48", phase="diagnostic-29")]
        rows[-2]["invariants"]["case"]["loss_trace"] = True
        rows[-2]["loss_trace"] = {"enabled": True, "available": True, "valid": False, "reasons": ["overflow"]}
        report = self.report(rows)
        main = [c for c in report["comparisons"] if c["phase"] == "main"]
        self.assertEqual([c["paired_n"] for c in main], [1, 1])
        self.assertEqual(len(report["unpaired"]), 2)
        diagnostic = {c["subset"]: c for c in report["comparisons"] if c["phase"] == "diagnostic-29"}
        self.assertEqual(diagnostic["all_observed_diagnostic"]["paired_n"], 1)
        self.assertEqual(diagnostic["artifact_intact_timing_valid_complete"]["paired_n"], 0)
        self.assertIn("overflow", report["trials"][-2]["loss_trace"]["reasons"])

    def test_metric_n_does_not_silently_assume_every_pair_has_samples(self):
        rows = [trial("reference-q48", 11), trial("candidate-q48", 11),
                trial("reference-q48", 29), trial("candidate-q48", 29)]
        rows[-1]["metrics"][PAIR_METRICS[3]] = None
        report = self.report(rows)
        strict = next(c for c in report["comparisons"] if c["subset"] == "artifact_intact_timing_valid_complete")
        self.assertEqual(strict["paired_n"], 2)
        self.assertEqual(strict["metrics"][PAIR_METRICS[3]]["n"], 1)
        self.assertEqual(strict["metrics"][PAIR_METRICS[3]]["missing_seeds"], [29])

    def test_sparse_native_proof_keeps_epoch_confounded_timer_and_retirement(self):
        epoch = 987_654_321_000_000
        recovery = {"kind": "retry_timer", "data_seq": 9, "channel_seq": 5,
                    "time_ns": epoch + 200_000_000, "gap_ns": epoch + 40_000_000,
                    "ready_feedback_ns": epoch + 50_000_000, "relay_drop_ns": epoch,
                    "retired_ns": epoch + 230_000_000, "relay_drop_reason": "random_drops",
                    "ready_feedback_to_retry_ms": 150, "retry_to_retire_ms": 30}
        trace = {"enabled": True, "available": True, "valid": True, "reasons": [],
                 "fixed_peer_native_trace": {"available": False, "valid": False, "reasons": ["old peer ABI"]},
                 "attribution": {"recoveries": [recovery], "stalls": [
                     {"call_kind": "bulk", "record_id": 5, "start_ns": epoch + 10_000_000,
                      "end_ns": epoch + 240_000_000, "duration_ns": 230_000_000,
                      "classification": "relay_timing_confounded", "relay_lateness_confound": True,
                      "native_timer_recovery_evidence": [recovery]}], "misses": []}}
        result = trace_summary(trace, Path("/campaign"))
        self.assertEqual(result["sparse_proof_epoch_ns"], epoch)
        self.assertEqual(len(result["recovery_proof"]), 1)  # deduplicated nested proof
        proof = result["recovery_proof"][0]
        self.assertEqual(proof["time_relative_ns"], 200_000_000)
        self.assertEqual(proof["retired_relative_ns"] + result["sparse_proof_epoch_ns"], recovery["retired_ns"])
        span = result["slow_call_spans"][0]
        self.assertEqual(span["duration_ns"], 230_000_000)
        self.assertEqual(span["native_admission_timer_evidence"], [0])
        self.assertEqual(span["classification"], "relay_timing_confounded")
        self.assertFalse(result["fixed_peer_native_trace"]["available"])
        self.assertTrue(any("ACK generation" in interval for interval in result["unknown_intervals"]))
        self.assertNotIn("rows", result)

    def test_invalid_trace_with_plausible_events_cannot_produce_proof(self):
        result = trace_summary({"enabled": True, "valid": False, "reasons": ["clock drift"],
                                "attribution": {"recoveries": [{"time_ns": 100, "ready_feedback_ns": 20}]}}, Path("/campaign"))
        self.assertFalse(result["causal_evidence_available"])
        self.assertNotIn("recovery_proof", result)
        self.assertEqual(result["reasons"], ["clock drift"])

    def test_missed_probe_lateness_is_elapsed_and_does_not_change_sparse_epoch(self):
        epoch = 987_654_321_000_000
        call = {"call_kind": "bulk", "record_id": 17, "start_ns": epoch + 40_000_000,
                "end_ns": epoch + 240_000_000, "duration_ns": 200_000_000}
        miss = {"type": "probe_not_injected", "id": 18, "scheduled_ns": epoch + 30_000_000,
                "observed_ns": epoch + 240_000_001, "lateness_ns": 210_000_001,
                "last_call": call, "last_slow_call": call,
                "classification": "native_admission_timer_recovery",
                "overlapping_call_spans": [{key: call[key] for key in ("start_ns", "end_ns", "call_kind", "record_id")}]}
        result = trace_summary({"enabled": True, "available": True, "valid": True,
                                "attribution": {"recoveries": [], "stalls": [], "misses": [miss]}}, Path("/campaign"))
        saved = result["misses"][0]
        proof_epoch = result["sparse_proof_epoch_ns"]
        self.assertEqual(proof_epoch, epoch + 30_000_000)
        self.assertEqual(saved["lateness_ns"], miss["lateness_ns"])
        self.assertNotIn("lateness_relative_ns", saved)
        self.assertEqual(saved["observed_relative_ns"] + proof_epoch, miss["observed_ns"])
        self.assertEqual(saved["scheduled_relative_ns"] + proof_epoch, miss["scheduled_ns"])
        self.assertEqual(saved["last_call"]["duration_ns"], call["duration_ns"])
        self.assertEqual(saved["last_call"]["start_relative_ns"] + proof_epoch, call["start_ns"])
        self.assertEqual(saved["overlapping_call_spans"][0]["end_relative_ns"] + proof_epoch, call["end_ns"])

    def test_auxiliary_failures_stay_separate_and_preserve_failed_criteria(self):
        calibration = {"pass": False, "approved": False, "failed_samples": ["one"], "samples": [
            {"profile_id": "ten_mbit", "seed": 29, "mode": "paced", "pass": False, "criteria": [
                {"criterion": "lateness", "pass": False, "observed": 5},
                {"criterion": "integrity", "pass": True, "observed": 1}]}]}
        saved = auxiliary_status(calibration, Path("/campaign"))
        sample = next(r for r in saved["failed_records"] if r["path"] == "samples[0]")
        self.assertEqual(len(sample["failed_criteria"]), 1)
        self.assertEqual(saved["failed_sample_count"], 1)
        self.assertFalse(saved["approved"])
        preflight = auxiliary_status({"pass": False, "phases": {"smoke": {
            "complete": True, "expected_trials": 4, "observed_trials": 4, "returncode": 1, "failed_trials": ["bad"]}}}, Path("/campaign"))
        self.assertEqual(preflight["phases"]["smoke"]["failed_trials"], ["bad"])
        self.assertIn("does not audit", preflight["artifact_status"])

    def test_output_guards_inputs_sealed_source_and_external_binary_trees(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binary = root / "source", root / "external-bin"
            source.mkdir(); binary.mkdir()
            native = binary / "native.dylib"; native.write_bytes(b"binary")
            input_path = root / "report" / "aggregate.json"
            input_path.parent.mkdir(); input_path.write_text("{}")
            audited = aggregate([trial("candidate-q48")])
            audited["provenance"].update(candidate_library=str(native), candidate_metadata={"campaign_source_snapshot": str(source)})
            for prefix in (input_path.with_suffix(""), root / "main/new-report", source / "new-report", binary / "new-report"):
                with self.assertRaisesRegex(ValueError, "overwrite|sealed"):
                    guard_output(input_path, prefix, root, audited)
            guard_output(input_path, root / "export/result", root, audited)

    def test_portable_export_input_hash_no_bulk_options_and_lf_csv(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = trial("candidate-q48")
            row["artifact_directory"] = str(root / "main/trial")
            audited = aggregate([row])
            input_path = root / "aggregate.json"
            input_path.write_text(json.dumps(audited))
            report = build_report(audited, root, input_path)
            self.assertEqual(report["provenance"]["input_sha256"], hashlib.sha256(input_path.read_bytes()).hexdigest())
            self.assertEqual(report["trials"][0]["artifact_directory"], "main/trial")
            self.assertEqual(report["trials"][0]["actual_configuration"]["dut_window_packets"], 31)
            self.assertEqual(report["trials"][0]["peer_sha256"], "fixed-peer-sha")
            self.assertNotIn("build_options", json.dumps(report))
            output = root / "result"
            write_report(report, output)
            self.assertNotIn(b"\r\n", output.with_suffix(".csv").read_bytes())
            with output.with_suffix(".csv").open(newline="") as stream:
                self.assertEqual(next(csv.DictReader(stream))["trial_id"], row["trial_id"])
            self.assertEqual(json.loads(output.with_suffix(".json").read_text())["counts"]["trials"], 1)

    def test_dirty_candidate_source_identity_survives_export_beyond_base_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference_source = b"base native transport source"
            candidate_source = b"native transport source with fast loss recovery"
            source_file = "libfreerdp/core/rdpeudp.c"
            reference = {"commit": "same-base-commit", "tracked_dirty": False,
                         "tracked_changes_sha256": hashlib.sha256(b"").hexdigest(),
                         "native_source_sha256": {source_file: hashlib.sha256(reference_source).hexdigest()},
                         "campaign_source_diff_sha256": hashlib.sha256(b"base snapshot diff").hexdigest(),
                         "campaign_source_files": {source_file: hashlib.sha256(reference_source).hexdigest()}}
            candidate = {"commit": "same-base-commit", "tracked_dirty": True,
                         "tracked_changes_sha256": hashlib.sha256(b"native tracked patch").hexdigest(),
                         "native_source_sha256": {source_file: hashlib.sha256(candidate_source).hexdigest()},
                         "campaign_source_diff_sha256": hashlib.sha256(b"candidate snapshot diff").hexdigest(),
                         "campaign_source_files": {source_file: hashlib.sha256(candidate_source).hexdigest()}}
            rows = [trial("reference-q48"), trial("candidate-q48")]
            rows[0]["manifest"]["build_metadata"] = reference
            rows[1]["manifest"]["build_metadata"] = candidate
            audited = aggregate(rows)
            audited["provenance"].update(reference_metadata=reference, candidate_metadata=candidate)
            report = build_report(audited, root)
            output = root / "result"
            write_report(report, output)
            saved = json.loads(output.with_suffix(".json").read_text())
            a, b = (saved["provenance"][f"{name}_metadata"] for name in ("reference", "candidate"))
            self.assertEqual(a["commit"], b["commit"])
            self.assertFalse(a["tracked_dirty"])
            self.assertTrue(b["tracked_dirty"])
            self.assertNotEqual(a["tracked_changes_sha256"], b["tracked_changes_sha256"])
            self.assertNotEqual(a["native_source_sha256"], b["native_source_sha256"])
            self.assertEqual(b["native_source_sha256"][source_file], hashlib.sha256(candidate_source).hexdigest())
            self.assertEqual(b["campaign_source_files"], candidate["campaign_source_files"])
            version = saved["trials"][1]["version"]
            self.assertTrue(version["tracked_dirty"])
            self.assertEqual(version["tracked_changes_sha256"], b["tracked_changes_sha256"])
            self.assertEqual(version["campaign_source_diff_sha256"], b["campaign_source_diff_sha256"])


if __name__ == "__main__":
    unittest.main()
