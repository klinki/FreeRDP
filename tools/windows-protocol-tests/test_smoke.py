"""Harness regressions; these do not substitute for Microsoft's live protocol tests."""

import io
import contextlib
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
import xml.etree.ElementTree as ET

from control import Client, Request, connection, ControlServer, MAX_FIELD, stop_process
from run import configure, main, result_from_trx, set_property, NS
from profiles import (PIN, CaseOptions, graphics_compatibility_mask,
                      options_for, profile_cases, select_cases)


def parameters(address="127.0.0.1", port=3391, screen=0):
    address = address.encode()
    return struct.pack("<I6H", 1, port, screen, 1024, 768, 0, len(address)) + address


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

    def test_fullscreen_is_only_allowed_for_cases_that_request_it(self):
        with self.assertRaises(ValueError):
            connection(parameters(screen=1), 3391)
        self.assertIn("/f", connection(parameters(screen=1), 3391, allow_fullscreen=True))
        self.assertNotIn("/f", connection(parameters(), 3391, allow_fullscreen=True))
        with self.assertRaises(ValueError):
            connection(parameters(screen=2), 3391, allow_fullscreen=True)


class ProfileTests(unittest.TestCase):
    def test_quick_is_a_baseline_subset_of_extended(self):
        quick, extended = profile_cases("quick"), profile_cases("extended")
        self.assertEqual(len(quick), 6)
        self.assertEqual(len(extended), 92)
        self.assertEqual(extended[:len(quick)], quick)
        self.assertTrue(all(options_for(case) == CaseOptions() for case in quick))
        self.assertEqual(sum(options_for(case) != CaseOptions() for case in extended), 6)
        self.assertTrue(set(PIN["case_options"]) <= set(extended))
        self.assertNotIn("BVT_StaticVirtualChannel_PositiveTest_CompressionNotSupported", extended)
        self.assertNotIn("S1_Connection_Initialization_InitialUDPConnection", extended)

    def test_selection_is_restricted_to_the_profile_without_duplicates(self):
        extra = next(case for case in profile_cases("extended") if case not in profile_cases("quick"))
        self.assertEqual(select_cases("extended", [extra]), [extra])
        for requested in ([extra], ["Nonexistent_Test"], [], [profile_cases("quick")[0]] * 2):
            with self.subTest(requested=requested), self.assertRaises(ValueError):
                select_cases("quick", requested)

    def test_manifest_include_cycles_and_duplicate_cases_are_rejected(self):
        manifest = {"profiles": {
            "quick": {"cases": ["TestOne"]},
            "extended": {"include": ["quick"], "cases": ["TestOne"]},
        }}
        with self.assertRaises(ValueError):
            profile_cases("extended", manifest)
        manifest["profiles"]["quick"]["include"] = ["extended"]
        with self.assertRaises(ValueError):
            profile_cases("quick", manifest)

    def test_list_needs_no_client_and_defaults_to_quick(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(main(["--list"]), 0)
        self.assertEqual(len(output.getvalue().splitlines()), 7)
        self.assertTrue(output.getvalue().startswith("Profile quick: 6 cases"))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(main(["--profile", "extended", "--list"]), 0)
        self.assertEqual(len(output.getvalue().splitlines()), 93)

    def test_graphics_filter_requires_the_audited_build_layout(self):
        self.assertEqual(graphics_compatibility_mask("WITH_GFX_AV1=OFF WITH_GFX_AZURE=OFF"), 0x200)
        for config in ("", "WITH_GFX_AV1=OFF", "WITH_GFX_AV1=ON WITH_GFX_AZURE=OFF",
                       "WITH_GFX_AV1=OFF WITH_GFX_AZURE=ON"):
            with self.subTest(config=config), self.assertRaises(ValueError):
                graphics_compatibility_mask(config)

    def test_client_case_options_only_add_the_requested_features(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process = Mock()
            process.poll.return_value = 0
            for options in (CaseOptions(), CaseOptions(redirect_drive=True),
                            CaseOptions(video=True), CaseOptions(graphics=True)):
                client = Client(root / "sdl-freerdp", 3391, root, "test-fingerprint", "standard",
                                options, 0x200 if options.graphics else None)
                with patch("control.subprocess.Popen", return_value=process) as launch:
                    try:
                        client.start(parameters(screen=int(options.graphics)))
                        args = launch.call_args.args[0]
                        self.assertEqual("/video" in args, options.video)
                        self.assertEqual("/gfx:AVC444,mask:0x200" in args, options.graphics)
                        self.assertEqual("/f" in args, options.graphics)
                        drives = [arg for arg in args if arg.startswith("/drive:")]
                        self.assertEqual(bool(drives), options.redirect_drive)
                        if drives:
                            self.assertEqual(drives, [f"/drive:wpts,{root / 'shared-folder'}"])
                            self.assertEqual(list((root / "shared-folder").iterdir()), [])
                    finally:
                        client.stop()

    def test_case_configuration_resets_after_special_options(self):
        def config_file(path, properties, adapters=False):
            root = ET.Element(f"{{{NS}}}TestSite")
            props = ET.SubElement(root, f"{{{NS}}}Properties")
            for names in properties:
                parent = props
                for name in names[:-1]:
                    group = parent.find(f"{{{NS}}}Group[@name='{name}']")
                    if group is None:
                        group = ET.SubElement(parent, f"{{{NS}}}Group", name=name)
                    parent = group
                ET.SubElement(parent, f"{{{NS}}}Property", name=names[-1], value="initial")
            if adapters:
                ET.SubElement(ET.SubElement(root, f"{{{NS}}}Adapters"),
                              f"{{{NS}}}Adapter", name="IRdpSutControlAdapter")
            ET.ElementTree(root).write(path, encoding="utf-8")

        with tempfile.TemporaryDirectory() as directory:
            suite = Path(directory)
            (suite / "Bin").mkdir()
            config_file(suite / "Bin/RDP_ClientTestSuite.ptfconfig", [
                ("SUTControl", name) for name in
                ("AgentAddress", "TransportType", "ClientSupportRDPFile", "AlwaysNeedResponse")
            ], adapters=True)
            path = suite / "Bin/RDP_ClientTestSuite.deployment.ptfconfig"
            config_file(path, [(name,) for name in (
                "ServerPort", "ProxyIP", "IpVersion", "ServerDomain", "ServerUserName",
                "ServerUserPassword", "CertificatePath", "CertificatePassword", "IsWindowsImplementation",
                "DropConnectionForInvalidRequest", "SupportRDPEFS", "Version", "VerifyRdpbcgrMessage", "WaitTime",
            )] + [("Security", "Negotiation"), ("Security", "Protocol"), ("VerifySUTDisplay", "Enable")])
            configure(suite, suite, 3391, 4488, suite / "test.pfx",
                      CaseOptions(rdp_version="10.8", redirect_drive=True))
            special = ET.parse(path)
            self.assertEqual(special.find(f".//{{{NS}}}Property[@name='Version']").get("value"), "10.8")
            configure(suite, suite, 3392, 4489, suite / "test.pfx")
            reset = ET.parse(path)
            for key, expected in (("Version", "10.6"), ("SupportRDPEFS", "false"), ("ServerPort", "3392")):
                self.assertEqual(reset.find(f".//{{{NS}}}Property[@name='{key}']").get("value"), expected)


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
