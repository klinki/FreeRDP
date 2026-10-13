"""Deterministic queue-workload checks without sockets or performance load."""
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rdpbench import endpoint
from rdpbench.common import read_json
from rdpbench.type1 import one_trial
from rdpbench.native import Native, QUEUE_BINS, QUEUE_FIELDS, QUEUE_WIDTH_NS, histogram_summary


class QueueWorkloadTests(unittest.TestCase):
    def test_byte_weighted_residence_uses_quantile_intervals(self):
        bins = [1, 0, 94, 0, 5]
        result = histogram_summary(bins, QUEUE_WIDTH_NS, 600000)
        self.assertEqual(result["weight"], 100)
        self.assertEqual(result["p95_bounds"], [.25, .375])
        self.assertEqual(result["p95"], .375)
        self.assertEqual(result["p99_bounds"], [.5, .625])
        self.assertEqual(result["max"], .6)

    def test_empty_or_overflowed_histogram_does_not_invent_quantiles(self):
        empty = histogram_summary([0, 0], QUEUE_WIDTH_NS, 0)
        self.assertIsNone(empty["p95"])
        self.assertIsNone(empty["max"])
        overflow = histogram_summary([10, 0], QUEUE_WIDTH_NS, 3000000000, overflow=1)
        self.assertIsNone(overflow["p99"])
        self.assertEqual(overflow["max"], 3000)
        self.assertEqual(overflow["overflow_weight"], 1)

    def queue_driver(self, final_offset):
        configured = []
        fields = dict.fromkeys(QUEUE_FIELDS, 0)
        fields.update(valid=1, queue_limit_bytes=32768, hist_bins=QUEUE_BINS,
                      hist_width_ns=QUEUE_WIDTH_NS, residence_bytes=10,
                      residence_max_ns=300000, admission_calls=2, admission_max_ns=100000,
                      accepted_cohort_bytes=10, first_sent_cohort_bytes=10,
                      own_log_window=5, peer_log_window=5, max_payload_bytes=1232,
                      effective_window_packets=31, observer_enabled=1)

        class Library:
            def freerdp_bench_v1_queue_measure(self, handle, start, stop):
                configured.append((start, stop))
                return 1

            def freerdp_bench_v1_queue_stats(self, handle, out, count):
                for i, name in enumerate(QUEUE_FIELDS):
                    out[i] = fields[name]
                return 1

            def freerdp_bench_v1_queue_hist(self, handle, kind, out, count):
                if kind == 0:
                    out[2] = 10
                elif kind == 1:
                    out[0] = 2
                return 1

        driver = Native.__new__(Native)
        driver.h = object()
        driver.lib = Library()
        driver.lib.fields = fields
        driver.queue_available = True
        driver.queue_alignment = None
        clock = lambda offset: dict(python_before_ns=100, python_after_ns=106, native_ns=103+offset,
                                    offset_ns=offset, uncertainty_ns=3)
        driver._queue_clock_sample = unittest.mock.Mock(side_effect=[clock(-10), clock(final_offset)])
        return driver, configured

    def test_queue_cohort_clock_mapping_and_retained_histograms(self):
        driver, configured = self.queue_driver(-12)
        with patch.dict("os.environ", {"RDP_BENCH_QUEUE_OBSERVER": "1"}):
            driver.configure_queue_measurement(1000, 2000)
        self.assertEqual(configured, [(990, 1990)])
        result = driver.queue_stats()
        self.assertTrue(result["valid"])
        self.assertEqual(result["clock_alignment"]["offset_drift_ns"], 2)
        self.assertEqual(result["summaries"]["residence_ms"]["p95_bounds"], [.25, .375])
        self.assertEqual(result["histograms"]["residence"][2], 10)
        self.assertEqual(result["summaries"]["blocked_admission_ms"]["weight"], 0)
        self.assertIsNone(result["summaries"]["blocked_admission_ms"]["p99"])

    def test_queue_clock_drift_beyond_brackets_invalidates_telemetry(self):
        driver, configured = self.queue_driver(250000)
        with patch.dict("os.environ", {"RDP_BENCH_QUEUE_OBSERVER": "1"}):
            driver.configure_queue_measurement(1000, 2000)
        result = driver.queue_stats()
        self.assertFalse(result["valid"])
        self.assertIn("clock alignment drift", result["reasons"][0])

    def test_queue_drain_and_histogram_accounting_failures_are_retained(self):
        driver, configured = self.queue_driver(-10)
        driver.lib.fields["accepted_cohort_bytes"] = 11
        driver.lib.fields["residence_bytes"] = 11
        with patch.dict("os.environ", {"RDP_BENCH_QUEUE_OBSERVER": "1"}):
            driver.configure_queue_measurement(1000, 2000)
        result = driver.queue_stats()
        self.assertFalse(result["valid"])
        self.assertIn("measurement cohort bytes remain unobserved after drain", result["reasons"])
        self.assertIn("residence histogram count mismatch", result["reasons"])
        self.assertEqual(result["fields"]["accepted_cohort_bytes"], 11)

    def test_queue_observer_disabled_or_old_api_returns_unknown(self):
        driver, configured = self.queue_driver(-10)
        driver.lib.fields['observer_enabled'] = 0
        driver.lib.fields['valid'] = 0
        with patch.dict("os.environ", {"RDP_BENCH_QUEUE_OBSERVER": "0"}):
            driver.configure_queue_measurement(1000, 2000)
        self.assertEqual(configured, [])
        result = driver.queue_stats()
        self.assertTrue(result['valid'])
        self.assertFalse(result['observer_enabled'])
        self.assertIsNone(result['fields']['residence_bytes'])
        self.assertEqual(result['summaries'], {})
        self.assertEqual(result['fields']['queue_limit_bytes'], 32768)
        self.assertEqual(result['fields']['effective_window_packets'], 31)
        driver.queue_available = False
        self.assertIsNone(driver.queue_stats())

    def test_upload_mixed_routes_bulk_and_probes_through_client_stream(self):
        root = Path(__file__).resolve().parents[1]
        config = read_json(root / "configs/type1.smoke.json")
        profile = read_json(root / "configs/profiles.json")["profiles"]["loopback"]

        class LaunchedProcess:
            stdin = io.StringIO()
            stdout = io.StringIO()

            def poll(self):
                return 0

        # Stop before either process runs. Inspect the actual configs produced
        # by the orchestrator instead of invoking sockets in this unit test.
        with tempfile.TemporaryDirectory() as folder:
            trial = Path(folder) / "trial"
            with patch("rdpbench.type1.environment", return_value=dict(python="test", system="test", host="test", utc="test")), \
                 patch("rdpbench.type1.free_port", side_effect=[1234, 5678]), \
                 patch("rdpbench.type1.subprocess.Popen", side_effect=[LaunchedProcess(), RuntimeError("stop before sockets")]):
                one_trial({"id": "candidate"}, {"id": "upload-mixed", "profile": "loopback",
                          "workload": "upload_mixed"}, profile, config, 11, trial, direct=True)
            self.assertEqual(read_json(trial / "endpoint-0.config.json")["role"], "bulk_probe")
            self.assertEqual(read_json(trial / "endpoint-1.config.json")["role"], "sink")

    def test_upload_probes_keep_deadlines_when_bulk_send_blocks(self):
        # A bulk write occupies the owner for 250 ms. Probe deadlines continue
        # regardless of replies; severely overdue deadlines are counted as
        # missed rather than replayed together after the blocked write.
        now = [0]
        sent = []
        events = []

        class Driver:
            def __init__(self, *args):
                pass

            def send(self, data):
                kind = endpoint.H.unpack_from(data)[1]
                sent.append(endpoint.Parser().feed(data)[0])
                if kind == 0:
                    now[0] += 250_000_000

            def read(self, timeout_ms=0):
                now[0] += 5_000_000
                return b""

            def stats(self):
                return None

            def configure_queue_measurement(self, measure_ns, stop_ns):
                pass

            def queue_stats(self):
                return None

            def close(self):
                pass

        class Journal:
            def __init__(self, *args):
                pass

            def emit(self, kind, **data):
                events.append((kind, data))

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as folder:
            config = dict(driver="native", library="fake", bind="127.0.0.1", port=1,
                          peer="127.0.0.1", peer_port=2, role="bulk_probe", seed=11,
                          payload_bytes=16384, probe_hz=20, events=str(Path(folder)/"events"),
                          result=str(Path(folder)/"result.json"))
            control = '{"start_ns":0,"measure_ns":0,"stop_ns":400000000,"end_ns":450000000}\n'
            with patch.object(endpoint, "Native", Driver), patch.object(endpoint, "Journal", Journal), \
                 patch.object(endpoint, "sleep_until"), patch.object(endpoint.time, "monotonic_ns", side_effect=lambda: now[0]), \
                 patch.object(endpoint.sys, "stdin", io.StringIO(control)), patch.object(endpoint.sys, "stdout", io.StringIO()):
                self.assertEqual(endpoint.run(config), 0)
            result = read_json(config["result"])
        probes = [r for r in sent if r[0] == 1]
        self.assertEqual([(r[1], r[2]) for r in probes], [(0, 0), (2, 100_000_000)])
        self.assertEqual(result["counters"]["probes_expected"], 8)
        self.assertEqual(result["counters"]["probes_injected"], 2)
        self.assertEqual(result["counters"]["probes_not_injected"], 6)
        self.assertEqual(result["probe_timeouts"], 2)
        self.assertEqual(result["counters"]["sent_records"], 2)
        self.assertIn(("probe_not_injected", dict(id=1, scheduled_ns=50_000_000,
                                                reason="driver_backpressure")), events)


if __name__ == "__main__":
    unittest.main()
