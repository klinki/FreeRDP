"""Minimal Microsoft SUT control protocol adapter for a local SDL client.

Wire format: WindowsProtocolTestSuites/TestSuites/RDP/RDPSUTControlAgent/Docs/.
Only start/stop are implemented. Unsupported commands fail explicitly.
"""

from dataclasses import dataclass
import json
import os
from pathlib import Path
import signal
import socket
import socketserver
import struct
import subprocess
import threading
import uuid

from profiles import CaseOptions

MAX_FIELD = 65536


def exact(stream, size):
    if not 0 <= size <= MAX_FIELD:
        raise ValueError("SUT control field exceeds limit")
    data = bytearray()
    while len(data) < size:
        part = stream.recv(size - len(data))
        if not part:
            raise EOFError("Truncated SUT control message")
        data.extend(part)
    return bytes(data)


def field(stream):
    return exact(stream, struct.unpack("<I", exact(stream, 4))[0])


@dataclass
class Request:
    command: int
    case: bytes
    request_id: int
    payload: bytes

    @classmethod
    def read(cls, stream):
        kind, suite, command = struct.unpack("<HHH", exact(stream, 6))
        if (kind, suite) != (0, 1):
            raise ValueError("Expected an RDP SUT control request")
        case = field(stream)
        request_id = struct.unpack("<H", exact(stream, 2))[0]
        field(stream)  # Help text is informational, never executable.
        return cls(command, case, request_id, field(stream))

    def response(self, error=""):
        message = error.encode("utf-8")[:4096]
        return (struct.pack("<HHHI", 1, 1, self.command, len(self.case)) + self.case
                + struct.pack("<HII", self.request_id, bool(error), len(message))
                + message + struct.pack("<I", 0))


def connection(payload, expected_port, allow_fullscreen=False):
    if len(payload) < 16:
        raise ValueError("Truncated connection parameters")
    kind, port, screen, width, height, approach, length = struct.unpack("<I6H", payload[:16])
    if kind != 1 or approach != 0 or screen not in (0, 1):
        raise ValueError("Only negotiated parameter connections are supported")
    if screen == 1 and not allow_fullscreen:
        raise ValueError("This test does not permit a fullscreen connection")
    if len(payload) != 16 + length:
        raise ValueError("Invalid connection address length")
    address = payload[16:].decode("utf-8")
    if address != "127.0.0.1" or port != expected_port:
        raise ValueError("The smoke adapter only connects to its configured loopback endpoint")
    if not (200 <= width <= 2048 and 200 <= height <= 2048):
        raise ValueError("Unsupported smoke-test desktop size")
    args = [f"/v:{address}:{port}", f"/size:{width}x{height}"]
    if screen == 1:
        args.append("/f")
    return args


def stop_process(process):
    if process is not None and process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        except ProcessLookupError:
            process.wait(timeout=5)


class Client:
    def __init__(self, binary, port, directory, fingerprint, mode,
                 options=CaseOptions(), graphics_mask=None):
        self.binary = str(binary)
        self.port = port
        self.directory = directory
        self.fingerprint = fingerprint
        self.mode = mode
        self.options = options
        self.graphics_mask = graphics_mask
        if options.graphics and graphics_mask != 0x200:
            raise ValueError("Graphics cases require the verified compatibility mask")
        self.process = self.log = self.bridge = self.worker = None
        self.errors = []
        self.starts = 0
        self.events = []

    def start(self, payload):
        args = connection(payload, self.port, allow_fullscreen=self.options.graphics)
        self.stop()
        args += ["/u:wpts", "/sec:tls", "-auto-reconnect", "-clipboard", "/audio-mode:2"]
        if self.options.redirect_drive:
            share = self.directory / "shared-folder"
            if "," in str(share):
                raise ValueError("Redirected-drive tests require an output path without commas")
            share.mkdir(mode=0o700, exist_ok=True)
            args.append(f"/drive:wpts,{share}")
        if self.options.video:
            args.append("/video")
        if self.options.graphics:
            args.append(f"/gfx:AVC444,mask:{self.graphics_mask:#x}")
        environment = dict(os.environ, WLOG_LEVEL="INFO")
        # Keep FreeRDP's configuration/certificate files out of the user's profile.
        environment["XDG_CONFIG_HOME"] = str(self.directory / "config")
        environment["XDG_CACHE_HOME"] = str(self.directory / "cache")
        contents = Path(self.binary).parent.parent
        if (contents / "Frameworks/ossl-modules").is_dir():
            environment["OPENSSL_MODULES"] = str(contents / "Frameworks/ossl-modules")
            environment["OPENSSL_CONF"] = str(contents / "Resources/openssl.cnf")
        self.starts += 1
        self.log = (self.directory / f"client-{self.starts}.log").open("wb")
        kwargs = dict(stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                      env=environment, start_new_session=True)
        if self.mode == "standard":
            args += ["/p:wpts", f"/cert:fingerprint:sha256:{self.fingerprint}"]
            self.process = subprocess.Popen([self.binary, *args], **kwargs)
        else:
            self.bridge, child = socket.socketpair()
            self.session = str(uuid.uuid4())
            try:
                self.process = subprocess.Popen([
                    self.binary, f"/launcher-fd:{child.fileno()}",
                    f"/launcher-session:{self.session}",
                ], pass_fds=(child.fileno(),), **kwargs)
            finally:
                child.close()
            self.worker = threading.Thread(target=self.run_bridge, args=(args,), daemon=True)
            self.worker.start()

    def send(self, kind, **values):
        message = dict(v=1, type=kind, sessionId=self.session, **values)
        self.bridge.sendall((json.dumps(message) + "\n").encode())

    def run_bridge(self, args):
        try:
            buffer = b""
            hello = False
            while True:
                part = self.bridge.recv(65536)
                if not part:
                    return
                buffer += part
                if len(buffer) > 1048576:
                    raise ValueError("Oversized launcher bridge message")
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    message = json.loads(line)
                    if message.get("v") != 1 or message.get("sessionId", "").lower() != self.session:
                        raise ValueError("Invalid bridge session or protocol version")
                    kind = message["type"]
                    self.events.append(message)
                    if not hello:
                        if kind != "hello":
                            raise ValueError("Expected launcher hello")
                        hello = True
                        self.send("start", arguments=args, displaySelections=[])
                    elif kind == "certificate_request":
                        # Accept only the certificate generated for this local test run.
                        received = message.get("fingerprint", "").replace(":", "").lower()
                        accepted = received == self.fingerprint.replace(":", "").lower()
                        self.send("certificate_response", requestId=message["requestId"],
                                  decision="accept_once" if accepted else "reject")
                        if not accepted:
                            raise ValueError("Unexpected test-server certificate")
                    elif kind == "auth_request":
                        self.send("auth_response", requestId=message["requestId"], accepted=True,
                                  username="wpts", domain="", password="wpts")
        except (OSError, ValueError, KeyError) as error:
            self.errors.append(str(error))

    def stop(self):
        stop_process(self.process)
        if self.worker is not None:
            self.worker.join(timeout=5)
        if self.bridge is not None:
            self.bridge.close()
        if self.log is not None:
            self.log.close()
        self.process = self.log = self.bridge = self.worker = None


class ControlServer(socketserver.TCPServer):
    allow_reuse_address = True

    def __init__(self, client):
        self.client = client
        self.failures = []
        super().__init__(("127.0.0.1", 0), ControlHandler)


class ControlHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        try:
            request = Request.read(self.request)
        except (OSError, ValueError, EOFError) as error:
            self.server.failures.append(str(error))
            return
        error = ""
        try:
            if request.command == 1:
                self.server.client.start(request.payload)
            elif request.command == 2 and not request.payload:
                self.server.client.stop()
            else:
                raise ValueError(f"Unsupported SUT command: {request.command}")
        except (OSError, ValueError, subprocess.SubprocessError) as failure:
            error = str(failure)
            self.server.failures.append(error)
        self.request.sendall(request.response(error))
