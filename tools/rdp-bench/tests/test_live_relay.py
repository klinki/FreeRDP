"""Real socket checks for the scoped live relay and independent lease child."""
import argparse
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from live_relay import LiveRelay, adapter, encoded, rpc
from rdpbench.link import Link


def profile(**settings):
    direction = dict(rate_mbps=10, one_way_ms=15, jitter_ms=0, loss=0,
                     queue_ms=30, overhead_bytes=28, duplicate=0, reorder=0)
    direction.update(settings)
    return dict(c2s=dict(direction), s2c=dict(direction))


class LiveRelayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.relays = []
        self.children = []
        self.sockets = []

    def tearDown(self):
        for relay, thread in self.relays:
            relay.stop_reason = "stopped"
            thread.join(5)
            self.assertFalse(thread.is_alive())
        for args, run_id in self.children:
            with contextlib.suppress(Exception):
                args.request = encoded(dict(op="restore", run_id=run_id))
                adapter(args)
        for value in self.sockets:
            value.close()
        self.temp.cleanup()

    def udp_target(self):
        value = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        value.bind(("127.0.0.1", 0))
        value.settimeout(3)
        self.sockets.append(value)
        return value

    def relay(self, port, p=None):
        folder = self.folder/("relay-"+str(len(self.relays)))
        folder.mkdir()
        config = dict(profile=p or profile(), seed=29, lease_s=10, lease_id="fixture",
                      run_id="test", profile_id="fixture", token="fixture",
                      listen_host="127.0.0.1", listen_port=0, target_host="127.0.0.1", target_port=port,
                      ledger_path=str(folder/"relay.jsonl"), status_path=str(folder/"status.json"))
        relay = LiveRelay(config)
        thread = threading.Thread(target=relay.run)
        self.relays.append((relay, thread))
        thread.start()
        return relay

    def test_protected_udp_bytes_and_separate_client_return_paths(self):
        target = self.udp_target()
        relay = self.relay(target.getsockname()[1])
        # Payloads include zero bytes and arbitrary protected prefixes. The
        # relay must never alter any payload or pair two clients' return paths.
        clients = [self.udp_target(), self.udp_target()]
        payloads = [b"\x00\xff\x00protected"+bytes(range(256))*4,
                    b"\xff\x00\xffprotected"+bytes(reversed(range(256)))*4]
        for client, payload in zip(clients, payloads):
            client.sendto(payload, ("127.0.0.1", relay.port))
        peers = {}
        for _ in clients:
            data, peer = target.recvfrom(65535)
            self.assertIn(data, payloads)
            peers[data] = peer
        self.assertNotEqual(*peers.values())
        for payload in reversed(payloads):
            target.sendto(payload, peers[payload])
        for client, payload in zip(clients, payloads):
            data, source = client.recvfrom(65535)
            self.assertEqual(data, payload)
            self.assertEqual(source, ("127.0.0.1", relay.port))
        summary = relay.snapshot()
        self.assertTrue(summary["udp_counter_hits_both_directions"])
        self.assertFalse(summary["tcp_impaired"])
        self.assertFalse(summary["full_rdp_profile"])
        for direction in ("c2s", "s2c"):
            self.assertEqual(summary[direction]["received_datagrams"], 2)
            self.assertTrue(summary[direction]["packet_accounting"]["balanced"])

    def test_live_deadlines_include_ten_mbit_serialization_both_directions(self):
        target = self.udp_target()
        relay = self.relay(target.getsockname()[1])
        client = self.udp_target()
        payload = b"p"*1222  # 1250 IPv4+UDP bytes = exactly 1 ms at 10 Mbit/s
        begin = time.monotonic_ns()
        client.sendto(payload, ("127.0.0.1", relay.port))
        data, peer = target.recvfrom(65535)
        target.sendto(data, peer)
        self.assertEqual(client.recvfrom(65535)[0], payload)
        self.assertGreaterEqual(time.monotonic_ns()-begin, 32_000_000)
        relay.stop_reason = "stopped"
        self.relays[-1][1].join(5)
        rows = [json.loads(line) for line in Path(relay.config["ledger_path"]).read_text().splitlines()]
        delivered = [row for row in rows if row["type"] == "dispatch"]
        self.assertEqual(len(delivered), 2)
        for row in delivered:
            self.assertEqual(row["due_ns"]-row["offered_ns"], 16_000_000)
            self.assertGreaterEqual(row["actual_dispatch_ns"], row["due_ns"])
            self.assertEqual(row["lateness_ns"], row["actual_dispatch_ns"]-row["due_ns"])
            self.assertNotIn("payload", row)
            self.assertEqual(row["udp_bytes"], len(payload))

    def test_tcp_stream_is_exact_and_half_close_reliably_drains(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        self.sockets.append(listener)
        received = bytearray()
        error = []
        response = b"response\x00"*200000
        def server():
            try:
                with listener.accept()[0] as value:
                    value.settimeout(5)
                    while True:
                        chunk = value.recv(65536)
                        if not chunk:
                            break
                        received.extend(chunk)
                    value.sendall(response)
                    value.shutdown(socket.SHUT_WR)
            except BaseException as value:
                error.append(value)
        server_thread = threading.Thread(target=server)
        server_thread.start()
        # Total UDP loss cannot affect the independently reliable TCP proxy.
        relay = self.relay(listener.getsockname()[1], profile(loss=1))
        request = bytes(range(256))*12000
        with socket.create_connection(("127.0.0.1", relay.port), timeout=5) as client:
            client.settimeout(5)
            client.sendall(request)
            client.shutdown(socket.SHUT_WR)
            result = bytearray()
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                result.extend(chunk)
        server_thread.join(5)
        self.assertFalse(server_thread.is_alive())
        self.assertEqual(error, [])
        self.assertEqual(hashlib.sha256(received).digest(), hashlib.sha256(request).digest())
        self.assertEqual(result, response)
        summary = relay.snapshot()
        self.assertEqual(summary["tcp"]["c2s"]["forwarded_bytes"], len(request))
        self.assertEqual(summary["tcp"]["s2c"]["forwarded_bytes"], len(response))
        self.assertEqual(summary["c2s"]["received_datagrams"], 0)

    def args(self, request, target_port, listen_port=0):
        return argparse.Namespace(state=str(self.folder/"state.json"), output_dir=str(self.folder/"leases"),
                                  listen_host="127.0.0.1", listen_port=listen_port,
                                  target_host="127.0.0.1", target_port=target_port, request=encoded(request))

    def test_adapter_live_child_owner_guard_and_restore_then_new_lease(self):
        target = self.udp_target()
        request = dict(op="apply", run_id="first", profile_id="ten_mbit_clean", profile=profile(), seed=47,
                       lease_s=5, trial_dir=str(self.folder/"trial"))
        args = self.args(request, target.getsockname()[1])
        receipt = adapter(args)
        self.children.append((args, "first"))
        self.assertTrue(receipt["verified"])
        self.assertTrue(receipt["active"])
        self.assertTrue(Path(receipt["capturepath"]).is_file())
        args.request = encoded(dict(op="apply", **{key: value for key, value in request.items() if key != "op"}))
        with self.assertRaisesRegex(RuntimeError, "live relay"):
            adapter(args)
        args.request = encoded(dict(op="restore", run_id="unrelated"))
        with self.assertRaisesRegex(RuntimeError, "does not own"):
            adapter(args)
        args.request = encoded(dict(op="status", run_id="first", profile_id="ten_mbit_clean"))
        self.assertTrue(adapter(args)["active"])
        args.request = encoded(dict(op="restore", run_id="first"))
        restored = adapter(args)
        self.assertEqual(restored["state"], "restored")
        self.assertTrue(restored["sockets_closed"])
        self.assertTrue(restored["ledger"]["closed"])
        self.assertTrue(restored["ledger"]["valid"])
        # Repeatable campaigns need to reclaim the same local TCP/UDP port.
        args.listen_port = receipt["listen"][1]
        request.update(run_id="second")
        args.request = encoded(request)
        second = adapter(args)
        self.children.append((args, "second"))
        self.assertEqual(second["listen"], receipt["listen"])
        self.assertNotEqual(second["lease_id"], receipt["lease_id"])

    def test_child_lease_expires_independently_after_adapter_command_exits(self):
        target = self.udp_target()
        args = self.args(dict(op="apply", run_id="expire", profile_id="short", profile=profile(),
                             seed=11, lease_s=.4), target.getsockname()[1])
        command = [sys.executable, str(ROOT/"live_relay.py"), "adapter", "--state", args.state,
                   "--output-dir", args.output_dir, "--listen-port", "0", "--target-host", "127.0.0.1",
                   "--target-port", str(args.target_port), args.request]
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr+result.stdout)
        receipt = json.loads(result.stdout)
        self.children.append((args, "expire"))
        self.assertTrue(receipt["active"])
        time.sleep(.65)
        args.request = encoded(dict(op="status", run_id="expire"))
        closed = adapter(args)
        self.assertEqual(closed["state"], "expired")
        self.assertFalse(closed["active"])
        self.assertTrue(closed["sockets_closed"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(tuple(receipt["listen"]))
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(tuple(receipt["listen"]))

    def test_seeded_live_loss_and_pending_accounting_match_existing_link(self):
        target = self.udp_target()
        p = profile(loss=1, jitter_ms=1)
        relay = self.relay(target.getsockname()[1], p)
        client = self.udp_target()
        client.sendto(b"lost", ("127.0.0.1", relay.port))
        deadline = time.monotonic()+2
        while relay.links[0].n < 1 and time.monotonic() < deadline:
            time.sleep(.01)
        summary = relay.snapshot()
        self.assertEqual(summary["c2s"]["random_drops"], 1)
        self.assertEqual(summary["c2s"]["delivered_datagrams"], 0)
        self.assertTrue(summary["c2s"]["packet_accounting"]["balanced"])
        expected = Link(p["c2s"], 29, "c2s")
        expected.offer(0, b"lost")
        self.assertEqual(summary["c2s"]["random_drops"], expected.count["random_drops"])

    def test_schedule_replays_seeded_link_decisions_and_accounts_queue_drops(self):
        target = self.udp_target()
        folder = self.folder/"scheduled"
        folder.mkdir()
        p = profile(loss=.08, jitter_ms=1, duplicate=.06, reorder=.1, reorder_extra_ms=15)
        config = dict(profile=p, seed=71, lease_s=1, lease_id="scheduled", run_id="scheduled",
                      profile_id="scheduled", token="fixture", listen_host="127.0.0.1", listen_port=0,
                      target_host="127.0.0.1", target_port=target.getsockname()[1],
                      ledger_path=str(folder/"relay.jsonl"), status_path=str(folder/"status.json"))
        relay = LiveRelay(config)
        try:
            flow = relay.udp_flow(("127.0.0.1", 12345))
            expected = []
            # Burst past the modeled queue budget. Compare both independent
            # directions with the already established transport Link contract.
            for direction, seed in ((0, 71), (1, 71+1000003)):
                oracle = Link(p[("c2s", "s2c")[direction]], seed, ("c2s", "s2c")[direction])
                oracle.start_ns = relay.started_ns
                for i in range(200):
                    now = relay.started_ns+i*100000
                    data = bytes([i])*1222
                    for due, payload in oracle.offer(now, data):
                        expected.append((due, direction, payload))
                    with patch("live_relay.time.monotonic_ns", return_value=now):
                        relay.udp_offer(direction, flow, data)
                summary = relay.direction_summary(direction)
                self.assertGreater(summary["queue_drops"], 0)
                self.assertEqual(summary["random_drops"], oracle.count["random_drops"])
                self.assertEqual(summary["duplicates"], oracle.count["duplicates"])
                self.assertTrue(summary["packet_accounting"]["balanced"])
                self.assertEqual(summary["pending_udp_bytes"], summary["pending_datagrams"]*1222)
            observed = [(item[0], item[2], item[4]) for item in relay.heap]
            self.assertEqual(sorted(observed), sorted(expected))
        finally:
            result = relay.close()
        self.assertGreater(result["c2s"]["pending_datagrams"], 0)
        self.assertEqual(result["c2s"]["delivered_datagrams"], 0)
        self.assertTrue(result["ledger"]["closed"])

    def test_live_control_malformed_request_cannot_stop_the_relay(self):
        target = self.udp_target()
        relay = self.relay(target.getsockname()[1])
        for payload in (b"[]\n", b"not-json\n"):
            with socket.create_connection(relay.control.getsockname(), timeout=2) as value:
                value.sendall(payload)
                self.assertEqual(value.recv(1024), b"")
        self.assertIsNone(relay.stop_reason)
        self.assertIsNone(relay.error)
        client = self.udp_target()
        client.sendto(b"still-live", ("127.0.0.1", relay.port))
        self.assertEqual(target.recvfrom(65535)[0], b"still-live")

    def test_tcp_resets_require_explicit_owned_teardown_and_client_side(self):
        for peer_side, marked, expected_teardown in (("client", False, False),
                                                     ("client", True, True),
                                                     ("target", True, False)):
            with self.subTest(peer_side=peer_side, marked=marked):
                listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sockets.append(listener)
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                listener.settimeout(3)
                relay = self.relay(listener.getsockname()[1])
                client = socket.create_connection(("127.0.0.1", relay.port), timeout=3)
                self.sockets.append(client)
                target, peer = listener.accept()
                self.sockets.append(target)
                target.settimeout(3)
                # Prove establishment and a completely forwarded request.
                client.sendall(b"drained")
                self.assertEqual(target.recv(7), b"drained")
                if marked:
                    reference = dict(control=relay.control.getsockname(), token="fixture", run_id="test")
                    receipt = rpc(reference, "status", begin_teardown=True)
                    self.assertIsNotNone(receipt["teardown_started_ns"])
                victim = client if peer_side == "client" else target
                victim.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                victim.close()
                deadline = time.monotonic()+3
                while relay.tcp_counts["closed_connections"] < 1 and time.monotonic() < deadline:
                    time.sleep(.01)
                summary = relay.snapshot()
                self.assertEqual(summary["tcp_connections"]["socket_errors"], 1)
                self.assertEqual(summary["tcp_connections"]["active_socket_errors"], 0 if expected_teardown else 1)
                self.assertEqual(summary["tcp_connections"]["teardown_client_resets"], 1 if expected_teardown else 0)
                relay.stop_reason = "stopped"
                self.relays[-1][1].join(3)
                rows = list(map(json.loads, Path(relay.config["ledger_path"]).read_text().splitlines()))
                close = next(row for row in rows if row["type"] == "tcp_close")
                self.assertEqual(close["error_side"], peer_side)
                self.assertEqual(close["error_errno"], errno.ECONNRESET)
                self.assertEqual(close["error_class"], "owned_client_reset" if expected_teardown else "active_error")
                self.assertEqual(close["pending_bytes"], [0, 0])

    def test_pending_discard_is_an_active_error_even_after_owned_teardown(self):
        target = self.udp_target()
        folder = self.folder/"pending-error"
        folder.mkdir()
        config = dict(profile=profile(), seed=11, lease_s=1, lease_id="pending", run_id="pending",
                      profile_id="pending", token="fixture", listen_host="127.0.0.1", listen_port=0,
                      target_host="127.0.0.1", target_port=target.getsockname()[1],
                      ledger_path=str(folder/"relay.jsonl"), status_path=str(folder/"status.json"))
        relay = LiveRelay(config)
        try:
            relay.teardown_started_ns = time.monotonic_ns()
            connection = dict(id=1, sockets=[], out=[bytearray(b"undelivered"), bytearray()],
                              connecting=False, received=[0, 11], forwarded=[0, 0])
            relay.tcp[1] = connection
            relay.tcp_close(connection, "client reset", error_side=0, error_operation="receive",
                            error_errno=errno.ECONNRESET)
            summary = relay.snapshot()
            self.assertEqual(summary["tcp_connections"]["active_socket_errors"], 1)
            self.assertEqual(summary["tcp_connections"]["teardown_client_resets"], 0)
            self.assertEqual(summary["tcp_connections"]["discarded_pending_bytes"], 11)
        finally:
            relay.close()

    def test_due_packet_is_dispatched_before_the_next_arrival_batch(self):
        target = self.udp_target()
        client = self.udp_target()
        folder = self.folder/"fairness"
        folder.mkdir()
        config = dict(profile=profile(one_way_ms=0, rate_mbps=0), seed=11, lease_s=1,
                      lease_id="fairness", run_id="fairness", profile_id="fairness", token="fixture",
                      listen_host="127.0.0.1", listen_port=0, target_host="127.0.0.1",
                      target_port=target.getsockname()[1], ledger_path=str(folder/"relay.jsonl"),
                      status_path=str(folder/"status.json"))
        relay = LiveRelay(config)
        try:
            flow = relay.udp_flow(client.getsockname())
            relay.udp_offer(0, flow, b"expired-before-batch")
            client.sendto(b"new-arrival", ("127.0.0.1", relay.port))
            original = relay.udp_offer
            seen = []
            def observe(direction, current, payload):
                target.settimeout(.1)
                seen.append(target.recvfrom(65535)[0])
                original(direction, current, payload)
            with patch.object(relay, "udp_offer", side_effect=observe):
                relay.udp_read(True)
            self.assertEqual(seen, [b"expired-before-batch"])
        finally:
            relay.close()

    def test_status_quantiles_are_conservative_and_do_not_sort_link_samples(self):
        target = self.udp_target()
        folder = self.folder/"quantiles"
        folder.mkdir()
        config = dict(profile=profile(), seed=11, lease_s=1, lease_id="quantiles", run_id="quantiles",
                      profile_id="quantiles", token="fixture", listen_host="127.0.0.1", listen_port=0,
                      target_host="127.0.0.1", target_port=target.getsockname()[1],
                      ledger_path=str(folder/"relay.jsonl"), status_path=str(folder/"status.json"))
        relay = LiveRelay(config)
        try:
            flow = relay.udp_flow(("127.0.0.1", 12345))
            now = relay.started_ns
            with patch("live_relay.time.monotonic_ns", return_value=now):
                relay.udp_offer(0, flow, b"quantile")
            due = relay.heap[0][0]
            with patch("live_relay.time.monotonic_ns", return_value=due+1_499_999):
                relay.udp_dispatch()
            with patch.object(relay.links[0], "summary", side_effect=AssertionError("must not sort raw samples in loop")):
                summary = relay.snapshot()
            self.assertEqual(summary["c2s"]["lateness_ms"]["count"], 1)
            self.assertEqual(summary["c2s"]["lateness_ms"]["p99"], 1.5)
            self.assertEqual(summary["c2s"]["lateness_ms"]["max"], 1.499999)
            self.assertIn("upper bounds", summary["c2s"]["lateness_quantile_method"])
            self.assertTrue(summary["c2s"]["packet_accounting"]["balanced"])
        finally:
            relay.close()

    def test_expired_lease_never_dispatches_a_previously_scheduled_packet(self):
        target = self.udp_target()
        folder = self.folder/"expired-pending"
        folder.mkdir()
        config = dict(profile=profile(), seed=11, lease_s=1, lease_id="expired-pending",
                      run_id="expired-pending", profile_id="expired-pending", token="fixture",
                      listen_host="127.0.0.1", listen_port=0, target_host="127.0.0.1",
                      target_port=target.getsockname()[1], ledger_path=str(folder/"relay.jsonl"),
                      status_path=str(folder/"status.json"))
        relay = LiveRelay(config)
        try:
            flow = relay.udp_flow(("127.0.0.1", 12345))
            relay.udp_offer(0, flow, b"pending-at-expiry")
            with patch("live_relay.time.monotonic_ns", return_value=relay.deadline_ns+1):
                relay.udp_dispatch()
            self.assertEqual(relay.stop_reason, "expired")
            self.assertEqual(relay.links[0].count["delivered_datagrams"], 0)
            self.assertEqual(relay.direction_summary(0)["pending_datagrams"], 1)
            self.assertTrue(relay.direction_summary(0)["packet_accounting"]["balanced"])
        finally:
            relay.close("expired")

    def test_adapter_rejects_stale_ownership_and_bind_collision_without_claiming_ready(self):
        target = self.udp_target()
        occupied = self.udp_target()
        args = self.args(dict(op="apply", run_id="blocked", profile_id="fixture", profile=profile(),
                             seed=11, lease_s=1), target.getsockname()[1], occupied.getsockname()[1])
        with self.assertRaisesRegex(RuntimeError, "failed before bind"):
            adapter(args)
        self.assertFalse(Path(args.state).exists())
        status = json.loads(next((self.folder/"leases").glob("*/status.json")).read_text())
        self.assertFalse(status["verified"])
        self.assertEqual(status["state"], "failed")
        Path(args.state).write_text(json.dumps(dict(control=["127.0.0.1", 1], run_id="blocked",
            lease_id="missing", status_path=str(self.folder/"missing"), token="missing")))
        with self.assertRaisesRegex(RuntimeError, "refusing to overwrite"):
            adapter(args)


if __name__ == "__main__":
    unittest.main()
