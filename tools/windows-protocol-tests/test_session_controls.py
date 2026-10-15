"""Exercise the loopback fault fixture without launching FreeRDP or a desktop."""

from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest
from unittest.mock import Mock

from control import Client, ControlServer, ReconnectRelay
from profiles import CaseOptions


class EchoServer:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.closed = threading.Event()
        self.peers = []
        self.worker = threading.Thread(target=self.run, daemon=True)
        self.worker.start()

    def run(self):
        while not self.closed.is_set():
            try:
                stream, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.peers.append(stream)
            stream.settimeout(1)
            try:
                while not self.closed.is_set():
                    data = stream.recv(65536)
                    if not data:
                        break
                    stream.sendall(data)
            except OSError:
                pass
            finally:
                stream.close()

    def close(self):
        self.closed.set()
        self.listener.close()
        for peer in self.peers:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.worker.join(timeout=2)


class SessionControlTests(unittest.TestCase):
    def test_interruption_preserves_listener_and_forwards_reconnected_bytes(self):
        server = EchoServer()
        relay = ReconnectRelay(server.port)
        self.addCleanup(server.close)
        self.addCleanup(relay.close)
        with socket.create_connection(("127.0.0.1", relay.port), timeout=2) as first:
            first.sendall(b"original TLS stream")
            self.assertEqual(first.recv(100), b"original TLS stream")
            relay.interrupt()
            self.assertEqual(first.recv(100), b"")
        with socket.create_connection(("127.0.0.1", relay.port), timeout=2) as second:
            second.sendall(b"new TLS stream and saved reconnect cookie")
            self.assertEqual(second.recv(100), b"new TLS stream and saved reconnect cookie")
            self.assertEqual(relay.connections, 2)
            self.assertEqual(relay.interruptions, 1)
            with self.assertRaisesRegex(ValueError, "Only one"):
                relay.interrupt()
        self.assertEqual(relay.errors, [])

    def test_no_active_connection_cannot_be_reported_as_interrupted(self):
        relay = ReconnectRelay(1)
        self.addCleanup(relay.close)
        with self.assertRaisesRegex(ValueError, "No active"):
            relay.interrupt()
        self.assertEqual(relay.interruptions, 0)

    def test_close_wakes_idle_and_connected_workers(self):
        server = EchoServer()
        self.addCleanup(server.close)
        for connected in (False, True):
            relay = ReconnectRelay(server.port)
            peer = None
            try:
                if connected:
                    peer = socket.create_connection(("127.0.0.1", relay.port), timeout=2)
                    peer.sendall(b"ready")
                    self.assertEqual(peer.recv(5), b"ready")
                relay.close()
                self.assertFalse(relay.worker.is_alive())
                if peer:
                    self.assertEqual(peer.recv(1), b"")
            finally:
                if peer:
                    peer.close()
                relay.close()

    def test_reconnect_requires_opt_in_and_running_client(self):
        with tempfile.TemporaryDirectory() as directory:
            client = Client("/unused", 3389, Path(directory), "fingerprint", "standard")
            with self.assertRaisesRegex(ValueError, "does not permit"):
                client.reconnect()
            client.options = CaseOptions(auto_reconnect=True)
            client.relay = Mock()
            with self.assertRaisesRegex(ValueError, "not running"):
                client.reconnect()
            client.relay.interrupt.assert_not_called()

    def test_graceful_close_uses_owned_signal_or_authenticated_bridge_once(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ("standard", "bridge"):
                client = Client("/unused", 3389, Path(directory), "fingerprint", mode,
                                CaseOptions(graceful_close=True))
                process = client.process = Mock()
                process.poll.return_value = None
                client.send = Mock()
                client.close_connection()
                client.close_connection()
                self.assertEqual(client.graceful_closes, 1)
                process.wait.assert_not_called()
                process.kill.assert_not_called()
                if mode == "standard":
                    import signal
                    process.send_signal.assert_called_once_with(signal.SIGTERM)
                    client.send.assert_not_called()
                else:
                    client.send.assert_called_once_with("cancel")
                    process.send_signal.assert_not_called()

    def test_graceful_close_cannot_count_prelaunch_cleanup_as_test_action(self):
        with tempfile.TemporaryDirectory() as directory:
            client = Client("/unused", 3389, Path(directory), "fingerprint", "standard",
                            CaseOptions(graceful_close=True))
            client.close_connection()
            self.assertEqual(client.graceful_closes, 0)
            client.options = CaseOptions()
            with self.assertRaisesRegex(ValueError, "does not permit"):
                client.close_connection()

    def test_reconnect_command_invokes_client_without_relaunching(self):
        client = Mock()
        server = ControlServer(client)
        thread = threading.Thread(target=server.handle_request, daemon=True)
        thread.start()
        try:
            with socket.create_connection(server.server_address, timeout=2) as stream:
                case = b"reconnect"
                message = (struct.pack("<HHHI", 0, 1, 3, len(case)) + case
                           + struct.pack("<HII", 1, 0, 0))
                stream.sendall(message)
                response = stream.recv(1024)
                status_offset = 10 + len(case) + 2
                self.assertEqual(struct.unpack_from("<I", response, status_offset)[0], 0)
            thread.join(timeout=2)
            client.reconnect.assert_called_once_with()
            client.start.assert_not_called()
            client.stop.assert_not_called()
            self.assertEqual(server.failures, [])
        finally:
            server.server_close()

    def test_reconnect_command_rejects_payload_and_records_failure(self):
        client = Mock()
        server = ControlServer(client)
        thread = threading.Thread(target=server.handle_request, daemon=True)
        thread.start()
        try:
            with socket.create_connection(server.server_address, timeout=2) as stream:
                case = b"reconnect"
                message = (struct.pack("<HHHI", 0, 1, 3, len(case)) + case
                           + struct.pack("<HII", 1, 0, 1) + b"x")
                stream.sendall(message)
                self.assertIn(b"Unsupported SUT command", stream.recv(1024))
            thread.join(timeout=2)
            client.reconnect.assert_not_called()
            self.assertEqual(server.failures, ["Unsupported SUT command: 3"])
        finally:
            server.server_close()


if __name__ == "__main__":
    unittest.main()
