"""Live SDL launcher adapter. Authentication responses are never journaled."""
from __future__ import annotations

import json
import os
import re
import socket
import subprocess
import threading
import time
import uuid
from pathlib import Path

from .common import Journal

MANAGED_BLOCKED_KEYS = {
    "p", "password", "gp", "gateway", "g", "pth", "from-stdin", "args-from",
    "credentials-delegation", "reconnect-cookie", "assistance", "gat", "proxy",
    "auth-only", "smartcard", "smartcard-logon", "aad", "azure", "remote-app",
    "app", "load-balance-info", "tune", "cert", "cert-ignore", "cert-tofu",
    "cert-name", "cert-deny", "sec-ext", "restricted-admin", "restrictedadmin",
    "remote-credential-guard", "authentication", "force-console-callbacks", "list",
    "help", "version", "buildconfig", "monitors", "sdl-monitor-scale", "launcher-fd",
    "launcher-session", "log-level", "log-filters", "tls-secrets-file",
}


def sensitive_env_name(name):
    lowered = name.lower()
    return any(word in lowered for word in ("password", "secret", "credential")) or lowered.endswith(("_pass", "_token", "_key"))


def child_environment(lab=None, extra=None):
    """Only the controller keeps the RDP secret; no spawned helper inherits it."""
    result = dict(os.environ)
    result.update(extra or {})
    names = {"RDP_BENCH_PASSWORD", "RDP_TEST_PASSWORD"}
    if lab and lab.get("password_env"):
        names.add(lab["password_env"])
    return {name: value for name, value in result.items() if name not in names and not sensitive_env_name(name)}


def managed_argument(value):
    """Mirror the current native managed-mode guard before launching a client."""
    if not isinstance(value, str) or not value or value[0] not in "/+-" or "\0" in value or len(value.encode()) > 8192:
        return False
    text = value.lstrip("/+-")
    if not text:
        return False
    parts = re.split(r"[:=]", text, maxsplit=1)
    key, options = parts[0].lower(), parts[1].lower() if len(parts) == 2 else ""
    if key in MANAGED_BLOCKED_KEYS or key.startswith("launcher"):
        return False
    if key == "sec" and ("ext" in options or "aad" in options):
        return False
    return not (key == "tls" and "secrets" in options)


def fingerprint(value):
    value = re.sub(r"^sha256[:\s]*", "", str(value), flags=re.I)
    value = re.sub(r"[:\s]", "", value).lower()
    return value if re.fullmatch(r"[0-9a-f]{64}", value) else None


class Client:
    """One owned client, bounded received telemetry and pinned certificate trust."""

    EVENT_CAPACITY = 240000

    def __init__(self, binary, lab, folder, env=None, tls_secrets_file=None):
        if len(lab["rdp_arguments"]) > 256 or any(not managed_argument(value) for value in lab["rdp_arguments"]):
            raise ValueError("unsupported managed RDP arguments; use bridge fields for credentials/displays/TLS secrets and WLOG environment for logging")
        self.lab = lab
        self.sid = uuid.uuid4().hex
        self.connected = threading.Event()
        self.ready = threading.Event()
        self.ended = threading.Event()
        self.failed = None
        self.events = []
        self.overflow = 0
        self.lock = threading.Lock()
        self.sendlock = threading.Lock()
        self.tls_secrets_file = tls_secrets_file
        self.pin_seen = False
        self.pin_fingerprint = None
        self.cert_store = Path(folder) / "client-config"
        self.cert_store.mkdir(mode=0o700, exist_ok=False)
        self.journal = Journal(Path(folder) / "bridge.jsonl")
        parent, child = socket.socketpair()
        self.sock = parent
        child_env = child_environment(lab, env)
        child_env["XDG_CONFIG_HOME"] = str(self.cert_store.resolve())
        self.err = open(Path(folder) / "client.stderr", "w", encoding="utf-8")
        try:
            self.proc = subprocess.Popen(
                [str(binary), f"/launcher-fd:{child.fileno()}", f"/launcher-session:{self.sid}"],
                pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=self.err, env=child_env)
        except BaseException:
            self.sock.close()
            self.err.close()
            self.journal.close()
            raise
        finally:
            child.close()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def send(self, kind, **data):
        frame = json.dumps({"v": 1, "sessionId": self.sid, "type": kind, **data},
                           allow_nan=False).encode() + b"\n"
        if len(frame) > 1024 * 1024:
            raise ValueError("oversized outgoing bridge frame")
        with self.sendlock:
            self.sock.sendall(frame)

    def handle(self, event, received_ns):
        if event.get("sessionId") != self.sid:
            return
        # Received prompts contain identity; outgoing authentication responses
        # never pass through this recorder.
        self.journal.emit("bridge", event=event, received_ns=received_ns)
        with self.lock:
            if len(self.events) < self.EVENT_CAPACITY:
                self.events.append((received_ns, event))
            else:
                self.overflow += 1
        kind = event.get("type")
        if kind == "hello":
            capabilities = event.get("capabilities", [])
            if "performance_monitoring" not in capabilities:
                raise RuntimeError("client lacks performance_monitoring capability")
            start = dict(arguments=self.lab["rdp_arguments"],
                         displaySelections=self.lab.get("display_selections", []),
                         performanceSupported=True, thumbnailsEnabled=False,
                         confirmSessionClose=False)
            if self.tls_secrets_file:
                if "tls_keylog" not in capabilities:
                    raise RuntimeError("client lacks requested tls_keylog capability")
                start["tlsSecretsFile"] = str(self.tls_secrets_file)
            self.send("start", **start)
            self.send("performance_control", performanceEnabled=True,
                      performanceOverlayVisible=False, performanceGeneration=1)
            self.ready.set()
        elif kind == "auth_request":
            password = os.environ.get(self.lab["password_env"])
            if event.get("rejected") or not password:
                raise RuntimeError("missing or rejected test credentials")
            self.send("auth_response", requestId=event["requestId"], accepted=True,
                      username=self.lab["username"], domain=self.lab.get("domain", ""),
                      password=password)
        elif kind == "certificate_request":
            expected = fingerprint(self.lab.get("certificate_fingerprint", self.lab.get("certificate_sha256", "")))
            accepted = expected is not None and expected == fingerprint(event.get("fingerprint", ""))
            self.send("certificate_response", requestId=event["requestId"],
                      decision="accept_once" if accepted else "reject")
            if not accepted:
                raise RuntimeError("certificate pin missing or mismatched")
            self.pin_seen = True
            self.pin_fingerprint = expected
        elif kind == "state":
            if event.get("phase") == "connected":
                self.connected.set()
            else:
                self.connected.clear()
        elif kind == "ended":
            self.ended.set()

    def read(self):
        pending = b""
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    if len(line) > 1024 * 1024:
                        raise RuntimeError("oversized bridge frame")
                    event = json.loads(line)
                    if not isinstance(event, dict):
                        raise RuntimeError("bridge event is not an object")
                    self.handle(event, time.monotonic_ns())
                if len(pending) > 1024 * 1024:
                    raise RuntimeError("oversized bridge buffer")
            if pending:
                raise RuntimeError("truncated bridge frame at EOF")
        except BaseException as error:
            self.failed = type(error).__name__ + ": " + str(error)
            self.journal.emit("bridge_reader_error", error=self.failed)
        finally:
            self.connected.clear()
            self.ended.set()

    def wait_connected(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.connected.wait(.1):
                return
            if self.failed or self.proc.poll() is not None or self.ended.is_set():
                raise RuntimeError(self.failed or "client exited before connecting")
        raise TimeoutError("RDP initial connection timed out")

    def snapshot(self):
        with self.lock:
            return list(self.events)

    def require_observed_pin(self):
        if not self.pin_seen:
            raise RuntimeError("expected certificate pin was not observed by a callback in the fresh private certificate store")

    def check_live(self):
        if self.failed or self.proc.poll() is not None or self.ended.is_set():
            raise RuntimeError(self.failed or "client exited during observation")
        if self.overflow:
            raise RuntimeError("bridge telemetry capacity exceeded")

    def close(self):
        try:
            self.send("cancel")
        except OSError:
            pass
        try:
            self.proc.wait(8)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        self.thread.join(5)
        self.err.close()
        self.journal.close()
