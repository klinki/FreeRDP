#!/usr/bin/env python3
"""Exercise a real launcher-enabled client without a remote host or credentials."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid


def runtime_environment(binary, environment=None):
    environment = dict(os.environ if environment is None else environment)
    executable = Path(binary).resolve()
    if executable.parent.name == "Helpers":
        contents = executable.parent.parent
        modules = contents / "Frameworks/ossl-modules"
        if (modules / "legacy.dylib").is_file():
            environment["OPENSSL_MODULES"] = str(modules)
            environment["OPENSSL_CONF"] = str(contents / "Resources/openssl.cnf")
    return environment


class Client:
    def __init__(self, binary, environment=None, dock_single=False, log_path=None, trace=None):
        self.session = str(uuid.uuid4()).upper()
        self.socket, child = socket.socketpair()
        self.socket.settimeout(12)
        self.buffer = b""
        self.log = Path(log_path).open("w+b") if log_path else tempfile.TemporaryFile()
        self.trace = trace or (lambda *args, **kwargs: None)
        arguments = [binary, f"/launcher-fd:{child.fileno()}", f"/launcher-session:{self.session}"]
        if dock_single:
            arguments.append("/launcher-dock:single")
        self.process = subprocess.Popen(
            arguments,
            pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL,
            stdout=self.log, stderr=self.log, env=runtime_environment(binary, environment),
        )
        child.close()
        self.trace("client_started", pid=self.process.pid, session_id=self.session)

    def send(self, kind, **fields):
        self.trace("bridge_send", message=dict(type=kind, **fields))
        self.socket.sendall((json.dumps(dict(v=1, type=kind, sessionId=self.session, **fields)) + "\n").encode())

    def receive(self, timeout=None):
        previous_timeout = self.socket.gettimeout()
        deadline = time.monotonic() + timeout if timeout is not None else None
        try:
            while b"\n" not in self.buffer:
                if deadline is not None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("Bridge message deadline exceeded")
                    self.socket.settimeout(remaining)
                chunk = self.socket.recv(65536)
                if not chunk:
                    self.log.seek(0)
                    raise AssertionError("Unexpected EOF: " + self.log.read().decode(errors="replace")[-3000:])
                self.buffer += chunk
                assert len(self.buffer) <= 1048576, "Oversized message"
        finally:
            self.socket.settimeout(previous_timeout)
        frame, self.buffer = self.buffer.split(b"\n", 1)
        message = json.loads(frame)
        self.trace("bridge_receive", message=message)
        assert message["v"] == 1, message
        assert message["sessionId"].upper() == self.session, message
        return message

    def hello(self):
        message = self.receive()
        assert message["type"] == "hello", message
        assert {"auth", "certificate", "display_uuid", "focus", "retry"} <= set(message["capabilities"]), message
        assert message["displays"], "Expected a display on this Mac"
        for display in message["displays"]:
            assert display["uuid"] and display["width"] > 0 and display["height"] > 0, display
        return message

    def ended(self, timeout=15):
        events = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = self.receive(timeout=max(0.001, deadline - time.monotonic()))
            except TimeoutError as error:
                raise AssertionError(f"No terminal event within {timeout}s") from error
            events.append(message)
            if message["type"] == "ended":
                return message, events
        raise AssertionError(f"No terminal event within {timeout}s")

    def wait(self):
        return self.process.wait(timeout=10)

    def close(self):
        self.socket.close()
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.log.close()


def exercise(binary, scenario):
    client = Client(binary)
    try:
        hello = client.hello()
        if scenario == "discover_cancel":
            client.send("cancel")
            terminal, _ = client.ended()
            assert terminal["outcome"] == "cancelled", terminal
            client.wait()
        elif scenario == "parent_eof":
            client.socket.close()
            client.wait()
        elif scenario == "unreachable":
            # Bound but non-listening loopback socket: reserved, unreachable port.
            with socket.socket() as unused:
                unused.bind(("127.0.0.1", 0))
                port = unused.getsockname()[1]
                client.send("start", arguments=[f"/v:127.0.0.1:{port}", "/u:smoke", "/timeout:1000", "-auto-reconnect"], displaySelections=[])
                terminal, events = client.ended()
                assert terminal["outcome"] in ("initial_failure", "client_error"), terminal
                assert not terminal.get("hadConnected", False), terminal
                assert not any(event["type"] == "auth_request" for event in events), events
                client.wait()
        elif scenario == "missing_monitor":
            client.send("start", arguments=["/v:127.0.0.1:1"], displaySelections=[{"uuid":str(uuid.uuid4())}])
            terminal, _ = client.ended()
            assert terminal["outcome"] in ("initial_failure", "client_error"), terminal
            assert not terminal.get("hadConnected", False), terminal
            client.wait()
        print(f"PASS {scenario}: {hello['engineVersion']}, {len(hello['displays'])} displays")
    finally:
        client.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    options = parser.parse_args()
    for case in ("discover_cancel", "parent_eof", "unreachable", "missing_monitor"):
        exercise(os.path.abspath(options.binary), case)
