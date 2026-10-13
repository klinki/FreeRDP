import json
import contextlib
import io
import errno
import importlib.util
import os
import re
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from rdpbench.bridge import Client, child_environment, fingerprint, managed_argument, MANAGED_BLOCKED_KEYS
from rdpbench.common import audit, read_json, write_json
from rdpbench.live import (ProbeSchedule, aggregate, bridge_metrics, measured_probes,
                           network, render_metrics, same_identity, source_evidence,
                           ssh_options, trial, validate_inputs, receipt_errors, udp_activity_delta,
                           ledger_evidence, actual_conditions, provenance_errors, runtime_provenance,
                           environment_preflight, run_suite, begin_owned_teardown, configured_display_errors,
                           ipc_publish_script, Observer, confirmed_owned_foreground, wait_owned_foreground)

WINDOWS_AGENT_SPEC = importlib.util.spec_from_file_location("benchmark_windows_agent_fixture", ROOT / "windows/agent.py")
WINDOWS_AGENT = importlib.util.module_from_spec(WINDOWS_AGENT_SPEC)
WINDOWS_AGENT_SPEC.loader.exec_module(WINDOWS_AGENT)


class LiveTests(unittest.TestCase):
    def interval(self):
        return {"begin_ns": 1_000_000_000, "end_ns": 2_000_000_000,
                "expected": 2, "missed": [],
                "requested": [{"probe_id": 1, "scheduled_ns": 1_000_000_000},
                              {"probe_id": 2, "scheduled_ns": 1_500_000_000}]}

    def marker(self, t, probe=0, tick=1, phase=2, tag=77):
        return dict(type="marker", t_ns=t, probe_id=probe, tick=tick,
                    phase_id=phase, run_tag=tag)

    def test_fixed_schedule_skips_late_slots_without_catchup(self):
        schedule = ProbeSchedule(0, 2_000_000_000, 2, budget_ms=50)
        self.assertEqual(schedule.due(0), ((1, 0), []))
        self.assertEqual(schedule.due(1_010_000_000), ((3, 1_000_000_000), [(2, 500_000_000)]))
        self.assertEqual(schedule.due(1_510_000_000), ((4, 1_500_000_000), []))
        self.assertEqual(schedule.finish(), [])
        self.assertEqual(schedule.total, 4)

    def test_over_budget_and_final_unvisited_slots_are_missed(self):
        schedule = ProbeSchedule(0, 1_000_000_000, 2, budget_ms=20)
        self.assertEqual(schedule.due(21_000_000), (None, [(1, 0)]))
        self.assertEqual(schedule.finish(), [(2, 500_000_000)])
        self.assertEqual(schedule.finish(), [])

    def test_response_drain_scores_after_end_and_retains_timeouts(self):
        events = [dict(type="probe_injected", probe_id=1, t_ns=1_010_000_000),
                  dict(type="probe_injected", probe_id=2, t_ns=1_990_000_000),
                  self.marker(2_050_000_000, probe=2, tick=20),
                  self.marker(1_900_000_000, probe=1, phase=1),
                  self.marker(1_020_000_000, probe=1, tag=88)]
        metrics, probes = measured_probes(events, self.interval(), 3_000_000_000, 77, 2, 60, .5)
        self.assertEqual(probes["injected"], 2)
        self.assertEqual(probes["observed"], 1)
        self.assertEqual(probes["timed_out_ids"], [1])
        self.assertEqual(metrics["input_visible_ms.p99"], 60)
        self.assertEqual(metrics["input_scheduled_ms.p99"], 550)
        self.assertEqual(metrics["progress.marker_observations"], 0)

    def test_missing_injections_cannot_be_called_zero_latency(self):
        metrics, probes = measured_probes([], self.interval(), 5_000_000_000, 77, 2, 60)
        self.assertEqual(probes["missed_injections"], 2)
        self.assertEqual(probes["unconfirmed_requests"], [1, 2])
        self.assertEqual(probes["timeouts"], 0)
        self.assertIsNone(metrics["input_visible_ms.p95"])
        self.assertEqual(metrics["input_visible_ms.count"], 0)

    def test_too_short_drain_reports_censoring_and_ignores_late_response(self):
        events = [dict(type="probe_injected", probe_id=1, t_ns=1_200_000_000),
                  self.marker(2_400_000_000, probe=1)]
        _, probes = measured_probes(events, self.interval(), 2_200_000_000, 77, 2, 60, 2)
        self.assertEqual(probes["censored_ids"], [1])
        self.assertEqual(probes["observed"], 0)
        self.assertEqual(probes["timeouts"], 0)

    def test_duplicate_injection_and_idle_progress_are_explicit(self):
        events = [dict(type="probe_injected", probe_id=1, t_ns=1_010_000_000),
                  dict(type="probe_injected", probe_id=1, t_ns=1_020_000_000),
                  self.marker(1_030_000_000, probe=1)]
        metrics, probes = measured_probes(events, self.interval(), 6_000_000_000, 77, 2, 60, idle=True)
        self.assertEqual(probes["duplicate_injection_ids"], [1])
        self.assertEqual(metrics["input_visible_ms.p99"], 20)
        self.assertIsNone(metrics["progress.longest_gap_ms"])

    def test_progress_uses_unique_source_ticks_with_censored_edges(self):
        events = [self.marker(1_100_000_000, tick=1), self.marker(1_200_000_000, probe=9, tick=1),
                  self.marker(1_800_000_000, tick=4), self.marker(2_050_000_000, tick=6)]
        metrics, _ = measured_probes(events, self.interval(), 5_000_000_000, 77, 2, 60)
        self.assertEqual(metrics["progress.observed_hz"], 2)
        self.assertEqual(metrics["progress.longest_gap_ms"], 700)
        self.assertEqual(metrics["progress.freeze_fraction_over_250ms"], .7)

    def test_traffic_is_weighted_and_goodput_stays_unknown(self):
        events = []
        for timestamp, seconds, value, age in ((1.1, 1, 10, .1), (1.7, 2, 20, .7), (1.9, 1, 30, .1)):
            events.append((round(timestamp * 1e9), {"type": "performance_sample", "performance": {
                "connected": True, "reset": False, "epoch": 1, "sequence": len(events) + 1,
                "timestamp": timestamp, "intervalSeconds": seconds, "traffic": {"udpReceive": value},
                "rtt": {"milliseconds": 30, "ageSeconds": age, "source": "rdp", "transport": "udp"}}}))
        metrics, negotiated = bridge_metrics(events, 1_000_000_000, 2_000_000_000)
        self.assertEqual(metrics["traffic.udp_rx_Bps_mean"], 20)
        self.assertIsNone(metrics["verified_goodput_mbps"])
        self.assertEqual(metrics["transport_rtt_ms.count"], 2)
        self.assertIsNone(negotiated["graphics_udp_verified"])

    def test_render_includes_only_complete_measured_intervals(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "render.jsonl"
            entries = []
            for start, duration, sample in ((0, 150, 9000000), (100, 100, 1000000), (200, 100, 3000000), (290, 100, 7000000)):
                entries.append(dict(schema="freerdp.sdl_render_metrics", interval_start_ns=start,
                                    interval_duration_ns=duration, uploaded_bytes=10,
                                    redraw_wall_ns_samples=[sample], frame_interval_wall_ns_samples=[]))
            path.write_text("\n".join(json.dumps(entry) for entry in entries) + "\n")
            metrics = render_metrics(path, 100, 300)
            self.assertEqual(metrics["render.redraw_ms.count"], 2)
            self.assertEqual(metrics["render.redraw_ms.p95"], 3)

    def test_identity_requires_uuid_pid_and_interactive_session(self):
        identity = dict(agent_uuid="fixture", pid=10, windows_session_id=2)
        self.assertTrue(same_identity(identity, dict(identity, arbitrary="extra")))
        self.assertFalse(same_identity(identity, dict(identity, agent_uuid="other")))
        self.assertFalse(same_identity({}, {}))

    def test_ssh_control_path_used_for_ssh_and_scp_options(self):
        options = ssh_options({"ssh_control_path": "/private/tmp/test-control.sock"})
        self.assertIn("ControlPath=/private/tmp/test-control.sock", options)
        self.assertIn("BatchMode=yes", options)
        self.assertIn("StrictHostKeyChecking=yes", options)

    def test_bridge_certificate_pin_and_auth_secret_never_journaled(self):
        client = object.__new__(Client)
        client.lab = dict(password_env="TEST_LIVE_SECRET", username="fixture", certificate_sha256="ab" * 32)
        client.sid = "owned"
        client.events, client.overflow = [], 0
        client.lock, client.journal, client.send = threading.Lock(), Mock(), Mock()
        self.assertEqual(fingerprint("SHA256:" + ":".join(["ab"] * 32)), "ab" * 32)
        with patch.dict(os.environ, TEST_LIVE_SECRET="unit-fixture-secret"):
            client.handle(dict(type="auth_request", sessionId="owned", requestId="auth", rejected=False), 1)
        client.send.assert_called_with("auth_response", requestId="auth", accepted=True,
                                       username="fixture", domain="", password="unit-fixture-secret")
        self.assertNotIn("unit-fixture-secret", repr(client.journal.mock_calls))
        client.handle(dict(type="certificate_request", sessionId="owned", requestId="cert", fingerprint="ab" * 32), 2)
        client.send.assert_called_with("certificate_response", requestId="cert", decision="accept_once")
        with self.assertRaisesRegex(RuntimeError, "mismatched"):
            client.handle(dict(type="certificate_request", sessionId="owned", requestId="cert2", fingerprint="cd" * 32), 3)

    def test_network_rejects_owner_and_verification_mismatch(self):
        lab = {"network": {"apply_command": ["fixture-adapter"]}}
        for response in ({"verified": False}, {"verified": True, "run_id": "foreign"},
                         {"verified": True, "profile_id": "wrong"}):
            with patch("rdpbench.live.subprocess.check_output", return_value=json.dumps(response)):
                with self.assertRaises(RuntimeError):
                    network(lab, "apply", "owned", Mock(), profile_id="clean")

    def test_apply_failure_still_restores_and_seals_partial_artifacts(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "client"
            binary.write_bytes(b"not-run")
            variant = dict(id="candidate", client=str(binary), commit="fixture")
            cfg = dict(warmup_s=1, measure_s=1, probe_hz=2, source_fps=60, scroll_px_s=400)
            case = dict(id="test-clean", scene="canvas-motion", profile="clean")
            lab = dict(lab_id="fixture", rdp_arguments=[], username="fixture", password_env="TEST_LIVE_SECRET")
            calls = []
            def fake_network(_lab, operation, *_args, **_kwargs):
                calls.append(operation)
                if operation == "apply":
                    raise RuntimeError("apply mutated then failed")
                return {"verified": True}
            with patch("rdpbench.live.network", side_effect=fake_network):
                result = trial(variant, case, {}, cfg, lab, 11, root / "trial")
            self.assertEqual(calls, ["apply", "restore"])
            self.assertEqual(result["sut_outcome"], "INCOMPLETE")
            self.assertFalse(result["strict_eligible"])
            self.assertEqual(audit(root / "trial"), [])

    def test_source_evidence_filters_phase_and_retains_overflow(self):
        identity = dict(agent_uuid="fixture", pid=3, windows_session_id=2)
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / "windows").mkdir()
            record = dict(type="fixture_events", identity=identity, droppedEvents=2,
                          events=[dict(type="frame_intent", runTag=77, phaseId=1, sourceSkippedTicks=9),
                                  dict(type="frame_intent", runTag=77, phaseId=2, sourceSkippedTicks=3),
                                  dict(type="probe_received", runTag=77, phaseId=2, probeId=2)])
            (root / "windows/agent.jsonl").write_text(json.dumps(record) + "\n")
            source = source_evidence(root, 77, 2, identity)
            self.assertEqual(source["frame_intents"], 1)
            self.assertEqual(source["source_skipped_ticks"], 3)
            self.assertEqual(source["dropped_events"], 2)
            self.assertEqual(source["probe_received_ids"], [2])
            self.assertTrue(source["identity_consistent"])

    def test_missing_planned_trials_remain_in_aggregate(self):
        with tempfile.TemporaryDirectory() as name:
            plan = dict(campaign_id="fixture", variants=[], profiles=[], environment={},
                        trials=[dict(trial_id="missing", variant="candidate", seed=11)])
            result = aggregate(name, plan, {"lab_id": "fixture"})
            self.assertEqual(result["schema"], "freerdp.live_benchmark")
            self.assertEqual(result["trials"][0]["sut_outcome"], "INCOMPLETE")
            self.assertEqual(result["trials"][0]["metrics"], {})

    def test_password_arguments_rejected_before_manifest_creation(self):
        with tempfile.TemporaryDirectory() as name:
            binary = Path(name) / "client"
            binary.write_bytes(b"fixture")
            cfg = dict(warmup_s=1, measure_s=1, probe_hz=2, source_fps=60, scroll_px_s=400,
                       seeds=[11], cases=[dict(id="test", scene="canvas-motion", profile="clean")])
            lab = dict(lab_id="fixture", ssh_target="fixture@127.0.0.1", windows_root="C:\\RdpBench",
                       windows_task="test", username="fixture", password_env="TEST_LIVE_SECRET",
                       certificate_sha256="ab" * 32, rdp_arguments=["/p:must-not-be-recorded"])
            with self.assertRaisesRegex(ValueError, "passwords"):
                validate_inputs(cfg, lab, [dict(id="candidate", client=str(binary))], {"clean": {}})

    def test_managed_argument_guard_matches_native_bridge_policy(self):
        path=ROOT.parents[1] / "client/SDL/SDL3/sdl_launcher.cpp"
        if not path.is_file():self.skipTest("this PR checkout has no native SDL launcher bridge policy")
        source=path.read_text()
        block = source.split("const char* blocked[] =", 1)[1].split("};", 1)[0]
        self.assertEqual(set(re.findall(r'"([a-z0-9-]+)"', block)), MANAGED_BLOCKED_KEYS)
        for argument in ("/log-level:INFO", "/log-filters:core:TRACE", "/monitors:1", "/sdl-monitor-scale:1=100/100",
                         "/tls:secrets-file:/tmp/test", "/tls-secrets-file:/tmp/test", "/cert:ignore",
                         "/launcher-capabilities", "/sec:aad", "/sec:ext", "saved.rdp", "/size:10\0bad"):
            self.assertFalse(managed_argument(argument), argument)
        for argument in ("/v:127.0.0.1:43389", "/u:david", "/size:1920x1080", "/gfx:AVC444:on",
                         "+multitransport", "/tls:enforce:1.3", "/sec:nla"):
            self.assertTrue(managed_argument(argument), argument)

    def receipt(self, state="active", hits=10, latency=.5):
        profile = {direction: dict(rate_mbps=10, one_way_ms=15) for direction in ("c2s", "s2c")}
        result = dict(verified=True, valid=True, active=state == "active", state=state,
                      run_id="fixture", profile_id="clean", profile=profile, seed=11,
                      ledger=dict(valid=True, overflow=0, error=None, closed=state != "active", pending_records=0),
                      udp_errors={}, tcp_connections={}, sockets_closed=state != "active",
                      full_rdp_profile=False, tcp_impaired=False, error=None)
        for direction in ("c2s", "s2c"):
            result[direction] = dict(profile=profile[direction], seed=11 + (1000003 if direction == "s2c" else 0),
                counter_hits=hits, received_datagrams=hits, delivered_datagrams=hits,
                socket_send_errors=0, pending_datagrams=0, pending_udp_bytes=0,
                packet_accounting=dict(received=hits, created_copies=0, accounted=hits, balanced=True),
                lateness_ms=dict(p99=latency if hits else None), random_drops=0, queue_drops=0, blackout_drops=0)
        return result

    def test_verified_receipt_cannot_hide_inactive_invalid_or_bad_accounting(self):
        receipt = self.receipt()
        self.assertEqual(receipt_errors(receipt, "status", receipt["profile"], 11), [])
        for mutation in (dict(active=False), dict(valid=False), dict(error="worker failed"),
                         dict(ledger=dict(valid=True, overflow=1)), dict(udp_errors=dict(socket_read_errors=1))):
            altered = {**receipt, **mutation}
            self.assertTrue(receipt_errors(altered, "status", receipt["profile"], 11))
        receipt["c2s"]["packet_accounting"]["accounted"] = 9
        self.assertTrue(receipt_errors(receipt, "status", receipt["profile"], 11))

    def test_apply_no_hits_allowed_and_final_pending_is_explicit(self):
        receipt = self.receipt(hits=0)
        self.assertEqual(receipt_errors(receipt, "apply", receipt["profile"], 11), [])
        self.assertTrue(receipt_errors(receipt, "status", receipt["profile"], 11))
        final = self.receipt(state="restored")
        self.assertEqual(receipt_errors(final, "restore", final["profile"], 11), [])
        del final["s2c"]["pending_udp_bytes"]
        self.assertTrue(receipt_errors(final, "restore", final["profile"], 11))

    def test_lateness_budget_and_measured_udp_delta_preserve_tcp_only_unknown(self):
        before, after = self.receipt(), self.receipt(hits=20, latency=2)
        self.assertTrue(receipt_errors(after, "status", after["profile"], 11))
        delta, errors = udp_activity_delta(before, after)
        self.assertEqual(delta, dict(c2s=10, s2c=10))
        self.assertEqual(errors, [])
        after["c2s"]["counter_hits"] = 10
        self.assertTrue(udp_activity_delta(before, after)[1])

    def test_winpr_float_sequence_gaps_and_unknown_sample_coverage(self):
        def event(sequence):
            return (1_100_000_000, dict(type="performance_sample", performance=dict(
                connected=True, reset=False, sequence=sequence, generation=1.0,
                traffic={}, intervalSeconds=1)))
        metrics, _ = bridge_metrics([event(37.0), event(38.0), event(40.0)], 1_000_000_000, 2_000_000_000)
        self.assertEqual(metrics["telemetry.sequence_gaps"], 1)
        self.assertEqual(metrics["telemetry.sequence_known_samples"], 3)
        self.assertEqual(metrics["telemetry.sequence_unknown_samples"], 0)
        metrics, _ = bridge_metrics([event(None), event(False), event(40.5)], 1_000_000_000, 2_000_000_000)
        self.assertIsNone(metrics["telemetry.sequence_gaps"])
        self.assertEqual(metrics["telemetry.sequence_unknown_samples"], 3)

    def test_all_children_sanitize_password_env_and_conditions_pair_only_actual_treatments(self):
        lab = dict(password_env="TEST_CUSTOM_PASS", rdp_arguments=["/size:1920x1080"])
        with patch.dict(os.environ, TEST_CUSTOM_PASS="unit-secret", RDP_BENCH_PASSWORD="unit-secret", API_TOKEN="unit-token", WLOG_LEVEL="INFO"):
            env = child_environment(lab)
            for name in ("TEST_CUSTOM_PASS", "RDP_BENCH_PASSWORD", "API_TOKEN"):
                self.assertNotIn(name, env)
            original = actual_conditions(lab, dict(env={}, arguments=[]))
            self.assertNotEqual(original, actual_conditions(lab, dict(env={"WLOG_LEVEL": "DEBUG"})))
            self.assertNotEqual(original, actual_conditions(lab, dict(arguments=["/gfx:AVC420:on"])))
            self.assertEqual(original, actual_conditions(lab, dict(env={"FREERDP_SDL_RENDER_METRICS": "/different/output"})))

    def test_unobserved_certificate_pin_cannot_enter_measurement(self):
        client = object.__new__(Client)
        client.pin_seen = False
        with self.assertRaisesRegex(RuntimeError, "not observed"):
            client.require_observed_pin()
        client.pin_seen = True
        client.require_observed_pin()

    def test_runtime_provenance_detects_source_and_binary_mutation(self):
        with tempfile.TemporaryDirectory() as name:
            binary = Path(name) / "client"
            binary.write_bytes(b"frozen")
            provenance = runtime_provenance({}, [dict(client=str(binary))])
            self.assertEqual(provenance_errors(provenance), [])
            binary.write_bytes(b"changed")
            self.assertTrue(any("client" in error for error in provenance_errors(provenance)))
            self.assertIn("windows/agent.py", provenance["windows_sources"])

    def test_closed_ledger_requires_measured_server_data_and_reconciles_pending(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "relay.jsonl"
            entries = []
            for direction in ("c2s", "s2c"):
                entries += [dict(type="offer", direction=direction, packet_id=1, scheduled_copies=1,
                                 offered_ns=1_100_000_000, kind="data_without_ack" if direction == "s2c" else "ack_only"),
                            dict(type="dispatch", direction=direction, packet_id=1, copy_id=1,
                                 offered_ns=1_100_000_000,
                                 due_ns=1_115_000_000, actual_dispatch_ns=1_115_500_000,
                                 kind="data_without_ack" if direction == "s2c" else "ack_only")]
            path.write_text("\n".join(json.dumps(entry) for entry in entries) + "\n")
            receipt = self.receipt(state="restored", hits=1)
            receipt["capturepath"] = str(path)
            receipt["ledger"]["count"] = len(entries)
            evidence, errors = ledger_evidence(receipt, self.interval(), name)
            self.assertEqual(errors, [])
            self.assertEqual(evidence["s2c"]["data_dispatches"], 1)
            entries[-1]["kind"] = "ack_only"
            path.write_text("\n".join(json.dumps(entry) for entry in entries) + "\n")
            self.assertTrue(any("DATA" in error for error in ledger_evidence(receipt, self.interval(), name)[1]))
            self.assertTrue(any("metadata" in error for error in ledger_evidence(receipt, self.interval(), name)[1]))

    def test_overdue_boundary_dispatch_is_not_excluded_from_tail_timing(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "relay.jsonl"
            entries = []
            for direction in ("c2s", "s2c"):
                for packet, offered, due, actual in ((1, 1_100_000_000, 1_110_000_000, 1_110_500_000),
                                                     (2, 1_990_000_000, 1_999_000_000, 2_005_000_000)):
                    metadata = dict(direction=direction, packet_id=packet, offered_ns=offered,
                                    kind="data_without_ack" if direction == "s2c" else "ack_only", flow_id=1, udp_bytes=1200)
                    entries += [dict(type="offer", scheduled_copies=1, **metadata),
                                dict(type="dispatch", copy_id=packet, due_ns=due, actual_dispatch_ns=actual, **metadata)]
            path.write_text("\n".join(json.dumps(entry) for entry in entries) + "\n")
            receipt = self.receipt(state="restored", hits=2)
            receipt["capturepath"] = str(path)
            receipt["ledger"]["count"] = len(entries)
            evidence, errors = ledger_evidence(receipt, self.interval(), name)
            self.assertEqual(evidence["s2c"]["dispatches"], 1)
            self.assertEqual(evidence["s2c"]["dispatched_after_measurement"], 1)
            self.assertEqual(evidence["s2c"]["lateness_ms"]["p99"], 6)
            self.assertTrue(any("budget" in error for error in errors))
            receipt["ledger"]["count"] += 1
            self.assertTrue(any("record count" in error for error in ledger_evidence(receipt, self.interval(), name)[1]))

    def test_environment_gate_failure_launches_nothing_and_retains_complete_plan(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "client"
            binary.write_bytes(b"never-launched")
            cfg = dict(warmup_s=1, measure_s=1, probe_hz=2, source_fps=60, scroll_px_s=400,
                       seeds=[11,29], cases=[dict(id="clean", scene="canvas-motion", profile="clean"),
                                             dict(id="impaired", scene="canvas-motion", profile="impaired")])
            lab = dict(lab_id="fixture", ssh_target="fixture@127.0.0.1", windows_root="C:\\RdpBench",
                       windows_task="test", username="fixture", password_env="TEST_DESKTOP_SECRET",
                       certificate_sha256="ab" * 32, rdp_arguments=["/size:1920x1080"],
                       observer=dict(environment_command=["fixture-environment-helper"]))
            files = {"suite": cfg, "lab": lab, "variants": {"variants": [dict(id="candidate", client=str(binary))]},
                     "profiles": {"profiles": {"clean": {}, "impaired": {}}}}
            for key, value in files.items():
                write_json(root / (key + ".json"), value)
            args = SimpleNamespace(**{key: str(root / (key + ".json")) for key in files},
                                   case=None, output=str(root / "campaign"))
            completed = SimpleNamespace(returncode=0, stdout=json.dumps(dict(available=False, screen_locked=True,
                                            capture_allowed=True, accessibility_allowed=True)), stderr="")
            with patch.dict(os.environ, TEST_DESKTOP_SECRET="unit-credential", RDP_BENCH_PASSWORD="unit-credential"), \
                 patch("rdpbench.live.environment", return_value=dict(utc="fixture")), \
                 patch("rdpbench.live.subprocess.run", return_value=completed) as subprocess_run, \
                 patch("rdpbench.live.trial") as launch, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_suite(args), 2)
                launch.assert_not_called()
                env = subprocess_run.call_args.kwargs["env"]
                self.assertNotIn("TEST_DESKTOP_SECRET", env)
                self.assertNotIn("RDP_BENCH_PASSWORD", env)
            result = read_json(root / "campaign/live-results.json")
            self.assertEqual(len(result["trials"]), 4)
            self.assertTrue(all(trial["sut_outcome"] == "INCOMPLETE" and trial["metrics"] == {} for trial in result["trials"]))
            self.assertTrue(all(any("desktop preflight" in reason for reason in trial["reasons"]) for trial in result["trials"]))
            self.assertEqual(sorted(path.name for path in (root / "campaign").iterdir()),
                             ["campaign.json", "live-results.json", "preflight-environment.json"])
            self.assertFalse(read_json(root / "campaign/preflight-environment.json")["latest"]["available"])

    def test_environment_gate_uses_strict_computed_availability_not_private_lock_key(self):
        lab = dict(observer=dict(environment_command=["fixture-helper"]))
        completed = SimpleNamespace(returncode=0, stdout=json.dumps(dict(available=True, screen_locked=None,
                                    capture_allowed=True, accessibility_allowed=True)), stderr="")
        with patch("rdpbench.live.subprocess.run", return_value=completed):
            self.assertTrue(environment_preflight(lab, "/private/tmp", "fixture")["available"])
        for value in (None, 1, "true", False):
            completed.stdout = json.dumps(dict(available=value))
            with patch("rdpbench.live.subprocess.run", return_value=completed):
                self.assertFalse(environment_preflight(lab, "/private/tmp", "fixture")["available"])
        completed.returncode = 1
        completed.stdout = json.dumps(dict(available=True))
        with patch("rdpbench.live.subprocess.run", return_value=completed):
            self.assertFalse(environment_preflight(lab, "/private/tmp", "fixture")["available"])

    def test_optional_environment_gate_absence_stays_unknown(self):
        result = environment_preflight({}, "/private/tmp", "fixture")
        self.assertIsNone(result["available"])
        self.assertFalse(result["required"])
        self.assertEqual(result["status"], "not_configured")

    def test_selected_physical_uuid_and_observer_id_must_both_be_active(self):
        lab = dict(display_selections=[dict(uuid="EXTERNAL-UUID")],
                   observer=dict(command=["fixture-helper", "--display", "111"]))
        payload = dict(display_inventory_valid=True,
                       active_displays=[dict(display_id=111, uuid="external-uuid"),
                                        dict(display_id=222, uuid="builtin-uuid")])
        self.assertEqual(configured_display_errors(lab, payload), [])
        payload["active_displays"] = [dict(display_id=222, uuid="builtin-uuid")]
        errors = configured_display_errors(lab, payload)
        self.assertTrue(any("UUID is not active" in error for error in errors))
        self.assertTrue(any("observer display ID is not active" in error for error in errors))

    def test_active_observer_id_cannot_substitute_another_physical_display(self):
        lab = dict(display_selections=[dict(uuid="external-uuid")],
                   observer=dict(command=["fixture-helper", "--display", "222"]))
        payload = dict(display_inventory_valid=True,
                       active_displays=[dict(display_id=111, uuid="external-uuid"),
                                        dict(display_id=222, uuid="builtin-uuid")])
        self.assertTrue(any("does not belong" in error for error in configured_display_errors(lab, payload)))
        lab["observer"]["command"][-1] = "333"
        self.assertTrue(configured_display_errors(lab, payload))

    def test_unknown_or_ambiguous_active_display_inventory_fails_closed(self):
        lab = dict(display_selections=[dict(uuid="external-uuid")], observer={})
        for payload in ({}, dict(display_inventory_valid=False, active_displays=[]),
                        dict(display_inventory_valid=True, active_displays=[dict(display_id=111, uuid=None)]),
                        dict(display_inventory_valid=True, active_displays=[dict(display_id=111, uuid="external-uuid"),
                                                                          dict(display_id=222, uuid="external-uuid")])):
            self.assertTrue(configured_display_errors(lab, payload))

    def test_global_desktop_available_cannot_override_missing_selected_display(self):
        lab = dict(display_selections=[dict(uuid="external-uuid")],
                   observer=dict(environment_command=["fixture-helper"],
                                 command=["fixture-helper", "--display", "111"]))
        payload = dict(available=True, display_inventory_valid=True,
                       active_displays=[dict(display_id=222, uuid="builtin-uuid")])
        with patch("rdpbench.live.subprocess.run", return_value=SimpleNamespace(returncode=0, stdout=json.dumps(payload))):
            result = environment_preflight(lab, "/private/tmp", "fixture")
        self.assertFalse(result["available"])
        self.assertEqual(result["observed"]["active_displays"], payload["active_displays"])
        self.assertTrue(result["required"])
        self.assertFalse(environment_preflight(dict(display_selections=[dict(uuid="external-uuid")]),
                                               "/private/tmp", "fixture")["available"])

    def test_display_removal_between_trials_stops_launches_and_preserves_remaining_plan(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            binary = root / "client"
            binary.write_bytes(b"never-executed")
            cfg = dict(warmup_s=1, measure_s=1, probe_hz=2, source_fps=60, scroll_px_s=400,
                       seeds=[11,29], cases=[dict(id="clean", scene="canvas-motion", profile="clean"),
                                             dict(id="impaired", scene="canvas-motion", profile="impaired")])
            lab = dict(lab_id="fixture", ssh_target="fixture@127.0.0.1", windows_root="C:\\RdpBench",
                       windows_task="test", username="fixture", password_env="TEST_DESKTOP_SECRET",
                       certificate_sha256="ab" * 32, rdp_arguments=["/size:1920x1080"],
                       display_selections=[dict(uuid="external-uuid")],
                       observer=dict(environment_command=["fixture-environment-helper"],
                                     command=["fixture-observer", "--display", "111"]))
            files = {"suite": cfg, "lab": lab, "variants": {"variants": [dict(id="candidate", client=str(binary))]},
                     "profiles": {"profiles": {"clean": {}, "impaired": {}}}}
            for key, value in files.items():
                write_json(root / (key + ".json"), value)
            args = SimpleNamespace(**{key: str(root / (key + ".json")) for key in files},
                                   case=None, output=str(root / "campaign"))
            def check(uuid_value, display_id):
                return SimpleNamespace(returncode=0, stdout=json.dumps(dict(available=True,
                    display_inventory_valid=True, active_displays=[dict(uuid=uuid_value, display_id=display_id)])))
            def complete(*arguments):
                folder = arguments[6]
                folder.mkdir()
                result = dict(trial_id=folder.name, validity="PROVISIONAL_LIVE", sut_outcome="COMPLETE",
                              strict_eligible=True, reasons=[], metrics={}, probes={})
                write_json(folder / "summary.json", result)
                from rdpbench.common import seal
                seal(folder)
                return result
            with patch("rdpbench.live.environment", return_value=dict(utc="fixture")), \
                 patch("rdpbench.live.subprocess.run", side_effect=[check("external-uuid", 111), check("builtin-uuid", 222)]), \
                 patch("rdpbench.live.trial", side_effect=complete) as launch, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_suite(args), 2)
                self.assertEqual(launch.call_count, 1)
            result = read_json(root / "campaign/live-results.json")
            self.assertEqual(len(result["trials"]), 4)
            self.assertEqual(sum(trial["sut_outcome"] == "COMPLETE" for trial in result["trials"]), 1)
            self.assertEqual(sum(trial["sut_outcome"] == "INCOMPLETE" for trial in result["trials"]), 3)
            self.assertFalse(result["environment_preflight"]["available"])

    def test_teardown_counter_exemption_requires_owned_marker_and_preserves_active_errors(self):
        receipt = self.receipt(state="restored")
        receipt["tcp_connections"] = dict(socket_errors=1, active_socket_errors=0,
                                          teardown_client_resets=1, discarded_pending_bytes=0)
        receipt["teardown_started_ns"] = 3_000_000_000
        proof = dict(marker_ns=3_000_000_000, client_cancel_ns=3_100_000_000)
        self.assertEqual(receipt_errors(receipt, "restore", receipt["profile"], 11, teardown_evidence=proof), [])
        self.assertTrue(receipt_errors(receipt, "restore", receipt["profile"], 11))
        receipt["tcp_connections"].update(socket_errors=2, active_socket_errors=1)
        self.assertTrue(receipt_errors(receipt, "restore", receipt["profile"], 11, teardown_evidence=proof))
        receipt["tcp_connections"].update(socket_errors=1)
        self.assertTrue(any("reconcile" in error for error in receipt_errors(receipt, "restore", receipt["profile"], 11, teardown_evidence=proof)))
        receipt["tcp_connections"] = dict(socket_errors=1)
        self.assertTrue(receipt_errors(receipt, "restore", receipt["profile"], 11, teardown_evidence=proof))

    def test_begin_teardown_is_a_fresh_owned_status_request(self):
        receipt = self.receipt()
        receipt["teardown_started_ns"] = 110
        receipts = []
        with patch("rdpbench.live.network", return_value=receipt) as request, \
             patch("rdpbench.live.time.monotonic_ns", side_effect=[100,120]):
            proof, errors = begin_owned_teardown({}, "fixture", "clean", receipt["profile"], 11, Mock(), receipts, 1.5)
        self.assertEqual(errors, [])
        self.assertEqual(proof["marker_ns"], 110)
        self.assertTrue(request.call_args.kwargs["begin_teardown"])
        self.assertEqual(request.call_args.kwargs["profile_id"], "clean")
        self.assertEqual(receipts, [receipt])
        receipt["teardown_started_ns"] = 90
        with patch("rdpbench.live.network", return_value=receipt), \
             patch("rdpbench.live.time.monotonic_ns", side_effect=[100,120]):
            self.assertTrue(begin_owned_teardown({}, "fixture", "clean", receipt["profile"], 11, Mock(), [], 1.5)[1])

    def test_tcp_reset_ledger_requires_zero_pending_client_reset_after_actual_cancel(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "relay.jsonl"
            entries = []
            for direction in ("c2s", "s2c"):
                metadata = dict(direction=direction, packet_id=1, offered_ns=1_100_000_000,
                                kind="data_without_ack" if direction == "s2c" else "ack_only")
                entries += [dict(type="offer", scheduled_copies=1, **metadata),
                            dict(type="dispatch", copy_id=1, due_ns=1_115_000_000,
                                 actual_dispatch_ns=1_115_500_000, **metadata)]
            entries += [dict(type="owned_teardown_begin", time_ns=3_000_000_000, run_id="fixture"),
                        dict(type="tcp_close", time_ns=3_200_000_000, error="Connection reset",
                             error_errno=errno.ECONNRESET, error_side="client", error_class="owned_client_reset",
                             teardown_started_ns=3_000_000_000, pending_bytes=[0,0],
                             received_bytes=[4608,3120], forwarded_bytes=[4608,3120])]
            receipt = self.receipt(state="restored", hits=1)
            receipt.update(capturepath=str(path), teardown_started_ns=3_000_000_000)
            receipt["tcp_connections"] = dict(socket_errors=1, active_socket_errors=0, teardown_client_resets=1)
            receipt["ledger"]["count"] = len(entries)
            proof = dict(marker_ns=3_000_000_000, client_cancel_ns=3_100_000_000)
            def inspect():
                path.write_text("\n".join(json.dumps(entry) for entry in entries) + "\n")
                return ledger_evidence(receipt, self.interval(), name, teardown_evidence=proof)[1]
            self.assertEqual(inspect(), [])
            for field, value in (("error_side", "target"), ("pending_bytes", [1,0]),
                                 ("time_ns", 3_050_000_000), ("error_errno", errno.EPIPE)):
                original = entries[-1][field]
                entries[-1][field] = value
                self.assertTrue(inspect(), field)
                entries[-1][field] = original

    def sharing_error(self, code):
        error = OSError("fixture Windows error")
        error.winerror = code
        return error

    def test_windows_atomic_status_replace_retries_only_sharing_and_lock_errors(self):
        with patch.object(WINDOWS_AGENT.os, "replace", side_effect=[self.sharing_error(32), self.sharing_error(33), None]) as replace, \
             patch.object(WINDOWS_AGENT.time, "sleep") as sleep:
            WINDOWS_AGENT.atomic_replace("source.tmp", "status.json", attempts=3)
        self.assertEqual(replace.call_count, 3)
        self.assertEqual([item.args[0] for item in sleep.call_args_list], [.025,.05])
        for code in (2,5,80,87,183):
            with patch.object(WINDOWS_AGENT.os, "replace", side_effect=self.sharing_error(code)) as replace, \
                 patch.object(WINDOWS_AGENT.time, "sleep") as sleep:
                with self.assertRaises(OSError):
                    WINDOWS_AGENT.atomic_replace("source.tmp", "status.json")
            self.assertEqual(replace.call_count, 1)
            sleep.assert_not_called()

    def test_windows_atomic_status_replace_is_bounded_and_keeps_previous_target(self):
        with tempfile.TemporaryDirectory() as name:
            destination = Path(name) / "status.json"
            destination.write_text('{"previous":true}')
            with patch.object(WINDOWS_AGENT.os, "replace", side_effect=self.sharing_error(32)) as replace, \
                 patch.object(WINDOWS_AGENT.time, "sleep") as sleep:
                with self.assertRaisesRegex(TimeoutError, "after 3 attempts"):
                    WINDOWS_AGENT.atomic_replace(Path(name) / "new.tmp", destination, attempts=3)
            self.assertEqual(replace.call_count, 3)
            self.assertEqual(sleep.call_count, 2)
            self.assertEqual(destination.read_text(), '{"previous":true}')

    def test_ambiguous_windows_access_denied_retries_only_kernel_confirmed_sharing(self):
        with patch.object(WINDOWS_AGENT.os, "replace", side_effect=[self.sharing_error(5), None]) as replace, \
             patch.object(WINDOWS_AGENT, "target_sharing_violation", return_value=True) as probe, \
             patch.object(WINDOWS_AGENT.time, "sleep") as sleep:
            WINDOWS_AGENT.atomic_replace("source.tmp", "status.json")
        self.assertEqual(replace.call_count, 2)
        probe.assert_called_once_with("status.json")
        self.assertEqual(sleep.call_count, 1)
        with patch.object(WINDOWS_AGENT.os, "replace", side_effect=self.sharing_error(5)) as replace, \
             patch.object(WINDOWS_AGENT, "target_sharing_violation", return_value=False), \
             patch.object(WINDOWS_AGENT.time, "sleep") as sleep:
            with self.assertRaises(OSError):
                WINDOWS_AGENT.atomic_replace("source.tmp", "status.json")
        self.assertEqual(replace.call_count, 1)
        sleep.assert_not_called()

    def test_status_writer_creates_unique_closed_json_before_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as name:
            destination = Path(name) / "status.json"
            destination.write_text('{"previous":true}')
            temporary_paths = []
            real_replace = WINDOWS_AGENT.os.replace
            def inspect(source, target):
                self.assertEqual(destination.read_text(), '{"previous":true}')
                self.assertEqual(json.loads(source.read_text()), dict(next=True))
                temporary_paths.append(source)
                real_replace(source, target)
            with patch.object(WINDOWS_AGENT.os, "replace", side_effect=inspect):
                WINDOWS_AGENT.write(destination, dict(next=True))
            self.assertEqual(json.loads(destination.read_text()), dict(next=True))
            self.assertTrue(re.fullmatch(r"status\.json\.[0-9a-f]{32}\.tmp", temporary_paths[0].name))
            self.assertFalse(temporary_paths[0].exists())
            with patch.object(WINDOWS_AGENT, "atomic_replace") as replace:
                with self.assertRaises(ValueError):
                    WINDOWS_AGENT.write(destination, dict(next=float("nan")))
            replace.assert_not_called()

    def test_controller_publication_uses_atomic_replace_and_sharing_only_budget(self):
        script = ipc_publish_script("C:\\Owned IPC\\command.json", b'{"one":true}', "ab" * 16, attempts=3)
        self.assertIn("[IO.File]::Replace($ipcSource,$ipcTarget,[NullString]::Value)", script)
        self.assertIn("[IO.File]::Move($ipcSource,$ipcTarget)", script)
        self.assertIn("$ipcCode -notin @(32,33)", script)
        self.assertIn("$ipcAttempt -lt 3", script)
        self.assertIn("after 3 attempts", script)
        self.assertNotIn("Move-Item", script)
        self.assertNotIn("Remove-Item", script)
        self.assertNotIn("Copy-Item", script)
        self.assertIn("command.json." + "ab" * 16 + ".tmp", script)
        for attempts in (0,25,True):
            with self.assertRaises(ValueError):
                ipc_publish_script("C:\\Owned IPC\\command.json", b"{}", "ab" * 16, attempts)

    def foreground(self, front=42, target=42, **changes):
        return dict(type="foreground_status", target_pid=target, frontmost_pid=front,
                    target_exists=True, target_terminated=False, target_active=True,
                    target_foreground=True, **changes)

    def test_foreground_confirmation_requires_exact_live_owned_pid(self):
        self.assertTrue(confirmed_owned_foreground(self.foreground(), 42))
        for name, value in (("target_pid", None), ("target_pid", 43), ("frontmost_pid", None),
                            ("frontmost_pid", 43), ("target_exists", False), ("target_terminated", True),
                            ("target_active", False), ("target_foreground", None)):
            event = self.foreground()
            event[name] = value
            self.assertFalse(confirmed_owned_foreground(event, 42), name)

    def test_foreground_wait_retries_before_measurement_and_preserves_status_diagnostics(self):
        client = Mock()
        client.proc.pid = 42
        observer = Mock()
        observer.foreground_status.side_effect = [dict(type="foreground_status_timeout", target_foreground=None),
                                                 self.foreground(front=43), self.foreground()]
        timeline = Mock()
        with patch("rdpbench.live.time.sleep"):
            result = wait_owned_foreground(client, observer, timeline, "preflight")
        self.assertEqual(result["frontmost_pid"], 42)
        self.assertEqual(client.send.call_count, 3)
        self.assertTrue(all(call.args == ("focus",) for call in client.send.call_args_list))
        statuses = [call.kwargs["status"] for call in timeline.emit.call_args_list if call.args[0] == "foreground_status"]
        self.assertEqual(statuses[1]["frontmost_pid"], 43)

    def test_foreground_wait_fails_bounded_with_last_exact_pid_evidence(self):
        client, observer, timeline = Mock(), Mock(), Mock()
        client.proc.pid = 42
        observer.foreground_status.return_value = self.foreground(front=43)
        with patch("rdpbench.live.time.sleep"), patch("rdpbench.live.time.monotonic", side_effect=[0,0,.01,.02,4]):
            with self.assertRaisesRegex(RuntimeError, '"frontmost_pid": 43'):
                wait_owned_foreground(client, observer, timeline, "measurement")
        self.assertEqual(client.send.call_count, 1)

    def test_foreground_rpc_matches_request_id_and_unknown_timeout_cannot_pass(self):
        observer = object.__new__(Observer)
        observer.foreground_replies = {"unrelated": self.foreground()}
        observer.lock = threading.Lock()
        observer.check_live = Mock()
        sink = io.StringIO()
        observer.proc = SimpleNamespace(stdin=sink)
        with patch("rdpbench.live.time.sleep"), patch("rdpbench.live.time.monotonic", side_effect=[0,0,.4]):
            status = observer.foreground_status(timeout=.3)
        self.assertEqual(status["type"], "foreground_status_timeout")
        self.assertIsNone(status["target_foreground"])
        self.assertIn("unrelated", observer.foreground_replies)
        request = json.loads(sink.getvalue())
        self.assertEqual(request["request_id"], status["request_id"])
        self.assertEqual(request["op"], "foreground_status")


if __name__ == "__main__":
    unittest.main()
