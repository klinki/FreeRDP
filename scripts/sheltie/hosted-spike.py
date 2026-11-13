#!/usr/bin/env python3
"""Spike driver for sheltie-macos#13: a hosted session against the loopback sample server.

Starts the sample server and the engine in hosted render mode, hands the exported layer's
context ID to a host program (the probe from the Sheltie repository), then plays a script of
layout and input commands. Throwaway code.

Usage: hosted-spike.py ENGINE SAMPLE_SERVER HOST_BINARY [--seconds N] [--log DIR]
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

here = Path(__file__).parent
spec = importlib.util.spec_from_file_location("smoke", here / "local-session-smoke.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)
bridge = smoke.bridge


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("sample_server")
    parser.add_argument("host")
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--log", type=Path, default=Path.cwd())
    parser.add_argument("--target", help="host[:port] of a real RDP host instead of the sample server")
    parser.add_argument("--user", default="launcher-test")
    parser.add_argument("--password", default="synthetic-test-password")
    parser.add_argument("--size", default="800x500")
    parser.add_argument("--script", type=Path, help="JSON list of [seconds, type, fields] to send instead of the default")
    parser.add_argument("--mode", choices=("hosted", "sdl"), default="hosted")
    parser.add_argument("--perf", action="store_true", help="enable engine performance samples and log them")
    parser.add_argument("--host-size", nargs=2, default=None, help="host window size in points")
    parser.add_argument("--icon", type=Path, default=here.parent.parent / "server/Sample/test_icon.bmp")
    args = parser.parse_args()
    args.log.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="rdp-hosted-", dir="/private/tmp") as directory:
        folder = Path(directory)
        certificate, key = folder / "server.crt", folder / "server.key"
        shutil.copy2(args.icon, folder / "test_icon.bmp")
        subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key),
                        "-out", str(certificate), "-days", "1", "-subj", "/CN=localhost"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       env={k: v for k, v in os.environ.items() if not k.startswith("OPENSSL_")})
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            label = reservation.getsockname()[1]
        environment = bridge.runtime_environment(args.sample_server, dict(os.environ, TMPDIR=str(folder) + "/"))
        server_log = (args.log / "server.log").open("wb")
        server = subprocess.Popen([args.sample_server, "--local-only", f"--port={label}", f"--cert={certificate}",
                                   f"--key={key}"], cwd=folder, env=environment, stdout=server_log, stderr=server_log)
        host = client = relay = None
        try:
            candidates = [folder / f"tfreerdp-server.{label}", Path("/tmp") / f"tfreerdp-server.{label}"]
            deadline = time.monotonic() + 10
            path = None
            while time.monotonic() < deadline and not path:
                path = next((p for p in candidates if p.exists()), None)
                time.sleep(0.05)
            assert path, "sample server did not start"
            relay = smoke.Relay(str(path))
            client = bridge.Client(args.engine, environment=dict(os.environ, XDG_CONFIG_HOME=str(folder / "config"),
                                                                 WLOG_LEVEL="INFO"),
                                   log_path=args.log / "engine.log")
            client.socket.settimeout(30)
            hello = client.hello()
            print("capabilities:", ",".join(hello["capabilities"]))
            target = args.target or f"127.0.0.1:{relay.port}"
            codec = ["/gfx"] if args.target else ["/rfx"]
            client.send("start", arguments=[f"/v:{target}", f"/u:{args.user}", f"/size:{args.size}", *codec,
                                             "/timeout:5000", "-multitransport", "-auto-reconnect",
                                             "+dynamic-resolution"],
                        displaySelections=[], thumbnailsEnabled=False, performanceSupported=args.perf,
                        **(dict(renderMode="hosted") if args.mode == "hosted" else {}))
            script = [  # seconds after the layer appears, command, fields
                (3.0, "layout", dict(width=640, height=400, scale=2)),
                (5.0, "input", dict(action="mouse_move", x=320, y=200)),
                (5.2, "input", dict(action="mouse_button", button=1, down=True, x=320, y=200, clicks=1)),
                (5.3, "input", dict(action="mouse_button", button=1, down=False, x=320, y=200, clicks=1)),
                (5.5, "input", dict(action="key", keyCode=0, down=True, flags=0)),
                (5.6, "input", dict(action="key", keyCode=0, down=False, flags=0)),
                (7.0, "layout", dict(width=800, height=500, scale=2)),
            ]
            if args.script:
                script = [tuple(entry) for entry in json.loads(args.script.read_text())]
            shown = None
            end = None
            client.socket.settimeout(0.2)
            give_up = time.monotonic() + 30
            while (end is None and time.monotonic() < give_up) or (end is not None and time.monotonic() < end):
                while shown and script and time.monotonic() - shown >= script[0][0]:
                    _, kind, fields = script.pop(0)
                    if kind == "cpu":
                        pids = dict(engine=client.process.pid, windowserver=int(subprocess.check_output(
                            ["pgrep", "-x", "WindowServer"]).split()[0]))
                        if host:
                            pids["host"] = host.pid
                        sample = {}
                        for name, pid in pids.items():
                            m, sec = subprocess.check_output(["ps", "-o", "cputime=", "-p", str(pid)]).decode().strip().rsplit(":", 1)
                            sample[name] = sum(int(x) * 60 ** i for i, x in enumerate(reversed(m.split(":")))) * 60 + float(sec)
                        print(f"t={time.monotonic() - shown:.1f} cpu {json.dumps(dict(wall=time.monotonic(), **sample))}")
                        continue
                    print(f"t={time.monotonic() - shown:.1f} send {kind} {fields}")
                    client.send(kind, **fields)
                try:
                    message = client.receive()
                except (TimeoutError, socket.timeout):
                    continue
                if message["type"] == "auth_request":
                    client.send("auth_response", requestId=message["requestId"], accepted=True, username=args.user,
                                domain="", password=args.password)
                else:
                    smoke.respond(client, message)
                if message["type"] == "performance_sample":
                    with (args.log / "perf.jsonl").open("a") as out:
                        out.write(json.dumps(message["performance"]) + "\n")
                    continue
                if message["type"] == "state" and message.get("phase") == "connected":
                    if args.perf:
                        client.send("performance_control", performanceEnabled=True, performanceOverlayVisible=False,
                                    performanceGeneration=1)
                    if args.mode == "sdl" and shown is None:
                        shown = time.monotonic()
                        end = shown + args.seconds
                if message["type"] != "state" or message.get("phase"):
                    print("event:", {k: v for k, v in message.items() if k not in ("v", "sessionId")})
                if message["type"] == "ended":
                    break
                if message["type"] == "layer_context" and not host:
                    shown = time.monotonic()
                    end = shown + args.seconds
                    host = subprocess.Popen([args.host, str(int(message["contextId"]))] + (["-", *args.host_size] if args.host_size else []),
                                            stdout=(args.log / "host.log").open("wb"), stderr=subprocess.STDOUT)
        finally:
            for process in (host, getattr(client, "process", None), server):
                if process and process.poll() is None:
                    process.terminate()
            if relay:
                relay.accepting = False


if __name__ == "__main__":
    main()
