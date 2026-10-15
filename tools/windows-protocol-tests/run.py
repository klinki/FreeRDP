#!/usr/bin/env python3
"""Run the quick or extended Microsoft RDP tests against a local SDL client."""

import argparse
from datetime import datetime, timezone
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import threading
import time
import xml.etree.ElementTree as ET

from control import Client, ControlServer, stop_process
from profiles import (ROOT, PIN, CaseOptions, graphics_compatibility_mask,
                      options_for, profile_cases, select_cases, suite_patches)

NS = "http://schemas.microsoft.com/windows/ProtocolsTest/2007/07/TestConfig"
XSI = "http://www.w3.org/2001/XMLSchema-instance"
ET.register_namespace("", NS)
ET.register_namespace("xsi", XSI)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def set_property(root, path, value):
    node = root.find(f"{{{NS}}}Properties")
    for name in path[:-1]:
        node = node.find(f"{{{NS}}}Group[@name='{name}']")
        if node is None:
            raise ValueError(f"Missing Microsoft configuration group: {path}")
    prop = node.find(f"{{{NS}}}Property[@name='{path[-1]}']")
    if prop is None:
        raise ValueError(f"Missing Microsoft configuration property: {path}")
    prop.set("value", str(value))


def configure(suite, case_dir, port, agent_port, certificate, options=CaseOptions()):
    path = suite / "Bin/RDP_ClientTestSuite.ptfconfig"
    tree = ET.parse(path)
    root = tree.getroot()
    for key, value in {"AgentAddress": f"127.0.0.1:{agent_port}", "TransportType": "TCP",
                       "ClientSupportRDPFile": "false", "AlwaysNeedResponse": "true"}.items():
        set_property(root, ("SUTControl", key), value)
    adapter = root.find(f"{{{NS}}}Adapters/{{{NS}}}Adapter[@name='IRdpSutControlAdapter']")
    adapter.attrib.clear()
    adapter.attrib.update({f"{{{XSI}}}type": "managed", "name": "IRdpSutControlAdapter",
                          "adaptertype": "Microsoft.Protocols.TestSuites.Rdp.ProtocolBasedRdpSUTControlAdapter"})
    for sink in root.findall(f"{{{NS}}}TestLog/{{{NS}}}Sinks/{{{NS}}}File"):
        sink.set("directory", str(case_dir / "ptf"))
    tree.write(path, encoding="utf-8", xml_declaration=True)
    path = suite / "Bin/RDP_ClientTestSuite.deployment.ptfconfig"
    tree = ET.parse(path)
    properties = {
        ("ServerPort",): port, ("ProxyIP",): "127.0.0.1", ("IpVersion",): "Ipv4",
        ("Security", "Negotiation"): "True", ("Security", "Protocol"): "TLS",
        ("ServerDomain",): "localhost", ("ServerUserName",): "wpts", ("ServerUserPassword",): "wpts",
        ("CertificatePath",): certificate, ("CertificatePassword",): "wpts",
        ("IsWindowsImplementation",): "false", ("DropConnectionForInvalidRequest",): "true",
        ("SupportRDPEFS",): str(options.redirect_drive).lower(),
        ("Version",): options.rdp_version,
        ("VerifyRdpbcgrMessage",): "true", ("WaitTime",): 10,
        ("VerifySUTDisplay", "Enable"): "false",
    }
    for key, value in properties.items():
        set_property(tree.getroot(), key, value)
    tree.write(path, encoding="utf-8", xml_declaration=True)


def make_certificate(directory):
    key, pem, pfx = [directory / name for name in ("server.key", "server.pem", "server.pfx")]
    commands = [
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
         "-keyout", str(key), "-out", str(pem)],
        ["openssl", "pkcs12", "-export", "-inkey", str(key), "-in", str(pem),
         "-out", str(pfx), "-passout", "pass:wpts"],
    ]
    with (directory / "certificate.log").open("w") as log:
        for command in commands:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    der = subprocess.check_output(["openssl", "x509", "-in", str(pem), "-outform", "DER"], timeout=10)
    return pfx, hashlib.sha256(der).hexdigest()


def result_from_trx(path, expected):
    # dotnet can exit zero when no tests match. A single actual Passed result is required.
    if not path.is_file():
        return False, "No TRX result was produced"
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError:
        return False, "Invalid TRX result"
    results = root.findall(".//{*}UnitTestResult")
    if len(results) != 1 or results[0].get("testName") != expected:
        return False, "Expected exactly one result for the requested test"
    result = results[0]
    if result.get("outcome") != "Passed":
        return False, result.findtext(".//{*}Message") or result.get("outcome", "Unknown result")
    return True, "Passed"


def run_case(binary, suite, mode, case, destination, certificate, fingerprint, timeout,
             *, options=None, graphics_mask=None):
    options = options if options is not None else options_for(case)
    destination.mkdir()
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    client = Client(binary, port, destination, fingerprint, mode, options, graphics_mask)
    server = ControlServer(client)
    try:
        configure(suite, destination, port, server.server_address[1], certificate, options)
    except Exception:
        server.server_close()
        raise
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    process = None
    started = time.monotonic()
    error = ""
    code = None
    try:
        with (destination / "driver.log").open("w") as log:
            process = subprocess.Popen([
                "dotnet", "test", str(suite / "Bin/RDP_ClientTestSuite.dll"),
                "--filter", f"Name={case}", "--logger", "trx;LogFileName=result.trx",
                "--ResultsDirectory", str(destination),
            ], cwd=suite, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            code = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        error = f"Driver exceeded {timeout}s deadline"
    finally:
        stop_process(process)
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        client.stop()
        write_json(destination / "bridge-events.json", client.events)
    passed, detail = result_from_trx(destination / "result.trx", case)
    problems = ([error] if error else []) + server.failures + client.errors
    if code != 0:
        problems.append(f"Driver exit status: {code}")
    if client.starts != 1:
        problems.append(f"Expected one client launch, observed {client.starts}")
    if options.graceful_close and client.graceful_closes != 1:
        problems.append("Expected one graceful client-close request")
    if options.auto_reconnect:
        if client.reconnects != 1 or client.relay_connections != 2:
            problems.append("Expected one network interruption and two connections from the same client")
    if mode == "bridge":
        if options.auto_reconnect:
            phases = [event.get("phase") for event in client.events]
            if "reconnecting" not in phases:
                problems.append("Bridge did not report reconnecting")
            if case == "BVT_AutoReconnect_PositiveTest" and phases.count("connected") != 2:
                problems.append("Bridge did not report both connected sessions")
        if not any(event["type"] == "hello" for event in client.events):
            problems.append("No bridge hello received")
        if case == "BVT_ConnectionTest_Disconnection_PositiveTest_ServerInitiated":
            terminal = [event for event in client.events if event["type"] == "ended"]
            if not any(event.get("phase") == "connected" for event in client.events):
                problems.append("Bridge did not report a connected session")
            if len(terminal) != 1 or not terminal[0].get("hadConnected"):
                problems.append("Expected one terminal bridge event after a connected session")
    return dict(case=case, mode=mode, options=asdict(options), graphics_mask=graphics_mask,
                client_launches=client.starts, reconnects=client.reconnects,
                relay_connections=client.relay_connections, graceful_closes=client.graceful_closes,
                passed=passed and not problems, detail=detail,
                errors=problems, seconds=round(time.monotonic() - started, 2))


def run(args):
    cases = select_cases(args.profile, args.case)
    options = {case: options_for(case) for case in cases}
    binary, source, output = args.client.resolve(), args.suite.resolve(), args.output.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"Client must be an executable file: {binary}")
    provenance = json.loads((source / "provenance.json").read_text())
    if provenance.get("revision") != PIN["revision"] or provenance.get("repository") != PIN["repository"]:
        raise ValueError("Suite provenance does not match suite.json; run prepare.py")
    if provenance.get("patches", []) != suite_patches():
        raise ValueError("Suite driver patches do not match suite.json; rebuild with prepare.py")
    if output.exists():
        raise ValueError(f"Results directory already exists; choose a new one: {output}")
    buildconfig = None
    graphics_mask = None
    if any(option.graphics for option in options.values()):
        buildconfig = subprocess.check_output([str(binary), "/buildconfig"], text=True, timeout=10,
                                             stderr=subprocess.PIPE)
        graphics_mask = graphics_compatibility_mask(buildconfig)
    output.mkdir(parents=True, mode=0o700)
    suite = output / "suite"
    shutil.copytree(source, suite)
    certificate, fingerprint = make_certificate(output)
    probe = subprocess.run([str(binary), "/version"], text=True, timeout=10,
                           capture_output=True, check=True)
    (output / "client-probe.log").write_text(probe.stderr)
    with binary.open("rb") as stream:
        client_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    write_json(output / "provenance.json", dict(
        microsoft=provenance, client=str(binary), client_version=probe.stdout.strip(),
        client_sha256=client_hash,
        harness_revision=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        harness_dirty=bool(subprocess.check_output(["git", "status", "--porcelain", "--", str(ROOT)], cwd=ROOT)),
        started=datetime.now(timezone.utc).isoformat(), profile=args.profile, cases=cases,
        case_options={case: asdict(option) for case, option in options.items()},
        graphics_compatibility_mask=graphics_mask, client_buildconfig=buildconfig,
    ))
    results = []
    print(f"Profile {args.profile}: {len(cases)} cases; mode {args.mode}", flush=True)
    for mode in (["standard", "bridge"] if args.mode == "both" else [args.mode]):
        for index, case in enumerate(cases, 1):
            print(f"[{index}/{len(cases)}] Running {mode}: {case}", flush=True)
            result = run_case(binary, suite, mode, case, output / f"{mode}-{index}",
                              certificate, fingerprint, args.timeout, options=options[case],
                              graphics_mask=graphics_mask if options[case].graphics else None)
            result["profile"] = args.profile
            results.append(result)
            write_json(output / "summary.json", results)
            print(f"{'PASS' if result['passed'] else 'FAIL'} {mode}: {case}", flush=True)
    print(f"Results: {output}")
    return 0 if all(result["passed"] for result in results) else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, help="SDL executable (required unless --list is used)")
    parser.add_argument("--suite", type=Path, default=ROOT / ".cache/suite")
    parser.add_argument("--output", type=Path, default=ROOT / "results" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--mode", choices=("standard", "bridge", "both"), default="standard")
    parser.add_argument("--profile", choices=tuple(PIN["profiles"]), default="quick")
    parser.add_argument("--list", action="store_true", help="List selected cases without running them")
    parser.add_argument("--case", action="append", metavar="TEST_NAME",
                        help="Run a named case from the selected profile (repeatable)")
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args(argv)
    if args.timeout < 10 or args.timeout > 600:
        parser.error("--timeout must be between 10 and 600 seconds")
    try:
        cases = select_cases(args.profile, args.case)
        for case in cases:
            options_for(case)
        if args.list:
            print(f"Profile {args.profile}: {len(cases)} cases")
            for case in cases:
                overrides = PIN.get("case_options", {}).get(case, {})
                print(case + (f"  {json.dumps(overrides, sort_keys=True)}" if overrides else ""))
            return 0
        if args.client is None:
            parser.error("--client is required unless --list is used")
        os.umask(0o077)
        return run(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
