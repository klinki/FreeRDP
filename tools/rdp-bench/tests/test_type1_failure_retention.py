"""Terminal transport failures remain distinguishable from broken trial artifacts."""
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from rdpbench.common import audit, read_json
from rdpbench.type1 import endpoint_result, native_transport_failed, one_trial


class FakeEndpoint:
    """A ready subprocess with independently controlled exit and timeout outcomes."""
    def __init__(self, returncode=0, timeout=False):
        self.stdin = io.StringIO()
        self.stdout = io.StringIO('{"type":"ready"}\n')
        self.returncode = None
        self.planned_returncode = returncode
        self.timeout = timeout
        self.wait_calls = 0
        self.kill_calls = 0

    def wait(self, timeout=None):
        self.wait_calls += 1
        if self.timeout and self.returncode is None:
            raise subprocess.TimeoutExpired("fixture endpoint", timeout)
        if self.returncode is None:
            self.returncode = self.planned_returncode
        return self.returncode

    def kill(self):
        self.kill_calls += 1
        self.returncode = -9

    def poll(self):
        return self.returncode


class Type1FailureRetentionTests(unittest.TestCase):
    @staticmethod
    def healthy():
        empty_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        return dict(completed=True, error=None, goodput_mbps=0,
                    sent_sha256=empty_hash, received_sha256=empty_hash,
                    counters=dict(sent_bytes=0, received_bytes=0, measured_bytes=0,
                                  probes_expected=0, probes_injected=0, probes_observed=0,
                                  probes_not_injected=0),
                    native_stats=dict(send_failed=0, receive_failed=0, sent_datagrams=1))

    def run_fixture(self, results, outcomes):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        folder = Path(temporary.name) / "trial"
        processes = []

        def create_process(command, **_kwargs):
            index = len(processes)
            endpoint_config = read_json(command[-1])
            result_path = Path(endpoint_config["result"])
            result = results[index]
            if result is not None:
                result_path.write_text(result if isinstance(result, str) else json.dumps(result))
            process = FakeEndpoint(**outcomes[index])
            processes.append(process)
            return process

        config = read_json(ROOT / "configs/type1.loss-stress.json")
        profile = read_json(ROOT / "configs/profiles.json")["profiles"]["ten_mbit_loss25"]
        relay = Mock()
        relay.ports = [12347, 12348]
        relay.close.return_value = dict(engine="python_udp_relay_v1", error=None,
                                       c2s=dict(received_datagrams=10, lateness_ms={"p99": 0.1}),
                                       s2c=dict(received_datagrams=10, lateness_ms={"p99": 0.1}))
        with patch("rdpbench.type1.free_port", side_effect=[12345, 12346]), \
             patch("rdpbench.type1.Relay", return_value=relay), \
             patch("rdpbench.type1.environment", return_value=dict(system="fixture", python="fixture",
                                                                 host="fixture", utc="fixture")), \
             patch("rdpbench.type1.subprocess.Popen", side_effect=create_process), \
             patch("rdpbench.type1.select.select", side_effect=lambda readers, *_: (readers, [], [])), \
             patch("rdpbench.type1.sleep_until"), \
             patch("rdpbench.type1.time.monotonic", return_value=1.0), \
             patch("rdpbench.type1.time.monotonic_ns", return_value=1_000_000_000), \
             patch("rdpbench.calibration.accepted", return_value=(False, ["fixture has no calibration"])):
            summary = one_trial(dict(id="candidate"), config["cases"][1],
                                profile, config, 11, folder)
        self.assertEqual(audit(folder), [])
        self.assertEqual(summary, read_json(folder / "summary.json"))
        relay.start.assert_called_once()
        relay.close.assert_called_once()
        return summary, processes, folder

    def test_terminal_native_failures_preserve_both_exits_and_are_sut_failures(self):
        send_failure = dict(completed=False, error="production send failed: retry budget exhausted",
                            native_stats=dict(send_failed=1, receive_failed=0, sent_datagrams=20),
                            queue_stats=dict(valid=False,
                                             reasons=["measurement cohort bytes remain unobserved after drain"]))
        receive_failure = dict(completed=False, error="production receive failed / unhealthy stream",
                               native_stats=dict(send_failed=0, receive_failed=1, sent_datagrams=10))
        summary, processes, _ = self.run_fixture([send_failure, receive_failure],
                                               [dict(returncode=1), dict(returncode=2)])
        self.assertEqual(summary["sut_outcome"], "FAIL")
        self.assertEqual(summary["validity"], "PROVISIONAL_UNCALIBRATED")
        self.assertEqual(summary["reasons"], [])
        self.assertEqual(summary["endpoint_termination"], [
            dict(endpoint=0, returncode=1, timed_out=False, terminated_by_runner=False),
            dict(endpoint=1, returncode=2, timed_out=False, terminated_by_runner=False)])
        self.assertTrue(any("endpoint 0 exited 1" in reason for reason in summary["sut_failures"]))
        self.assertTrue(any("endpoint 1 exited 2" in reason for reason in summary["sut_failures"]))
        self.assertIn("terminal transport failure", summary["telemetry_notes"][0])
        self.assertEqual(summary["metrics"]["client.native_stats.send_failed"], 1)
        self.assertEqual(summary["metrics"]["server.native_stats.receive_failed"], 1)
        self.assertFalse(summary["integrity_verified"])
        self.assertIsNone(summary["metrics"]["c2s.verified_goodput_mbps"])
        self.assertIsNone(summary["metrics"]["s2c.verified_goodput_mbps"])
        self.assertEqual([process.kill_calls for process in processes], [0, 0])

    def test_benchmark_deadline_retains_incomplete_drain_without_transport_failure(self):
        deadline = dict(
            completed=False,
            error="benchmark deadline reached; reliable drain incomplete",
            benchmark_deadline_reached=True,
            native_stats=dict(send_failed=1, receive_failed=0, sent_datagrams=20),
            queue_stats=dict(valid=False,
                             reasons=["measurement cohort bytes remain unobserved after drain"]),
        )
        self.assertFalse(native_transport_failed(deadline, "native"))
        summary, processes, _ = self.run_fixture([deadline, self.healthy()],
                                                 [dict(returncode=1), dict(returncode=0)])
        self.assertEqual(summary["sut_outcome"], "FAIL")
        self.assertEqual(summary["validity"], "PROVISIONAL_UNCALIBRATED")
        self.assertTrue(summary["benchmark_deadline_reached"])
        self.assertEqual(summary["reasons"], [])
        self.assertTrue(any("benchmark deadline reached" in reason
                            for reason in summary["sut_failures"]))
        self.assertEqual(summary["telemetry_notes"], [
            "queue telemetry: measurement cohort bytes remain unobserved after drain "
            "(benchmark drain deadline)"])
        self.assertEqual(summary["metrics"]["client.native_stats.send_failed"], 1)
        self.assertFalse(summary["integrity_verified"])
        self.assertIsNone(summary["metrics"]["c2s.verified_goodput_mbps"])
        self.assertEqual([process.kill_calls for process in processes], [0, 0])

    def test_deadline_does_not_hide_broken_telemetry_or_artifacts(self):
        deadline = dict(completed=False,
                        error="benchmark deadline reached; reliable drain incomplete",
                        benchmark_deadline_reached=True,
                        native_stats=dict(send_failed=1),
                        queue_stats=dict(valid=False, reasons=["native queue observer invalid"]))
        summary, _, _ = self.run_fixture([deadline, self.healthy()],
                                         [dict(returncode=1), dict(returncode=0)])
        self.assertEqual(summary["validity"], "INVALID_HARNESS")
        self.assertIn("queue telemetry: native queue observer invalid", summary["reasons"])
        summary, _, folder = self.run_fixture([deadline, "{broken JSON"],
                                             [dict(returncode=1), dict(returncode=0)])
        self.assertEqual(summary["validity"], "INVALID_HARNESS")
        self.assertTrue(any("endpoint 1: invalid endpoint result" in reason
                            for reason in summary["reasons"]))
        self.assertEqual((folder / "endpoint-1.result.json").read_text(), "{broken JSON")

    def test_invalid_deadline_scalars_remain_harness_errors(self):
        for value in (1, 0, "true", "false", None, [], {}):
            with self.subTest(value=value):
                broken = dict(completed=False, error="fixture driver failure",
                              benchmark_deadline_reached=value,
                              native_stats=dict(send_failed=1))
                summary, _, folder = self.run_fixture([broken, self.healthy()],
                                                      [dict(returncode=1), dict(returncode=0)])
                self.assertEqual(summary["validity"], "INVALID_HARNESS")
                self.assertEqual(summary["sut_outcome"], "FAIL")
                self.assertFalse(summary["benchmark_deadline_reached"])
                self.assertTrue(any("benchmark_deadline_reached; expected boolean" in reason
                                    for reason in summary["reasons"]))
                self.assertEqual(read_json(folder / "endpoint-0.result.json"), broken)
                self.assertIsNone(summary["metrics"]["c2s.verified_goodput_mbps"])

    def test_two_malformed_result_files_still_seal_failed_row(self):
        summary, _, folder = self.run_fixture(["{broken JSON", "[]"],
                                             [dict(returncode=0), dict(returncode=0)])
        self.assertEqual(summary["validity"], "INVALID_HARNESS")
        self.assertEqual(summary["sut_outcome"], "FAIL")
        self.assertFalse(summary["integrity_verified"])
        for index in range(2):
            self.assertTrue(any(f"endpoint {index}: invalid endpoint result" in reason
                                for reason in summary["reasons"]))
        self.assertIsNone(summary["metrics"]["c2s.verified_goodput_mbps"])
        self.assertIsNone(summary["metrics"]["s2c.verified_goodput_mbps"])
        self.assertEqual((folder / "endpoint-0.result.json").read_text(), "{broken JSON")
        self.assertEqual((folder / "endpoint-1.result.json").read_text(), "[]")

    def test_timeout_keeps_hang_reason_and_runner_termination_marker(self):
        summary, processes, _ = self.run_fixture([None, self.healthy()],
                                               [dict(timeout=True), dict(returncode=0)])
        self.assertEqual(summary["validity"], "INVALID_HARNESS")
        self.assertEqual(summary["sut_outcome"], "FAIL")
        self.assertEqual(summary["endpoint_termination"][0],
                         dict(endpoint=0, returncode=-9, timed_out=True, terminated_by_runner=True))
        self.assertEqual(summary["endpoint_termination"][1],
                         dict(endpoint=1, returncode=0, timed_out=False, terminated_by_runner=False))
        self.assertTrue(any("SUT_OR_DRIVER_HANG" in reason for reason in summary["reasons"]))
        self.assertFalse(any("exited -9" in reason for reason in summary["reasons"]))
        self.assertEqual(processes[0].kill_calls, 1)
        self.assertEqual(processes[0].wait_calls, 2)
        self.assertIsNone(summary["metrics"]["c2s.verified_goodput_mbps"])

    def test_unrecognized_endpoint_exit_stays_harness_failure(self):
        summary, _, _ = self.run_fixture([dict(completed=False, error="fixture driver import failed"),
                                         self.healthy()],
                                        [dict(returncode=3), dict(returncode=0)])
        self.assertEqual(summary["validity"], "INVALID_HARNESS")
        self.assertTrue(any("endpoint 0 exited 3" in reason for reason in summary["reasons"]))
        self.assertEqual(summary["sut_outcome"], "FAIL")

    def test_complete_healthy_fixture_is_integrity_verified(self):
        summary, _, _ = self.run_fixture([self.healthy(), self.healthy()],
                                        [dict(returncode=0), dict(returncode=0)])
        self.assertEqual(summary["sut_outcome"], "COMPLETE")
        self.assertEqual(summary["validity"], "PROVISIONAL_UNCALIBRATED")
        self.assertEqual(summary["reasons"], [])
        self.assertTrue(summary["integrity_verified"])
        self.assertEqual(summary["metrics"]["c2s.verified_goodput_mbps"], 0)

    def test_invalid_scalar_fields_and_complete_hashes_are_read_errors(self):
        healthy = self.healthy()
        invalid_hash = copy.deepcopy(healthy)
        invalid_hash["received_sha256"] = "not a SHA256"
        invalid_counter = copy.deepcopy(healthy)
        invalid_counter["counters"]["sent_bytes"] = True
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "result.json"
            for result in (42, None, [], dict(completed=1), invalid_hash, invalid_counter):
                with self.subTest(result=result):
                    path.write_text(json.dumps(result))
                    parsed = endpoint_result(path)
                    self.assertFalse(parsed["completed"])
                    self.assertTrue(parsed["result_read_error"])
                    self.assertIn("invalid endpoint result", parsed["error"])

    def test_nested_endpoint_schemas_produce_sealed_invalid_rows(self):
        mutations = [
            ("native_stats", dict(native_stats=["not a statistics object"])),
            ("queue_fields", dict(queue_stats=dict(valid=True, fields=[]))),
            ("queue_summaries", dict(queue_stats=dict(valid=True, summaries=[]))),
            ("queue_summary", dict(queue_stats=dict(valid=True,
                                                    summaries={"residence_ms": []}))),
            ("queue_bounds", dict(queue_stats=dict(valid=True,
                                                   summaries={"residence_ms": {"p95_bounds": [20]}}))),
            ("queue_clock", dict(queue_stats=dict(valid=True, clock_alignment=[]))),
            ("probe_timing", dict(probe_rtt_ms=[10, 20])),
            ("scheduled_timing", dict(probe_scheduled_ms="not a timing summary")),
            ("send_timing", dict(send_call_ms=[])),
            ("native_loss_trace", dict(loss_trace=dict(valid=True, native=[]))),
        ]
        for label, changes in mutations:
            with self.subTest(schema=label):
                broken = self.healthy()
                broken.update(changes)
                summary, _, folder = self.run_fixture([broken, self.healthy()],
                                                      [dict(returncode=0), dict(returncode=0)])
                self.assertEqual(summary["validity"], "INVALID_HARNESS")
                self.assertEqual(summary["sut_outcome"], "FAIL")
                self.assertTrue(any("invalid endpoint result" in reason for reason in summary["reasons"]))
                self.assertIsNone(summary["metrics"]["c2s.verified_goodput_mbps"])
                self.assertIsNone(summary["metrics"]["s2c.verified_goodput_mbps"])
                self.assertEqual(read_json(folder / "endpoint-0.result.json"), broken)

    def test_telemetry_and_cleanup_error_objects_are_retained_as_harness_reasons(self):
        telemetry = dict(operation="native_stats", error="RuntimeError('snapshot failed')",
                         observed_ns=1_500_000_000)
        cleanup = dict(operation="driver_close", error="RuntimeError('close failed')")
        endpoint = self.healthy()
        endpoint.update(telemetry_errors=[telemetry], cleanup_errors=[cleanup])
        summary, _, folder = self.run_fixture([endpoint, self.healthy()],
                                              [dict(returncode=0), dict(returncode=0)])
        self.assertEqual(summary["validity"], "INVALID_HARNESS")
        self.assertTrue(any("telemetry_errors" in reason and "snapshot failed" in reason
                            for reason in summary["reasons"]))
        self.assertTrue(any("cleanup_errors" in reason and "close failed" in reason
                            for reason in summary["reasons"]))
        retained = read_json(folder / "endpoint-0.result.json")
        self.assertEqual(retained["telemetry_errors"], [telemetry])
        self.assertEqual(retained["cleanup_errors"], [cleanup])


if __name__ == "__main__":
    unittest.main()
