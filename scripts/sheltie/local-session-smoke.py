#!/usr/bin/env python3
"""Connect to FreeRDP's synthetic sample desktop over a loopback-only relay.

Uses temporary certificates and synthetic credentials. Does not contact a remote
machine, save certificate trust, or access the user's Keychain.
"""
import argparse
import base64
import importlib.util
import json
from datetime import datetime, timezone
import traceback
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import struct
import tempfile
import threading
import time
import uuid

spec = importlib.util.spec_from_file_location("bridge_smoke", Path(__file__).with_name("bridge-smoke.py"))
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class Relay:
    """Socket ownership stays with each forwarder; listener shutdown is independent."""
    def __init__(self, unix_path):
        self.unix_path = unix_path
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.connections = []
        self.orphans = []
        self.lock = threading.Lock()
        self.accepting = True
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while self.accepting:
            try:
                client, _ = self.listener.accept()
            except (TimeoutError, OSError):
                continue
            upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                upstream.connect(self.unix_path)
            except OSError:
                client.close()
                upstream.close()
                continue
            connection = dict(client=client, upstream=upstream, drop=threading.Event(),
                              done=threading.Event(), preserve=False, abortive=False)
            with self.lock:
                if not self.accepting:
                    client.close()
                    upstream.close()
                    continue
                self.connections.append(connection)
            threading.Thread(target=self.forward, args=(connection,), daemon=True).start()

    def forward(self, connection):
        left, right = connection["client"], connection["upstream"]
        left.settimeout(0.5)
        right.settimeout(0.5)
        try:
            while not connection["drop"].is_set():
                readable, _, _ = select.select([left, right], [], [], 0.02)
                for source in readable:
                    if connection["drop"].is_set():
                        break
                    data = source.recv(65536)
                    if not data:
                        return
                    (right if source is left else left).sendall(data)
        except (OSError, ValueError):
            pass
        finally:
            try:
                if connection["abortive"]:
                    left.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                else:
                    left.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            left.close()
            with self.lock:
                if connection["preserve"]:
                    self.orphans.append(right)
                else:
                    right.close()
                self.connections.remove(connection)
            connection["done"].set()

    def drop_connections(self, preserve_upstreams=False, abortive=False):
        with self.lock:
            connections = list(self.connections)
            for connection in connections:
                connection["preserve"] = preserve_upstreams
                connection["abortive"] = abortive
                connection["drop"].set()
        for connection in connections:
            if not connection["done"].wait(2):
                raise AssertionError("Relay forwarder did not complete the requested disconnect")

    def stop_accepting(self):
        self.accepting = False
        self.listener.close()
        self.thread.join(timeout=1)

    def close(self):
        self.stop_accepting()
        self.drop_connections()
        with self.lock:
            for upstream in self.orphans:
                upstream.close()
            self.orphans.clear()


def respond(client, message, reject_certificate=False, cancel_auth=False):
    if message["type"] == "certificate_request":
        assert message.get("fingerprint"), "Certificate prompt must include a verifiable fingerprint"
        client.send("certificate_response", requestId=message["requestId"],
                    decision="reject" if reject_certificate else "accept_once")
    elif message["type"] == "auth_request":
        client.send("auth_response", requestId=message["requestId"], accepted=not cancel_auth,
                    username="launcher-test", domain="", password="synthetic-test-password")


def connect(client, relay, *, retries=0, reject_certificate=False, cancel_auth=False, thumbnails=False, fullscreen=False,
            reverse_mouse_wheel=False, performance=False, dynamic_resolution=False, window_geometry=False):
    hello = client.hello()
    if performance:
        assert "performance_monitoring" in hello["capabilities"]
        assert "performance_overlay_styles" in hello["capabilities"]
    if reverse_mouse_wheel:
        assert "reverse_mouse_wheel" in hello["capabilities"], "Missing mouse wheel reversal capability"
    if thumbnails:
        assert "session_thumbnail" in hello["capabilities"], "Missing thumbnail capability"
    arguments = [f"/v:127.0.0.1:{relay.port}", "/u:launcher-test", "/f" if fullscreen else "/size:800x600",
                 "/rfx", "/timeout:5000", "-multitransport"]
    if dynamic_resolution:
        arguments.append("+dynamic-resolution")
    arguments += ["+auto-reconnect", f"/auto-reconnect-max-retries:{retries}"] if retries else ["-auto-reconnect"]
    geometry = {}
    if window_geometry:
        assert "window_geometry" in hello["capabilities"], "Missing window geometry capability"
        assert not fullscreen, "Window geometry smoke checks need a windowed session"
        geometry = dict(rememberWindowSize=True, windowSize=dict(width=900, height=650))
    client.send("start", arguments=arguments, displaySelections=[], thumbnailsEnabled=thumbnails,
                reverseMouseWheel=reverse_mouse_wheel, performanceSupported=performance, **geometry)
    observed = []
    while True:
        message = client.receive()
        observed.append(message["type"])
        respond(client, message, reject_certificate, cancel_auth)
        if message["type"] == "state" and message.get("phase") == "connected":
            return message, observed
        if message["type"] == "ended":
            return message, observed


def confirm_frame(client, expected_geometry=None):
    """A post-reconnect connected event alone does not prove the server can render."""
    request_id = str(uuid.uuid4())
    client.send("thumbnail_request", requestId=request_id)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        client.socket.settimeout(max(0.1, deadline - time.monotonic()))
        event = client.receive()
        respond(client, event)
        assert event["type"] != "ended", event
        if event["type"] == "window_geometry" and expected_geometry is not None:
            assert event["windowSize"] == expected_geometry, "Restored window size did not remain stable"
        if event["type"] == "thumbnail" and event.get("requestId") == request_id:
            assert base64.b64decode(event.get("pngBase64", ""), validate=True).startswith(b"\x89PNG\r\n\x1a\n")
            client.socket.settimeout(30)
            return event
    raise AssertionError("No frame received after recovery")


def run_case(binary, sample_server, only_case, *, dock_single=False, fullscreen=False, reverse_mouse_wheel=False, dynamic_resolution=False, diagnostics=None, sample_server_icon=None):
    trace = diagnostics.event if diagnostics else (lambda *args, **kwargs: None)
    with tempfile.TemporaryDirectory(prefix="rdp-launcher-", dir="/private/tmp") as directory:
        folder = Path(directory)
        certificate, key = folder / "server.crt", folder / "server.key"
        icon = Path(sample_server_icon) if sample_server_icon else Path(sample_server).resolve().parent.parent / "Resources/test_icon.bmp"
        if not icon.is_file():
            raise FileNotFoundError("Sample server needs test_icon.bmp; use the staged server or --sample-server-icon")
        shutil.copy2(icon, folder / "test_icon.bmp")
        subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(certificate), "-days", "1",
                        "-subj", "/CN=localhost"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       env={key: value for key, value in os.environ.items() if not key.startswith("OPENSSL_")})
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            label = reservation.getsockname()[1]
        environment = bridge.runtime_environment(sample_server, dict(os.environ, TMPDIR=str(folder) + "/"))
        with (diagnostics.folder.joinpath("server.log").open("w+b") if diagnostics else tempfile.TemporaryFile()) as log:
            server = subprocess.Popen([sample_server, "--local-only", f"--port={label}",
                                       f"--cert={certificate}", f"--key={key}"], cwd=folder,
                                      env=dict(environment, WLOG_LEVEL="DEBUG") if diagnostics else environment, stdout=log, stderr=log)
            trace("server_started", pid=server.pid)
            relay = None
            socket_path = None
            try:
                candidates = [folder / f"tfreerdp-server.{label}", Path("/tmp") / f"tfreerdp-server.{label}"]
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and server.poll() is None:
                    socket_path = next((path for path in candidates if path.exists()), None)
                    if socket_path:
                        break
                    time.sleep(0.05)
                if not socket_path:
                    log.seek(0)
                    raise AssertionError("Sample server did not start: " + log.read().decode(errors="replace")[-3000:])
                relay = Relay(str(socket_path))

                cases = ("connect_cancel", "reopen") if only_case == "reopen" else (only_case,)
                for case in cases:
                    client = bridge.Client(binary, environment=dict(os.environ, XDG_CONFIG_HOME=str(folder / "config"), WLOG_LEVEL="DEBUG" if diagnostics else "WARN"),
                                           dock_single=dock_single,
                                           log_path=diagnostics.folder / f"client-{case}.log" if diagnostics else None, trace=trace)
                    client.socket.settimeout(30)
                    try:
                        message, observed = connect(client, relay, retries=2 if case in ("recover", "exhaust_retries") else 0,
                                                    reject_certificate=case == "reject_certificate",
                                                    cancel_auth=case == "cancel_auth", thumbnails=case in ("thumbnail", "recover", "window_geometry"),
                                                    fullscreen=fullscreen, reverse_mouse_wheel=reverse_mouse_wheel, performance=case == "performance", dynamic_resolution=dynamic_resolution,
                                                    window_geometry=case == "window_geometry")
                        if case == "reject_certificate":
                            assert "certificate_request" in observed, observed
                            assert message["type"] == "ended", message
                        elif case == "cancel_auth":
                            assert "auth_request" in observed, observed
                            assert message["type"] == "ended", message
                            assert message["outcome"] == "cancelled", message
                        else:
                            assert message.get("phase") == "connected", message
                            assert "certificate_request" in observed, observed
                            if case == "window_geometry":
                                deadline = time.monotonic() + 10
                                while True:
                                    event = client.receive(timeout=max(0.1, deadline - time.monotonic()))
                                    respond(client, event)
                                    assert event["type"] != "ended", event
                                    if event["type"] == "window_geometry":
                                        assert event["windowSize"] == dict(width=900, height=650), event
                                        break
                                    assert time.monotonic() < deadline, "No window geometry received"
                                expected = dict(width=900, height=650)
                                frame = confirm_frame(client, expected_geometry=expected)
                                if not dynamic_resolution:
                                    assert abs(frame["width"] / frame["height"] - 800 / 600) < 0.02, "Restoring local size changed fixed remote resolution"
                                until = time.monotonic() + 1
                                while time.monotonic() < until:
                                    try:
                                        event = client.receive(timeout=until - time.monotonic())
                                    except TimeoutError:
                                        break
                                    respond(client, event)
                                    assert event["type"] != "ended", event
                                    if event["type"] == "window_geometry":
                                        assert event["windowSize"] == expected, "Restored window size did not remain stable"
                                client.send("cancel")
                                terminal, _ = client.ended()
                                assert terminal["outcome"] == "cancelled", terminal
                            elif case == "performance":
                                client.send("performance_control", performanceEnabled=True, performanceOverlayVisible=True, performanceGeneration=1)
                                records = []
                                deadline = time.monotonic() + 12
                                while len(records) < 3 and time.monotonic() < deadline:
                                    item = client.receive(timeout=max(0.1, deadline-time.monotonic()))
                                    respond(client, item)
                                    assert item["type"] != "ended", item
                                    if item["type"] == "performance_sample":
                                        sample = item["performance"]
                                        assert sample["version"] == 1 and sample["generation"] == 1
                                        assert sample["connected"] and sample["intervalSeconds"] > 0
                                        assert sample["displays"] and all(d["width"] > 0 and d["height"] > 0 for d in sample["displays"])
                                        assert all(sample["traffic"][key] is None or sample["traffic"][key] >= 0 for key in sample["traffic"])
                                        assert all(d["framesPerSecond"] == d["updatesPerSecond"] for d in sample["displays"])
                                        bandwidth = sample["bandwidth"]
                                        assert bandwidth["source"] in ("rdp_estimate", "rdp_measurement", "unavailable")
                                        assert isinstance(bandwidth["stale"], bool)
                                        assert bandwidth["kilobitsPerSecond"] is None or bandwidth["kilobitsPerSecond"] > 0
                                        assert sample["decode"]["source"] == "codec_wall_time"
                                        assert sample["decode"]["recentSampleCount"] <= 256
                                        assert "packetLossPercentage" not in sample["udpReliability"]
                                        assert all(d["pacing"]["renderStalls"] >= 0 for d in sample["displays"])
                                        records.append(sample)
                                assert len(records) == 3, "No live performance samples"
                                assert records[-1]["sequence"] > records[0]["sequence"]
                                assert records[-1]["rtt"]["source"] in ("rdp", "tcp_estimate", "unavailable")
                                if diagnostics:
                                    (diagnostics.folder / "performance.json").write_text(json.dumps(records, indent=2))
                                # A style-only runtime control must preserve counter epochs and sampling.
                                for style, generation in (("quake", 2), ("panel", 3)):
                                    client.send("performance_control", performanceEnabled=True, performanceOverlayVisible=True,
                                                performanceGeneration=generation, performanceOverlayStyle=style)
                                    deadline = time.monotonic() + 5
                                    while time.monotonic() < deadline:
                                        item = client.receive(timeout=max(0.1, deadline-time.monotonic()))
                                        respond(client, item)
                                        assert item["type"] != "ended", item
                                        if item["type"] == "performance_sample" and item["performance"]["generation"] == generation:
                                            current = item["performance"]
                                            assert current["epoch"] == records[-1]["epoch"], "Style change reset counters"
                                            assert current["sequence"] > records[-1]["sequence"]
                                            records.append(current)
                                            break
                                    else:
                                        raise AssertionError("Style control interrupted performance sampling")
                                client.send("performance_control", performanceEnabled=False, performanceOverlayVisible=False, performanceGeneration=4)
                                until = time.monotonic() + 2.2
                                while time.monotonic() < until:
                                    try:
                                        item = client.receive(timeout=until-time.monotonic())
                                    except TimeoutError:
                                        break
                                    if item["type"] == "performance_sample":
                                        assert item["performance"]["generation"] < 4, "Sampling continued after pause"
                                    respond(client, item)
                                client.send("performance_control", performanceEnabled=True, performanceOverlayVisible=False, performanceGeneration=5, performanceOverlayStyle="quake")
                                while True:
                                    item = client.receive(timeout=5)
                                    if item["type"] == "performance_sample" and item["performance"]["generation"] == 5:
                                        assert item["performance"]["epoch"] > records[-1]["epoch"]
                                        break
                                    respond(client, item)
                                client.send("cancel")
                                terminal, _ = client.ended()
                                assert terminal["outcome"] == "cancelled"
                            elif case == "thumbnail":
                                request_id = str(uuid.uuid4())
                                client.send("thumbnail_request", requestId=request_id)
                                deadline = time.monotonic() + 15
                                while time.monotonic() < deadline:
                                    client.socket.settimeout(max(0.1, deadline - time.monotonic()))
                                    event = client.receive()
                                    assert event["type"] != "ended", f"Thumbnail session ended unexpectedly: {event}"
                                    if event["type"] != "thumbnail":
                                        respond(client, event)
                                        continue
                                    assert event.get("requestId") == request_id, "Mismatched thumbnail request"
                                    encoded = event.get("pngBase64", "")
                                    assert 0 < len(encoded) <= 350000, "Oversized thumbnail encoding"
                                    image = base64.b64decode(encoded, validate=True)
                                    assert 24 <= len(image) <= 262144, "Oversized thumbnail PNG"
                                    assert image[:8] == b"\x89PNG\r\n\x1a\n", "Invalid thumbnail PNG signature"
                                    assert image[12:16] == b"IHDR", "Missing PNG dimensions"
                                    width, height = struct.unpack(">II", image[16:24])
                                    assert 1 <= width <= 320 and 1 <= height <= 200, "Unbounded thumbnail dimensions"
                                    assert (event.get("width"), event.get("height")) == (width, height), "Incorrect thumbnail dimensions"
                                    break
                                else:
                                    raise AssertionError("No thumbnail arrived")
                                client.socket.settimeout(30)
                                client.send("thumbnail_request", thumbnailsEnabled=False)
                                client.send("cancel")
                                terminal, remaining = client.ended()
                                assert terminal["outcome"] == "cancelled", terminal
                                assert not any(item["type"] == "thumbnail" for item in remaining), "Capture continued after disabling"
                            elif case == "recover":
                                trace("disconnect_injected", mode="recover")
                                relay.drop_connections()
                                had_retry = False
                                while True:
                                    event = client.receive()
                                    respond(client, event)
                                    had_retry |= event["type"] == "retry"
                                    assert event["type"] != "ended", event
                                    if event["type"] == "state" and event.get("phase") == "connected":
                                        assert had_retry, "Expected a reconnect attempt"
                                        break
                                confirm_frame(client)
                                client.send("cancel")
                                terminal, _ = client.ended()
                                assert terminal["outcome"] == "cancelled", terminal
                            elif case == "exhaust_retries":
                                trace("disconnect_injected", mode="exhaust_retries")
                                relay.stop_accepting()
                                relay.drop_connections(preserve_upstreams=True, abortive=True)
                                # Two 5s connects plus two 5s retry delays already exceed
                                # Client.ended's normal 15s budget. Allow bounded runner
                                # scheduling/teardown overhead; still require exhaustion.
                                terminal, events = client.ended(timeout=90)
                                assert any(event["type"] == "retry" for event in events), events
                                assert terminal["outcome"] == "retries_exhausted", terminal
                                assert terminal["hadConnected"], terminal
                            else:
                                client.send("focus")
                                client.send("cancel")
                                terminal, _ = client.ended()
                                assert terminal["outcome"] == "cancelled", terminal
                        client.wait()
                        print(f"PASS {case}: synthetic local RDP desktop")
                    except Exception:
                        client.log.seek(0)
                        print("Synthetic client diagnostics:\n" + client.log.read().decode(errors="replace")[-5000:])
                        log.seek(0)
                        print("Synthetic server diagnostics:\n" + log.read().decode(errors="replace")[-5000:])
                        raise
                    finally:
                        client.close()
                        trace("client_exited", returncode=client.process.returncode)
            finally:
                if relay:
                    relay.close()
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
                trace("server_exited", returncode=server.returncode)
                if socket_path and socket_path.exists():
                    socket_path.unlink()


RELEASE_CASES = ("reject_certificate", "cancel_auth", "connect_cancel", "thumbnail", "reopen")
RECONNECT_CASES = ("recover", "exhaust_retries")
PERFORMANCE_CASES = ("performance",)
WINDOW_CASES = ("window_geometry",)
SUITES = {"release": RELEASE_CASES, "reconnect": RECONNECT_CASES, "performance": PERFORMANCE_CASES, "window": WINDOW_CASES,
          "all": RELEASE_CASES + RECONNECT_CASES + PERFORMANCE_CASES + WINDOW_CASES}


class Diagnostics:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.folder.mkdir(parents=True, exist_ok=False)
        self.lock = threading.Lock()

    def event(self, kind, **fields):
        def redact(value):
            if isinstance(value, dict):
                return {k: ("<omitted>" if k in ("password", "pngBase64", "tlsSecretsFile") else redact(v)) for k, v in value.items()}
            if isinstance(value, list):
                return [redact(item) for item in value]
            return value
        record = dict(time=datetime.now(timezone.utc).isoformat(), monotonic_ns=time.monotonic_ns(),
                      event=kind, **redact(fields))
        with self.lock, (self.folder / "events.jsonl").open("a") as output:
            output.write(json.dumps(record) + "\n")


def run(binary, sample_server, only_case=None, *, suite="all", repeat=1, diagnostics_dir=None, **options):
    if repeat < 1:
        raise ValueError("repeat must be positive")
    cases = (only_case,) if only_case else SUITES[suite]
    root = Path(diagnostics_dir) if diagnostics_dir else Path(tempfile.mkdtemp(prefix="sheltie-smoke-results-"))
    root.mkdir(parents=True, exist_ok=True)
    results = []
    for case in cases:
        for iteration in range(1, repeat + 1):
            diagnostic = Diagnostics(root / f"{case}-{iteration:03}")
            started = time.monotonic()
            result = dict(case=case, iteration=iteration, passed=False)
            diagnostic.event("attempt_started", case=case, iteration=iteration)
            try:
                run_case(binary, sample_server, case, diagnostics=diagnostic, **options)
                result["passed"] = True
            except Exception as error:
                result["error"] = str(error)
                (diagnostic.folder / "failure.txt").write_text(traceback.format_exc())
                print(f"FAIL {case} attempt {iteration}: {error}", flush=True)
            result["duration_seconds"] = time.monotonic() - started
            diagnostic.event("attempt_finished", **result)
            (diagnostic.folder / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
            (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"Diagnostics: {root.resolve()}", flush=True)
    if any(not result["passed"] for result in results):
        raise AssertionError(f"{sum(not r['passed'] for r in results)}/{len(results)} smoke attempts failed; see {root}")
    return results


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--sample-server", default=shutil.which("sfreerdp-server"))
    parser.add_argument("--sample-server-icon", type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--case", choices=SUITES["all"])
    selection.add_argument("--suite", choices=SUITES, default="all")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--diagnostics-dir", type=Path)
    parser.add_argument("--dock-single", action="store_true")
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--dynamic-resolution", action="store_true")
    parser.add_argument("--reverse-mouse-wheel", action="store_true")
    return parser


if __name__ == "__main__":
    parser = argument_parser()
    options = parser.parse_args()
    if not options.sample_server:
        parser.error("Provide --sample-server /path/to/sfreerdp-server")
    if options.repeat < 1:
        parser.error("--repeat must be positive")
    run(os.path.abspath(options.binary), os.path.abspath(options.sample_server), options.case,
        suite=options.suite, repeat=options.repeat, diagnostics_dir=options.diagnostics_dir,
        sample_server_icon=options.sample_server_icon,
        dock_single=options.dock_single, fullscreen=options.fullscreen, reverse_mouse_wheel=options.reverse_mouse_wheel, dynamic_resolution=options.dynamic_resolution)
