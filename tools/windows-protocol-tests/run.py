#!/usr/bin/env python3
"""Run a bounded Microsoft RDP connection smoke suite against a local SDL client."""

import argparse
from datetime import datetime, timezone
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

ROOT = Path(__file__).resolve().parent
PIN = json.loads((ROOT / "suite.json").read_text())
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


def configure(suite, case_dir, port, agent_port, certificate):
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
        ("SupportRDPEFS",): "false", ("VerifyRdpbcgrMessage",): "true", ("WaitTime",): 10,
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


def run_case(binary, suite, mode, case, destination, certificate, fingerprint, timeout):
    destination.mkdir()
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    client = Client(binary, port, destination, fingerprint, mode)
    server = ControlServer(client)
    configure(suite, destination, port, server.server_address[1], certificate)
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
    if mode == "bridge":
        if not any(event["type"] == "hello" for event in client.events):
            problems.append("No bridge hello received")
        if case == "BVT_ConnectionTest_Disconnection_PositiveTest_ServerInitiated":
            terminal = [event for event in client.events if event["type"] == "ended"]
            if not any(event.get("phase") == "connected" for event in client.events):
                problems.append("Bridge did not report a connected session")
            if len(terminal) != 1 or not terminal[0].get("hadConnected"):
                problems.append("Expected one terminal bridge event after a connected session")
    return dict(case=case, mode=mode, passed=passed and not problems, detail=detail,
                errors=problems, seconds=round(time.monotonic() - started, 2))


def run(args):
    binary, source, output = args.client.resolve(), args.suite.resolve(), args.output.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"Client must be an executable file: {binary}")
    provenance = json.loads((source / "provenance.json").read_text())
    if provenance.get("revision") != PIN["revision"] or provenance.get("repository") != PIN["repository"]:
        raise ValueError("Suite provenance does not match suite.json; run prepare.py")
    if output.exists():
        raise ValueError(f"Results directory already exists; choose a new one: {output}")
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
        started=datetime.now(timezone.utc).isoformat(), cases=args.case or PIN["cases"],
    ))
    results = []
    for mode in (["standard", "bridge"] if args.mode == "both" else [args.mode]):
        for index, case in enumerate(args.case or PIN["cases"], 1):
            print(f"Running {mode}: {case}", flush=True)
            result = run_case(binary, suite, mode, case, output / f"{mode}-{index}",
                              certificate, fingerprint, args.timeout)
            results.append(result)
            write_json(output / "summary.json", results)
            print(f"{'PASS' if result['passed'] else 'FAIL'} {mode}: {case}", flush=True)
    print(f"Results: {output}")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--suite", type=Path, default=ROOT / ".cache/suite")
    parser.add_argument("--output", type=Path, default=ROOT / "results" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--mode", choices=("standard", "bridge", "both"), default="standard")
    parser.add_argument("--case", choices=PIN["cases"], action="append")
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    if args.timeout < 10 or args.timeout > 600:
        parser.error("--timeout must be between 10 and 600 seconds")
    os.umask(0o077)
    try:
        raise SystemExit(run(args))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        parser.exit(1, f"{error}\n")
