"""Minimal Microsoft SUT control protocol adapter for a local SDL client.

Wire format: WindowsProtocolTestSuites/TestSuites/RDP/RDPSUTControlAgent/Docs/.
Start/stop and opt-in network interruption are implemented.
Unsupported commands fail explicitly.
"""

from dataclasses import dataclass
import json
import os
from pathlib import Path
import signal
import select
import socket
import socketserver
import struct
import subprocess
import threading
import traceback
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


def signal_process_group(process, number):
    try:
        os.killpg(process.pid, number)
    except PermissionError as failure:
        # macOS can return EPERM while an owned child is exiting, before it is
        # reapable, as well as for its zombie group. Use the existing cleanup
        # phase deadline to confirm exit; an immediate poll misses that window.
        # Never suppress a denied signal unless waitpid proves the child exited.
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            raise failure from None
    except ProcessLookupError:
        pass  # The bounded wait below must still reap the owned child.


def stop_process(process):
    if process is not None and process.poll() is None:
        signal_process_group(process, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            signal_process_group(process, signal.SIGKILL)
            process.wait(timeout=5)


class ReconnectRelay:
    """Forward opaque TLS bytes to one configured loopback endpoint.

    The SUT reconnect command drops only the current accepted pair. The listener
    remains available so the same process can reconnect with its saved cookie.
    This does not modify host routes, interfaces, or TLS authentication.
    """

    def __init__(self, destination_port):
        self.destination_port = destination_port
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.lock = threading.Lock()
        self.closed = threading.Event()
        self.pair = None
        self.connections = 0
        self.interruptions = 0
        self.errors = []
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    @staticmethod
    def _close_pair(pair):
        if pair:
            for stream in pair:
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()

    def _run(self):
        while not self.closed.is_set():
            try:
                downstream, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            upstream = None
            pair = None
            try:
                upstream = socket.create_connection(("127.0.0.1", self.destination_port), 5)
                upstream.settimeout(5)
                downstream.settimeout(5)
                pair = (downstream, upstream)
                with self.lock:
                    if self.closed.is_set():
                        return
                    self.pair = pair
                    self.connections += 1
                while not self.closed.is_set():
                    readable, _, _ = select.select(pair, [], [], 0.2)
                    if not readable:
                        continue
                    for stream in readable:
                        data = stream.recv(65536)
                        if not data:
                            break
                        (upstream if stream is downstream else downstream).sendall(data)
                    else:
                        continue
                    break
            except (OSError, ValueError) as error:
                # Closing the owned sockets is expected during interruption and
                # shutdown. Unexpected relay failures must fail the test.
                with self.lock:
                    if not self.closed.is_set() and (pair is None or self.pair is pair):
                        self.errors.append(str(error))
            finally:
                with self.lock:
                    if self.pair is pair:
                        self.pair = None
                self._close_pair((downstream, upstream) if upstream else (downstream,))

    def interrupt(self):
        with self.lock:
            if self.closed.is_set() or self.pair is None:
                raise ValueError("No active test connection to interrupt")
            if self.interruptions:
                raise ValueError("Only one network interruption is allowed per test")
            pair, self.pair = self.pair, None
            self.interruptions += 1
            self._close_pair(pair)

    def close(self):
        self.closed.set()
        self.listener.close()
        with self.lock:
            pair, self.pair = self.pair, None
            self._close_pair(pair)
        self.worker.join(timeout=6)
        if self.worker.is_alive():
            raise RuntimeError("Reconnect relay did not stop")


class Client:
    def __init__(self, binary, port, directory, fingerprint, mode,
                 options=CaseOptions(), graphics_mask=None, *, listener=None):
        self.listener = listener
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
        self.relay = None
        self.reconnects = 0
        self.relay_connections = 0
        self.graceful_closes = 0

    def start(self, payload):
        if self.listener is not None:
            self.port = self.listener.read()["port"]
        args = connection(payload, self.port, allow_fullscreen=self.options.graphics)
        self.stop()
        args += ["/u:wpts", "/sec:tls", "-clipboard", "/audio-mode:2"]
        if self.options.auto_reconnect:
            self.relay = ReconnectRelay(self.port)
            args[0] = f"/v:127.0.0.1:{self.relay.port}"
            args += ["+auto-reconnect", "/auto-reconnect-max-retries:3"]
        else:
            args.append("-auto-reconnect")
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

    def close_connection(self):
        if not self.options.graceful_close:
            raise ValueError("This test does not permit protocol close")
        # The same upstream command is used for cleanup before/after a test.
        # Count only the actual request to a running client; run.py checks it.
        if self.process is None or self.process.poll() is not None or self.graceful_closes:
            return
        if self.mode == "bridge":
            self.send("cancel")
        else:
            # SDL registers sdl_term_handler: abort the session and enqueue quit.
            # Success requires the server to observe the user-requested MCS PDU
            # and transport close. A raw process exit cannot satisfy that test.
            self.process.send_signal(signal.SIGTERM)
        self.graceful_closes += 1

    def reconnect(self):
        if not self.options.auto_reconnect or self.relay is None:
            raise ValueError("This test does not permit network interruption")
        if self.process is None or self.process.poll() is not None:
            raise ValueError("Cannot reconnect a client that is not running")
        self.relay.interrupt()
        self.reconnects += 1

    def stop(self):
        stop_process(self.process)
        if self.relay is not None:
            self.relay.close()
            self.relay_connections += self.relay.connections
            self.errors.extend(self.relay.errors)
            self.relay = None
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
        self.failure_details = []
        super().__init__(("127.0.0.1", 0), ControlHandler)

    def record_failure(self, error, *, command=None, stage):
        self.failures.append(str(error))
        self.failure_details.append(dict(
            stage=stage, command=command, exception=type(error).__name__,
            message=str(error), traceback=traceback.format_exc(),
        ))


class ControlHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        try:
            request = Request.read(self.request)
        except (OSError, ValueError, EOFError) as error:
            self.server.record_failure(error, stage="read-request")
            return
        error = ""
        try:
            if request.command == 1:
                self.server.client.start(request.payload)
            elif request.command == 2 and not request.payload:
                if self.server.client.options.graceful_close:
                    self.server.client.close_connection()
                else:
                    self.server.client.stop()
            elif request.command == 3 and not request.payload:
                self.server.client.reconnect()
            else:
                raise ValueError(f"Unsupported SUT command: {request.command}")
        except (OSError, ValueError, subprocess.SubprocessError) as failure:
            error = str(failure)
            self.server.record_failure(failure, command=request.command, stage="execute-command")
        try:
            self.request.sendall(request.response(error))
        except OSError as failure:
            self.server.record_failure(failure, command=request.command, stage="send-response")
