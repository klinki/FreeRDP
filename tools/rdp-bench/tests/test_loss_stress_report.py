"""Audited high-loss reporting keeps failures and distinguishes censored probes."""
import copy
import csv
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from loss_stress_report import collect, export, main as report_main
from rdpbench.common import audit, read_json, seal, sha256, write_json


class LossStressReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "campaign"
        self.root.mkdir()
        self.declarations = []

    @staticmethod
    def endpoint(index, expected=100, observed=100, unserved=0, unobserved=0, timeouts=0,
                 p95=10, p99=11):
        a, b = hashlib.sha256(b"a" * 1024).hexdigest(), hashlib.sha256(b"b" * 128).hexdigest()
        count = observed if index == 0 else 0
        percentile = dict(count=count, p50=p95, p95=p95, p99=p99, max=p99)
        return dict(role="bulk_probe" if index == 0 else "sink", completed=True,
                    error=None, terminal=False,
                    counters=dict(sent_bytes=1024 if index == 0 else 128,
                                  received_bytes=128 if index == 0 else 1024,
                                  measured_bytes=128 if index == 0 else 1024,
                                  probes_expected=expected if index == 0 else 0,
                                  probes_injected=expected-unserved if index == 0 else 0,
                                  probes_observed=observed if index == 0 else 0,
                                  probes_not_injected=unserved if index == 0 else 0,
                                  probe_send_failures=0),
                    sent_sha256=a if index == 0 else b, received_sha256=b if index == 0 else a,
                    native_stats=dict(send_failed=0, receive_failed=0, sent_datagrams=100),
                    probe_timeouts=timeouts if index == 0 else 0,
                    probes_pending_at_exit=unobserved if index == 0 else 0,
                    probe_unobserved=unobserved if index == 0 else 0,
                    probe_rtt_ms=percentile, probe_scheduled_ms=dict(percentile),
                    measurement_observed_s=30,
                    latency_scope="observed measured responses; survivor-only",
                    incomplete_record_bytes=0, goodput_mbps=2.5,
                    queue_stats=dict(valid=True, fields=dict(queue_limit_bytes=32768,
                                                            own_log_window=5, peer_log_window=15,
                                                            effective_window_packets=31)))

    def add_trial(self, ident="trial", outcome="COMPLETE", validity="PROVISIONAL_UNCALIBRATED",
                  integrity=True, endpoints=None, p95=10, p99=11, goodput=2.5, shape=None,
                  sut_failures=None):
        folder = self.root / ident
        folder.mkdir()
        profile = read_json(ROOT / "configs/profiles.json")["profiles"]["ten_mbit_loss25"]
        summary = dict(trial_id=ident, case_id="upload-loss25", seed=11,
                       variant="candidate", binary_sha256="ab" * 32, commit="fixture",
                       profile_id="ten_mbit_loss25", loss_probability=0.25,
                       configured_loss={"c2s": 0.25, "s2c": 0.25},
                       sut_outcome=outcome, validity=validity, integrity_verified=integrity,
                       reasons=[], sut_failures=["native transport failed"] if outcome == "FAIL" else [],
                       endpoint_termination=[], telemetry_notes=[],
                       metrics={"c2s.verified_goodput_mbps": goodput,
                                "client.probe_rtt_ms.p95": p95, "client.probe_rtt_ms.p99": p99})
        if sut_failures is not None:
            summary['sut_failures'] = sut_failures
        manifest = dict(profile_id="ten_mbit_loss25", profile=profile,
                        invariants=dict(warmup_s=5, measure_s=30, drain_s=12, payload_bytes=16384,
                                        probe_hz=20, peer_sha256="cd" * 32, peer_environment={}))
        default_direction = dict(received_datagrams=100, random_drops=25, queue_drops=0,
                                 blackout_drops=0, delivered_datagrams=75, socket_send_errors=0,
                                 duplicates=0, lateness_ms={"p99": 0.1})
        if shape is None:
            shape = dict(engine="python_udp_relay_v1", error=None, pending_on_close=0,
                         c2s=dict(default_direction), s2c=dict(default_direction))
        for name, value in (("summary.json", summary), ("manifest.json", manifest), ("shaper.json", shape)):
            write_json(folder / name, value)
        endpoints = endpoints or [self.endpoint(0, p95=p95, p99=p99), self.endpoint(1)]
        for index, endpoint in enumerate(endpoints):
            write_json(folder / f"endpoint-{index}.result.json", endpoint)
        seal(folder)
        self.declarations.append(copy.deepcopy(summary))
        write_json(self.root / "campaign.json", dict(version=1, trials=self.declarations))
        return folder

    def test_native_failure_keeps_survivor_latency_and_distinct_unobserved_counts(self):
        self.add_trial("complete", p95=10, p99=11)
        failed = self.endpoint(0, expected=10, observed=1, unserved=2, unobserved=7,
                               p95=100, p99=200)
        failed.update(terminal=True, error="production send failed: retry budget exhausted",
                      measurement_observed_s=2, probe_timeouts=0)
        failed["native_stats"]["send_failed"] = 1
        peer = self.endpoint(1)
        peer["counters"]["received_bytes"] = 512
        self.add_trial("failed", outcome="FAIL", integrity=False,
                       endpoints=[failed, peer], p95=100, p99=200, goodput=None)
        report = collect(self.root)
        self.assertEqual(report["counts"]["outcomes"], {"COMPLETE": 1, "FAIL": 1})
        group = report["groups"][0]
        self.assertEqual(group["native_terminal_failures"], 1)
        self.assertEqual(group["observed_trial_p95_ms"], dict(n=2, median=55, min=10, max=100))
        self.assertEqual(group["observed_trial_p99_ms"], dict(n=2, median=105.5, min=11, max=200))
        # One hundred good responses do not give that trial greater percentile weight.
        self.assertEqual(group["probes"]["probes_observed"], 101)
        self.assertEqual(group["probes"]["probes_not_injected"], 2)
        self.assertEqual(group["probes"]["unobserved"], 7)
        self.assertEqual(group["probes"]["timeouts"], 0)
        self.assertEqual(group["verified_goodput_mbps"]["n"], 1)
        self.assertEqual(group["verified_goodput_mbps"]["median"], 2.5)
        failure = next(row for row in report["trials"] if row["trial_id"] == "failed")
        self.assertIsNone(failure["verified_goodput_mbps"])
        self.assertEqual(failure["endpoints"][0]["measurement_observed_s"], 2)
        self.assertEqual(failure["endpoints"][0]["probe_rtt_ms"]["count"], 1)

    def test_deadline_timeout_count_is_not_replaced_by_all_unobserved_responses(self):
        early = self.endpoint(0, expected=10, observed=2, unobserved=8, timeouts=0)
        drained = self.endpoint(0, expected=10, observed=7, unobserved=3, timeouts=3)
        self.add_trial("early", outcome="FAIL", integrity=False, endpoints=[early, self.endpoint(1)])
        self.add_trial("drained", outcome="FAIL", integrity=False, endpoints=[drained, self.endpoint(1)])
        report = collect(self.root)
        probes = report["groups"][0]["probes"]
        self.assertEqual(probes["unobserved"], 11)
        self.assertEqual(probes["timeouts"], 3)
        self.assertEqual(probes["probes_observed"], 9)

    def test_failed_transfer_goodput_is_unavailable_in_json_csv_and_group(self):
        endpoint = self.endpoint(0, expected=100, observed=99, unserved=1)
        self.add_trial(outcome="FAIL", integrity=True, endpoints=[endpoint, self.endpoint(1)], goodput=99)
        report = collect(self.root)
        self.assertEqual(report["counts"]["artifact_invalid"], 0)
        self.assertIsNone(report["trials"][0]["verified_goodput_mbps"])
        self.assertEqual(report["groups"][0]["verified_goodput_mbps"]["n"], 0)
        output = Path(self.temporary.name) / "report"
        export(report, output)
        with output.with_suffix(".csv").open(newline="") as stream:
            row = next(csv.DictReader(stream))
        self.assertEqual(row["verified_goodput_mbps"], "")
        self.assertEqual(row["outcome"], "FAIL")

    def test_probe_only_failure_retains_payload_goodput_without_promoting_outcome(self):
        endpoint = self.endpoint(0, expected=100, observed=99, unserved=1)
        failures = ['probe deadlines unserved/unobserved']
        self.add_trial(outcome='FAIL', integrity=True, endpoints=[endpoint, self.endpoint(1)],
                       goodput=3.5, sut_failures=failures)
        report = collect(self.root)
        row = report['trials'][0]
        self.assertEqual(row['sut_outcome'], 'FAIL')
        self.assertEqual(row['sut_failures'], failures)
        self.assertIsNone(row['verified_goodput_mbps'])
        self.assertEqual(row['integrity_verified_goodput_mbps'], 3.5)
        self.assertEqual(report['groups'][0]['verified_goodput_mbps']['n'], 0)
        self.assertEqual(report['groups'][0]['integrity_verified_goodput_mbps'],
                         dict(n=1, median=3.5, min=3.5, max=3.5))
        output = Path(self.temporary.name) / 'report'
        export(report, output)
        with output.with_suffix('.csv').open(newline='') as stream:
            csv_row = next(csv.DictReader(stream))
        self.assertEqual(csv_row['verified_goodput_mbps'], '')
        self.assertEqual(csv_row['integrity_verified_goodput_mbps'], '3.5')
        self.assertEqual(csv_row['outcome'], 'FAIL')
        markdown = output.with_suffix('.md').read_text()
        self.assertIn('Integrity-verified payload goodput', markdown)
        self.assertIn('probe deadlines unserved/unobserved', markdown)

    def test_incomplete_probe_failure_never_has_payload_goodput(self):
        endpoint = self.endpoint(0, expected=100, observed=50, unobserved=50)
        endpoint.update(completed=False, error='production send failed: retry budget exhausted')
        self.add_trial(outcome='FAIL', integrity=False, endpoints=[endpoint, self.endpoint(1)],
                       goodput=99, sut_failures=['probe deadlines unserved/unobserved'])
        report = collect(self.root)
        self.assertIsNone(report['trials'][0]['integrity_verified_goodput_mbps'])
        self.assertEqual(report['groups'][0]['integrity_verified_goodput_mbps']['n'], 0)

    def test_payload_goodput_rejects_bad_timing_native_flags_and_other_failures(self):
        failures = ['probe deadlines unserved/unobserved']
        self.add_trial('harness_invalid', outcome='FAIL', validity='INVALID_HARNESS',
                       sut_failures=failures)
        endpoint = self.endpoint(0)
        endpoint['native_stats']['send_failed'] = 1
        self.add_trial('native_flag', outcome='FAIL', endpoints=[endpoint, self.endpoint(1)],
                       sut_failures=failures)
        self.add_trial('other_failure', outcome='FAIL', sut_failures=['native transport failed'])
        direction = dict(received_datagrams=100, random_drops=25, queue_drops=0,
                         blackout_drops=0, delivered_datagrams=75, socket_send_errors=0,
                         duplicates=0, lateness_ms={'p99': 2.0})
        self.add_trial('bad_dispatch', outcome='FAIL', sut_failures=failures,
                       shape=dict(pending_on_close=0, c2s=direction, s2c=dict(direction)))
        report = collect(self.root)
        self.assertTrue(all(row['integrity_verified_goodput_mbps'] is None
                            for row in report['trials']))
        self.assertEqual(report['groups'][0]['integrity_verified_goodput_mbps']['n'], 0)

    def test_random_drop_denominator_excludes_queue_and_blackout_drops(self):
        direction = dict(received_datagrams=100, random_drops=35, queue_drops=20,
                         blackout_drops=10, delivered_datagrams=35, socket_send_errors=0,
                         duplicates=0, lateness_ms={"p99": 0.1})
        self.add_trial(shape=dict(pending_on_close=0, c2s=direction, s2c=dict(direction)))
        report = collect(self.root)
        self.assertEqual(report["counts"]["artifact_invalid"], 0)
        for direction in ("c2s", "s2c"):
            observed = report["trials"][0]["network"][direction]
            self.assertEqual(observed["offered_datagrams"], 100)
            self.assertEqual(observed["random_loss_candidates"], 70)
            self.assertEqual(observed["realized_random_loss"], 0.5)
            self.assertEqual(report["groups"][0]["unexpected_queue_drops"][direction], 20)

    def test_tampered_artifact_withholds_metrics_and_keeps_declared_condition(self):
        folder = self.add_trial()
        summary = read_json(folder / "summary.json")
        summary["metrics"]["client.probe_rtt_ms.p99"] = 0
        write_json(folder / "summary.json", summary)
        report = collect(self.root)
        self.assert_invalid_condition_retained(report)
        self.assertIn("summary.json", report["trials"][0]["artifact_errors"][0])

    def test_missing_trial_withholds_metrics_and_keeps_declared_condition(self):
        self.add_trial()
        self.declarations[0]["trial_id"] = "missing"
        write_json(self.root / "campaign.json", dict(version=1, trials=self.declarations))
        report = collect(self.root)
        self.assert_invalid_condition_retained(report)
        self.assertEqual(report["trials"][0]["trial_id"], "missing")

    def test_sealed_malformed_endpoint_withholds_all_trial_metrics(self):
        folder = self.add_trial()
        write_json(folder / "endpoint-0.result.json", ["not an endpoint object"])
        seal(folder)
        report = collect(self.root)
        self.assert_invalid_condition_retained(report)

    def test_campaign_declaration_mismatch_does_not_use_sealed_metrics(self):
        self.add_trial()
        self.declarations[0]["seed"] = 999
        write_json(self.root / "campaign.json", dict(version=1, trials=self.declarations))
        report = collect(self.root)
        self.assert_invalid_condition_retained(report, seed=999)
        self.assertIn("declaration", report["trials"][0]["artifact_errors"][0])

    def test_complete_transfer_with_mismatched_hashes_is_not_integrity_verified(self):
        endpoints = [self.endpoint(0), self.endpoint(1)]
        endpoints[1]["received_sha256"] = "f" * 64
        self.add_trial(endpoints=endpoints)
        report = collect(self.root)
        self.assert_invalid_condition_retained(report)

    def test_complete_transfer_without_endpoint_hashes_cannot_verify_goodput(self):
        endpoints = [self.endpoint(0), self.endpoint(1)]
        for endpoint in endpoints:
            del endpoint["sent_sha256"]
            del endpoint["received_sha256"]
        self.add_trial(endpoints=endpoints)
        report = collect(self.root)
        self.assert_invalid_condition_retained(report)

    def test_pending_relay_copies_do_not_justify_negative_loss_candidates(self):
        direction = dict(received_datagrams=100, random_drops=0, queue_drops=60,
                         blackout_drops=50, delivered_datagrams=0, socket_send_errors=0,
                         duplicates=0, lateness_ms={"p99": None})
        self.add_trial(shape=dict(pending_on_close=11, c2s=direction, s2c=dict(direction)))
        report = collect(self.root)
        self.assert_invalid_condition_retained(report)

    def test_cli_cannot_overwrite_campaign_or_sealed_trial(self):
        folder = self.add_trial()
        original = sha256(self.root / "campaign.json")
        for output in (self.root / "campaign", folder / "report"):
            with self.subTest(output=output):
                with patch.object(sys, "argv", ["loss_stress_report.py", "--root", str(self.root),
                                                "--output", str(output)]), \
                     patch("sys.stderr"):
                    with self.assertRaises(SystemExit) as error:
                        report_main()
                    self.assertEqual(error.exception.code, 2)
                self.assertEqual(sha256(self.root / "campaign.json"), original)
                self.assertEqual(audit(folder), [])
                self.assertFalse((folder / "report.json").exists())

    def test_nested_endpoint_schemas_are_withheld_even_with_valid_file_seals(self):
        mutations = [
            ("native_stats", dict(native_stats=["not a statistics object"])),
            ("queue_fields", dict(queue_stats=dict(valid=True, fields=[]))),
            ("queue_summaries", dict(queue_stats=dict(valid=True, summaries=[]))),
            ("queue_summary", dict(queue_stats=dict(valid=True,
                                                    summaries={"residence_ms": []}))),
            ("queue_bounds", dict(queue_stats=dict(valid=True,
                                                   summaries={"residence_ms": {"p95_bounds": [20]}}))),
            ("probe_timing", dict(probe_rtt_ms=[10, 20])),
            ("scheduled_timing", dict(probe_scheduled_ms="not a timing summary")),
        ]
        for label, changes in mutations:
            endpoint = self.endpoint(0)
            endpoint.update(changes)
            self.add_trial(label, endpoints=[endpoint, self.endpoint(1)])
        report = collect(self.root)
        self.assertEqual(report["counts"]["artifact_invalid"], len(mutations))
        self.assertEqual(report["groups"][0]["verified_goodput_mbps"]["n"], 0)
        for row in report["trials"]:
            self.assertEqual(row["validity"], "INVALID_ARTIFACT")
            self.assertEqual(row["metrics"], {})
            self.assertEqual(row["endpoints"], [])
            self.assertIsNone(row["verified_goodput_mbps"])
            self.assertTrue(row["artifact_errors"])

    def test_telemetry_and_cleanup_error_objects_remain_diagnostic_evidence(self):
        telemetry = dict(operation="native_stats", error="RuntimeError('snapshot failed')",
                         observed_ns=1_500_000_000)
        cleanup = dict(operation="driver_close", error="RuntimeError('close failed')")
        endpoint = self.endpoint(0)
        endpoint.update(telemetry_errors=[telemetry], cleanup_errors=[cleanup])
        folder = self.add_trial(validity="INVALID_HARNESS", endpoints=[endpoint, self.endpoint(1)])
        summary = read_json(folder / "summary.json")
        summary["reasons"] = ["endpoint 0 telemetry_errors: " + str(telemetry),
                              "endpoint 0 cleanup_errors: " + str(cleanup)]
        write_json(folder / "summary.json", summary)
        seal(folder)
        report = collect(self.root)
        self.assertEqual(report["counts"]["artifact_invalid"], 0)
        row = report["trials"][0]
        self.assertEqual(row["validity"], "INVALID_HARNESS")
        self.assertEqual(row["endpoints"][0]["telemetry_errors"], [telemetry])
        self.assertEqual(row["endpoints"][0]["cleanup_errors"], [cleanup])
        self.assertEqual(row["reasons"], summary["reasons"])
        self.assertIsNone(row["verified_goodput_mbps"])
        self.assertEqual(report["groups"][0]["timing_valid_complete"], 0)
        self.assertEqual(report["groups"][0]["observed_trial_p99_ms"]["n"], 1)

    def test_declared_profile_mismatch_withholds_metrics_but_retains_declaration(self):
        self.add_trial()
        self.declarations[0]["profile_id"] = "ten_mbit_loss50"
        write_json(self.root / "campaign.json", dict(version=1, trials=self.declarations))
        report = collect(self.root)
        self.assert_invalid_condition_retained(report, profile_id="ten_mbit_loss50")
        self.assertIn("profile", report["trials"][0]["artifact_errors"][0])

    def test_declared_directional_loss_mismatch_withholds_metrics(self):
        self.add_trial()
        loss = {"c2s": 0.25, "s2c": 0.5}
        self.declarations[0]["configured_loss"] = loss
        write_json(self.root / "campaign.json", dict(version=1, trials=self.declarations))
        report = collect(self.root)
        self.assert_invalid_condition_retained(report, configured_loss=loss)
        self.assertIn("loss", report["trials"][0]["artifact_errors"][0])

    def test_multiple_candidate_variants_are_rejected_instead_of_aggregated(self):
        self.add_trial("first")
        folder = self.add_trial("second")
        summary = read_json(folder / "summary.json")
        summary["variant"] = "candidate-other"
        summary["binary_sha256"] = "ef" * 32
        write_json(folder / "summary.json", summary)
        seal(folder)
        self.declarations[1] = copy.deepcopy(summary)
        write_json(self.root / "campaign.json", dict(version=1, trials=self.declarations))
        with self.assertRaisesRegex(ValueError, "candidate|variant"):
            collect(self.root)

    def test_mixed_measurement_settings_for_one_profile_are_rejected(self):
        self.add_trial("first")
        folder = self.add_trial("second")
        manifest = read_json(folder / "manifest.json")
        manifest["invariants"]["measure_s"] = 60
        write_json(folder / "manifest.json", manifest)
        for index in (0, 1):
            endpoint = read_json(folder / f"endpoint-{index}.result.json")
            endpoint["measurement_observed_s"] = 60
            write_json(folder / f"endpoint-{index}.result.json", endpoint)
        seal(folder)
        with self.assertRaisesRegex(ValueError, "configuration|settings|invariant"):
            collect(self.root)

    def test_mixed_fixed_peer_settings_for_one_profile_are_rejected(self):
        self.add_trial("first")
        folder = self.add_trial("second")
        manifest = read_json(folder / "manifest.json")
        manifest["invariants"]["peer_environment"] = {"RDP_BENCH_SEND_QUEUE_BYTES": "65536"}
        write_json(folder / "manifest.json", manifest)
        endpoint = read_json(folder / "endpoint-1.result.json")
        endpoint["queue_stats"]["fields"]["queue_limit_bytes"] = 65536
        write_json(folder / "endpoint-1.result.json", endpoint)
        seal(folder)
        with self.assertRaisesRegex(ValueError, "configuration|settings|invariant"):
            collect(self.root)

    def assert_invalid_condition_retained(self, report, seed=11, profile_id="ten_mbit_loss25",
                                         configured_loss=None):
        self.assertEqual(report["counts"]["trials"], 1)
        self.assertEqual(report["counts"]["artifact_invalid"], 1)
        row = report["trials"][0]
        self.assertEqual(row["validity"], "INVALID_ARTIFACT")
        self.assertEqual(row["case_id"], "upload-loss25")
        self.assertEqual(row["seed"], seed)
        self.assertEqual(row["variant"], "candidate")
        self.assertEqual(row["profile_id"], profile_id)
        self.assertEqual(row["configured_loss"], configured_loss or {"c2s": 0.25, "s2c": 0.25})
        self.assertEqual(row["metrics"], {})
        self.assertEqual(row["endpoints"], [])
        self.assertEqual(row["network"], {})
        self.assertIsNone(row["verified_goodput_mbps"])
        self.assertEqual(report["groups"][0]["verified_goodput_mbps"]["n"], 0)
        self.assertEqual(report["groups"][0]["observed_trial_p99_ms"]["n"], 0)


if __name__ == "__main__":
    unittest.main()
