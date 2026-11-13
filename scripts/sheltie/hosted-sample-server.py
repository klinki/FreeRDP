#!/usr/bin/env python3
"""Spike helper for sheltie-macos#13: runs the loopback sample server behind a TCP relay.

Usage: hosted-sample-server.py SAMPLE_SERVER PORT_FILE [SECONDS]
Writes the relay's TCP port to PORT_FILE and serves for SECONDS (default 120). Throwaway code.
"""
import importlib.util, os, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

here = Path(__file__).parent
spec = importlib.util.spec_from_file_location("smoke", here / "local-session-smoke.py")
smoke = importlib.util.module_from_spec(spec); spec.loader.exec_module(smoke)
sample, port_file = sys.argv[1], Path(sys.argv[2])
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 120
with tempfile.TemporaryDirectory(prefix="rdp-hosted-", dir="/private/tmp") as directory:
    folder = Path(directory)
    certificate, key = folder / "server.crt", folder / "server.key"
    shutil.copy2(here.parent.parent / "server/Sample/test_icon.bmp", folder / "test_icon.bmp")
    subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out",
                    str(certificate), "-days", "1", "-subj", "/CN=localhost"], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, env={k: v for k, v in os.environ.items() if not k.startswith("OPENSSL_")})
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0)); label = reservation.getsockname()[1]
    environment = smoke.bridge.runtime_environment(sample, dict(os.environ, TMPDIR=str(folder) + "/"))
    server = subprocess.Popen([sample, "--local-only", f"--port={label}", f"--cert={certificate}", f"--key={key}"],
                              cwd=folder, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        candidates = [folder / f"tfreerdp-server.{label}", Path("/tmp") / f"tfreerdp-server.{label}"]
        deadline = time.monotonic() + 10; path = None
        while time.monotonic() < deadline and not path:
            path = next((p for p in candidates if p.exists()), None); time.sleep(0.05)
        assert path, "sample server did not start"
        relay = smoke.Relay(str(path))
        port_file.write_text(str(relay.port))
        time.sleep(seconds)
    finally:
        server.terminate()
