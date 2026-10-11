"""Synthetic sealed artifacts verify ACK ledger and conservative report audits."""
import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import ack_timing_report as reporter
from rdpbench.common import read_json, seal, timing_summary, write_json
from rdpbench.feedback import KINDS


def event(direction="s2c", delay=10, overshoot=.25):
    network = 1_000_000_000
    return {"direction": direction, "kind": "ackvec_only", "udp_bytes": 14,
            "network_due_ns": network, "feedback_due_ns": network + int(delay * 1e6),
            "actual_dispatch_ns": network + int((delay + overshoot) * 1e6)}


def feedback_summary(records, delay):
    def timing(values):
        return dict(timing_summary(values), sample_count=len(values), delivered_count=len(values), sampled=False)
    classification = {kind: sum(row["kind"] == kind for row in records) for kind in KINDS}
    return {"requested_delay_ms": delay, "delivered_datagrams": len(records),
            "classified_datagrams": len(records), "selected_datagrams": len(records),
            "scheduled_datagrams": len(records), "classification": classification,
            "ledger_events": len(records), "ledger_overflow_datagrams": 0,
            "added_delivery_ms": timing([max(0, row["actual_dispatch_ns"] - row["network_due_ns"]) / 1e6 for row in records]),
            "scheduler_overshoot_ms": timing([max(0, row["actual_dispatch_ns"] - row["network_due_ns"] - int(delay * 1e6)) / 1e6 for row in records])}


class AckTimingReportTests(unittest.TestCase):
    treatment = {"c2s_delay_ms": 0, "s2c_delay_ms": 10}

    def ledger(self, folder, records):
        (Path(folder) / "ack-feedback.jsonl").write_text("".join(json.dumps(row) + "\n" for row in records), encoding="utf-8")
        shaper = {direction: {"ack_feedback": feedback_summary([row for row in records if row["direction"] == direction],
                                                               self.treatment[direction + "_delay_ms"]),
                              "received_datagrams": sum(row["direction"] == direction for row in records),
                              "delivered_datagrams": sum(row["direction"] == direction for row in records)}
                  for direction in ("c2s", "s2c")}
        return dict(shaper, pending_on_close=0)

    def trial(self, root, ident, variant, *, outcome="COMPLETE", key="same", seed=11, delay=10,
              validity="PROVISIONAL_UNCALIBRATED", reasons=None, metrics=None):
        folder = Path(root) / "ack-timing" / ident
        folder.mkdir(parents=True)
        treatment = {"c2s_delay_ms": 0, "s2c_delay_ms": delay}
        records = [event(delay=delay)]
        self.treatment = treatment
        shaper = self.ledger(folder, records)
        summary = {"trial_id": ident, "case_id": "clean-upload-mixed-ack10", "variant": variant,
                   "comparison_key": key, "seed": seed, "validity": validity, "reasons": reasons or [],
                   "sut_outcome": outcome, "integrity_verified": True,
                   "sut_failures": ["probe deadlines unserved/unobserved"] if outcome == "FAIL" else [],
                   "metrics": metrics or {"c2s.verified_goodput_mbps": 8.0, "client.probe_rtt_ms.p95": 20.0,
                                          "client.probe_rtt_ms.p99": 25.0, "client.probes_expected": 100,
                                          "client.probes_injected": 99, "client.probes_observed": 99,
                                          "client.probes_not_injected": 1, "client.probe_timeouts": 0}}
        write_json(folder / "summary.json", summary)
        write_json(folder / "manifest.json", {"trial_id": ident, "profile_id": "ten_mbit_clean",
                   "effective_variant_environment": {"RDP_BENCH_SEND_QUEUE_BYTES": str(int(variant[1:]) * 1024)},
                   "invariants": {"case": {"id": summary["case_id"], "workload": "upload_mixed"},
                                  "ack_feedback": treatment}})
        write_json(folder / "shaper.json", shaper)
        seal(folder)
        return summary

    def campaign(self, root, rows, seeds=(11,)):
        write_json(Path(root) / "ack-timing" / "campaign.json", {"trials": rows})
        write_json(Path(root) / "ack-timing-suite.json", {"cases": [{"id": "clean-upload-mixed-ack10"}], "seeds": list(seeds)})

    def test_ledger_recomputes_count_deadlines_and_timing_percentiles(self):
        with tempfile.TemporaryDirectory() as folder:
            shaper = self.ledger(folder, [event(overshoot=.1), event(overshoot=.5)])
            result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
            self.assertTrue(result["valid"], result["reasons"])
            self.assertEqual(result["directions"]["s2c"]["computed_timings"]["added_delivery_ms"]["p99"], 10.5)
            shaper["s2c"]["ack_feedback"]["delivered_datagrams"] = 3
            shaper["s2c"]["ack_feedback"]["scheduler_overshoot_ms"]["p99"] = 0
            result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
            self.assertFalse(result["valid"])
            self.assertTrue(any("delivered_datagrams" in reason for reason in result["reasons"]))
            self.assertTrue(any("scheduler_overshoot_ms.p99" in reason for reason in result["reasons"]))

    def test_wrong_offset_and_nonzero_treatment_without_ack_are_invalid(self):
        with tempfile.TemporaryDirectory() as folder:
            row = event()
            row["feedback_due_ns"] += 1
            shaper = self.ledger(folder, [row])
            result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
            self.assertTrue(any("deadline offset" in reason for reason in result["reasons"]))
            shaper = self.ledger(folder, [])
            result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
            self.assertTrue(any("dispatched no selected ACKs" in reason for reason in result["reasons"]))
            zero = {"c2s_delay_ms": 0, "s2c_delay_ms": 0}
            self.treatment = zero
            shaper = self.ledger(folder, [])
            self.assertTrue(reporter.ack_ledger_audit(Path(folder), zero, shaper)["valid"])

    def test_classification_scheduling_and_network_count_inconsistency_is_invalid(self):
        with tempfile.TemporaryDirectory() as folder:
            for mutate, fragment in (
                (lambda shape: shape["s2c"]["ack_feedback"]["classification"].update(ack_only=-1), "classification counters"),
                (lambda shape: shape["s2c"]["ack_feedback"].update(classified_datagrams=2), "classification sum"),
                (lambda shape: shape["s2c"].update(received_datagrams=2), "received_datagrams"),
                (lambda shape: shape["s2c"]["ack_feedback"].update(selected_datagrams=2), "selected_datagrams"),
                (lambda shape: shape["s2c"]["ack_feedback"].update(scheduled_datagrams=0), "scheduled_datagrams"),
                (lambda shape: shape["s2c"]["ack_feedback"].update(scheduled_datagrams=True), "counters"),
            ):
                shaper = self.ledger(folder, [event()])
                mutate(shaper)
                result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
                self.assertFalse(result["valid"])
                self.assertTrue(any(fragment in reason for reason in result["reasons"]), result["reasons"])
            shaper["s2c"] = []
            result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
            self.assertTrue(any("malformed" in reason for reason in result["reasons"]))

    def test_loss_duplicate_and_undelivered_scheduling_bounds(self):
        with tempfile.TemporaryDirectory() as folder:
            # One selected arrival can schedule two copies when duplication is observed.
            shaper = self.ledger(folder, [event(), event(overshoot=.5)])
            feedback = shaper["s2c"]["ack_feedback"]
            feedback.update(classified_datagrams=1, selected_datagrams=1)
            feedback["classification"]["ackvec_only"] = 1
            shaper["s2c"].update(received_datagrams=1, duplicates=1)
            self.assertTrue(reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)["valid"])
            # Two selected arrivals with one actual link drop can schedule one copy.
            shaper = self.ledger(folder, [event()])
            feedback = shaper["s2c"]["ack_feedback"]
            feedback.update(classified_datagrams=2, selected_datagrams=2)
            feedback["classification"]["ackvec_only"] = 2
            shaper["s2c"].update(received_datagrams=2, random_drops=1)
            self.assertTrue(reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)["valid"])
            # An undelivered scheduled ACK must be explained by pending/error bounds.
            shaper["s2c"].update(random_drops=0)
            feedback["scheduled_datagrams"] = 2
            result = reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)
            self.assertTrue(any("pending/socket-error" in reason for reason in result["reasons"]))
            shaper["pending_on_close"] = 1
            self.assertTrue(reporter.ack_ledger_audit(Path(folder), self.treatment, shaper)["valid"])

    def test_failures_remain_diagnostic_and_invalid_ack_pair_is_excluded(self):
        with tempfile.TemporaryDirectory() as root:
            rows = [self.trial(root, "q48-11", "q48"), self.trial(root, "q64-11", "q64", outcome="FAIL")]
            self.campaign(root, rows)
            result = reporter.build_report(root, ["ack-timing"])
            counts = result["phases"][0]["counts"]
            self.assertEqual(counts["artifact_intact_timing_valid_complete_trials"], 1)
            self.assertEqual(counts["sut_failed_trials"], 1)
            metric = [row for row in result["comparisons"] if row["metric"] == "c2s.verified_goodput_mbps"]
            self.assertEqual(len(next(row for row in metric if row["subset"] == "all_observed_diagnostic")["pairs"]), 1)
            self.assertEqual(len(next(row for row in metric if row["subset"] == "artifact_intact_timing_valid_complete")["excluded_pairs"]), 1)
            group = next(row for row in result["groups"] if row["variant"] == "q48")
            totals = group["subsets"]["all_observed_diagnostic"]["counter_totals"]
            self.assertEqual(totals["client.probes_not_injected"]["sum"], 1)
            # Reseal an internally inconsistent ledger: SHA audit passes, timing audit must fail.
            folder = Path(root) / "ack-timing/q48-11"
            shape = read_json(folder / "shaper.json")
            shape["s2c"]["ack_feedback"]["added_delivery_ms"]["p99"] = 0
            write_json(folder / "shaper.json", shape)
            seal(folder)
            result = reporter.build_report(root, ["ack-timing"])
            self.assertEqual(result["phases"][0]["counts"]["ack_ledger_invalid_trials"], 1)
            self.assertFalse(next(row for row in result["trials"] if row["variant"] == "q48")["artifact_intact_timing_valid_complete"])

    def test_tamper_and_missing_trials_are_retained_and_metrics_withheld(self):
        with tempfile.TemporaryDirectory() as root:
            first = self.trial(root, "present", "q48", validity="INVALID_HARNESS", reasons=["relay late"])
            (Path(root) / "ack-timing/present/extra").write_text("unsealed")
            missing = dict(first, trial_id="missing", variant="q64")
            self.campaign(root, [first, missing], seeds=(11, 29))
            result = reporter.build_report(root, ["ack-timing"])
            phase = result["phases"][0]
            self.assertEqual(phase["counts"]["expected_trials"], 4)
            self.assertEqual(phase["counts"]["present_trial_artifacts"], 1)
            self.assertEqual(phase["counts"]["invalid_artifact_trials"], 2)
            self.assertEqual(len(phase["missing_expected_slots"]), 2)
            self.assertEqual(phase["missing_trial_artifacts"], ["missing"])
            present = next(row for row in result["trials"] if row["trial_id"] == "present")
            self.assertEqual(present["harness_reasons"], ["relay late"])
            self.assertIn("unsealed: extra", present["artifact_reasons"])
            self.assertIsNone(present["metrics"]["c2s.verified_goodput_mbps"])
            self.assertEqual(next(row for row in result["groups"] if row["variant"] == "q48")["subsets"]["all_observed_diagnostic"]["trial_count"], 1)

    def test_portable_exports_and_sealed_campaign_write_guard(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as output:
            rows = [self.trial(root, "q48", "q48"), self.trial(root, "q64", "q64", key="changed")]
            self.campaign(root, rows)
            result = reporter.build_report(root, ["ack-timing"])
            self.assertEqual(result["comparisons"], [])
            self.assertEqual(len(result["unpaired"]), 2)
            self.assertFalse(any(Path(row["artifact_directory"]).is_absolute() for row in result["trials"]))
            prefix = Path(output) / "summary"
            reporter.write_report(result, prefix, root)
            self.assertTrue(prefix.with_suffix(".json").is_file())
            self.assertIn("never pooled", prefix.with_suffix(".md").read_text())
            with prefix.with_suffix(".csv").open(newline="") as stream:
                self.assertTrue(list(csv.DictReader(stream)))
            with self.assertRaisesRegex(ValueError, "outside every sealed campaign"):
                reporter.write_report(result, Path(root) / "ack-timing/report", root)

    def test_output_guard_protects_configs_sources_binaries_and_symlink_targets(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as external:
            root = Path(root)
            external = Path(external)
            result = reporter.build_report(root, ["ack-timing"])
            binaries = external / "frozen-binaries"
            binaries.mkdir()
            library = binaries / "native.dylib"
            library.write_bytes(b"native fixture")
            snapshot = external / "external-frozen-source"
            snapshot.mkdir()
            write_json(root / "ack-sweep-plan.json", {"library": str(library),
                       "build_metadata": {"campaign_source_snapshot": str(snapshot)}})
            for prefix in (root / "ack-sweep-plan", root / "ack-sweep-status", root / "variants",
                           root / "peer-env", root / "baseline-suite", root / "source/new-report",
                           root / "relay-calibration/new-report", binaries / "new-report", snapshot / "new-report",
                           ROOT / "configs/new-report", ROOT / "README"):
                with self.assertRaisesRegex(ValueError, "provenance, source, binary or calibration inputs"):
                    reporter.write_report(result, prefix, root)
            prefix = external / "report"
            prefix.with_suffix(".json").symlink_to(root / "ack-sweep-plan.json")
            before = (root / "ack-sweep-plan.json").read_bytes()
            with self.assertRaisesRegex(ValueError, "provenance, source, binary or calibration inputs"):
                reporter.write_report(result, prefix, root)
            self.assertEqual((root / "ack-sweep-plan.json").read_bytes(), before)

    def test_cmake_source_install_and_include_options_allow_campaign_root_output(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            write_json(root / "ack-sweep-plan.json", {"build_metadata": {"build_options": {
                "FreeRDP_SOURCE_DIR:STATIC": str(root.parent), "CMAKE_INSTALL_PREFIX:PATH": str(root),
                "FREERDP_INCLUDE_DIR:PATH": str(root.parent)}}})
            result = reporter.build_report(root, ["ack-timing"])
            prefix = root / "analysis"
            reporter.write_report(result, prefix, root)
            self.assertTrue(prefix.with_suffix(".json").is_file())


if __name__ == "__main__":
    unittest.main()
