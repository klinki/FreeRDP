"""Harness regressions; these do not substitute for Microsoft's live protocol tests."""

import io
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET

from control import Request, connection, ControlServer, MAX_FIELD, stop_process
from run import result_from_trx, set_property, NS


def parameters(address="127.0.0.1", port=3391):
    address = address.encode()
    return struct.pack("<I6H", 1, port, 0, 1024, 768, 0, len(address)) + address


def request_bytes(command=1, help_text=b"", payload=None):
    payload = parameters() if payload is None else payload
    case = "case-\u00e9".encode()
    return (struct.pack("<HHHI", 0, 1, command, len(case)) + case
            + struct.pack("<HI", 42, len(help_text)) + help_text
            + struct.pack("<I", len(payload)) + payload)


class FragmentedSocket:
    def __init__(self, data):
        self.stream = io.BytesIO(data)

    def recv(self, size):
        return self.stream.read(min(1, size))


class ProtocolTests(unittest.TestCase):
    def test_fragmented_request_with_empty_and_nonempty_help(self):
        for help_text in (b"", b"connect the client"):
            with self.subTest(help=help_text):
                request = Request.read(FragmentedSocket(request_bytes(help_text=help_text)))
                self.assertEqual((request.command, request.request_id), (1, 42))
                self.assertEqual(request.case.decode(), "case-\u00e9")
                self.assertEqual(connection(request.payload, 3391), ["/v:127.0.0.1:3391", "/size:1024x768"])
                # Check independently against the documented response format.
                response = request.response("unsupported")
                self.assertEqual(struct.unpack("<HHHI", response[:10]), (1, 1, 1, 7))
                self.assertEqual(struct.unpack("<HII", response[17:27]), (42, 1, 11))
                self.assertEqual(response[27:], b"unsupported\x00\x00\x00\x00")

    def test_truncated_and_oversized_requests_fail(self):
        wire = request_bytes()
        for length in (0, 5, 9, 17, len(wire) - 1):
            with self.subTest(length=length), self.assertRaises(EOFError):
                Request.read(FragmentedSocket(wire[:length]))
        wire = struct.pack("<HHHI", 0, 1, 1, MAX_FIELD + 1)
        with self.assertRaises(ValueError):
            Request.read(FragmentedSocket(wire))

    def test_only_configured_loopback_destination_is_accepted(self):
        for payload in (parameters("example.com"), parameters(port=3389), parameters() + b"x",
                        struct.pack("<I", 0) + b"full address:s:example.com"):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                connection(payload, 3391)

    def test_unsupported_command_is_an_error_response(self):
        server = ControlServer(None)
        worker = threading.Thread(target=server.handle_request)
        worker.start()
        try:
            with socket.create_connection(server.server_address, timeout=3) as peer:
                peer.sendall(request_bytes(command=5, payload=b""))
                response = peer.recv(4096)
            self.assertEqual(struct.unpack("<I", response[19:23])[0], 1)
            self.assertIn(b"Unsupported SUT command: 5", response)
        finally:
            worker.join(timeout=5)
            server.server_close()
        self.assertEqual(server.failures, ["Unsupported SUT command: 5"])


class ResultsTests(unittest.TestCase):
    def check_result(self, content, expected):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.trx"
            if content is not None:
                path.write_text(content)
            return result_from_trx(path, expected)[0]

    def test_empty_skipped_failed_or_wrong_test_cannot_pass(self):
        for content in (None, '<invalid', '<TestRun xmlns="urn:test"><Results/></TestRun>',
                        '<TestRun><UnitTestResult testName="other" outcome="Passed"/></TestRun>',
                        '<TestRun><UnitTestResult testName="target" outcome="NotExecuted"/></TestRun>',
                        '<TestRun><UnitTestResult testName="target" outcome="Failed"/></TestRun>',
                        '<TestRun><UnitTestResult testName="target" outcome="Passed"/>'
                        '<UnitTestResult testName="other" outcome="Passed"/></TestRun>'):
            with self.subTest(content=content):
                self.assertFalse(self.check_result(content, "target"))

    def test_single_exact_test_result_passes(self):
        self.assertTrue(self.check_result(
            '<TestRun xmlns="urn:test"><Results><UnitTestResult testName="target" '
            'outcome="Passed"/></Results></TestRun>', "target"))

    def test_configuration_changes_are_scoped_and_fail_if_upstream_changes(self):
        root = ET.fromstring(f'<TestSite xmlns="{NS}"><Properties>'
                             '<Property name="Enable" value="true"/>'
                             '<Group name="VerifySUTDisplay"><Property name="Enable" value="true"/>'
                             '</Group></Properties></TestSite>')
        set_property(root, ("VerifySUTDisplay", "Enable"), "false")
        self.assertEqual(root.find(f"{{{NS}}}Properties/{{{NS}}}Property").get("value"), "true")
        with self.assertRaises(ValueError):
            set_property(root, ("Missing",), "false")


class CleanupTests(unittest.TestCase):
    def test_cleanup_stops_owned_process_without_affecting_another_client(self):
        command = [sys.executable, "-c", "import time; time.sleep(30)"]
        owned = subprocess.Popen(command, start_new_session=True)
        other = subprocess.Popen(command, start_new_session=True)
        try:
            stop_process(owned)
            self.assertIsNotNone(owned.poll())
            self.assertIsNone(other.poll())
        finally:
            stop_process(owned)
            stop_process(other)


if __name__ == "__main__":
    unittest.main()
