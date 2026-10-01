#!/usr/bin/env python3
"""Exercise the real client's standalone discovery; never contacts an RDP host."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(binary, arguments, environment):
    return subprocess.run([binary, *arguments], env=environment, capture_output=True,
                          text=True, timeout=3, stdin=subprocess.DEVNULL)


def main():
    binary, enabled, tls_enabled = sys.argv[1:]
    supported = enabled == "ON"
    query = "/launcher-capabilities"
    with tempfile.TemporaryDirectory(prefix="freerdp-capabilities-") as directory:
        # Preferences are read during context creation. Reading this FIFO blocks,
        # proving discovery never reads configuration or constructs that context.
        preferences = Path(directory) / "freerdp"
        preferences.mkdir()
        os.mkfifo(preferences / "sdl-freerdp.json")
        environment = dict(os.environ, XDG_CONFIG_HOME=directory,
                           SDL_VIDEODRIVER="launcher-query-invalid-driver",
                           SDL_AUDIODRIVER="launcher-query-invalid-driver")
        response = run(binary, [query], environment)
        if supported:
            require(response.returncode == 0, f"Query failed: {response.stderr}")
            require(response.stderr == "", f"Unexpected setup/log output: {response.stderr}")
            require(response.stdout.endswith("\n") and response.stdout.count("\n") == 1,
                    "Discovery must emit one JSON line")
            result = json.loads(response.stdout)
            require(set(result) == {"schemaVersion", "client", "engineVersion",
                                    "bridgeProtocolVersion", "capabilities"}, "Wrong schema fields")
            require(type(result["schemaVersion"]) is int and result["schemaVersion"] == 1,
                    "Wrong schema version")
            require(result["client"] == "sdl3", "Wrong client identity")
            require(isinstance(result["engineVersion"], str) and result["engineVersion"],
                    "Missing engine version")
            require(type(result["bridgeProtocolVersion"]) is int and
                    result["bridgeProtocolVersion"] == 1, "Wrong bridge version")
            expected = ["auth", "certificate", "focus", "retry",
                    "display_uuid", "per_monitor_scaling", "dynamic_resolution", "multimon",
                    "close_confirmation", "session_thumbnail", "dock_accessory", "primary_monitor", "reverse_mouse_wheel"]
            if tls_enabled == "ON":
                expected.append("tls_keylog")
            require(result["capabilities"] == expected,
                    "Wrong compiled capabilities")
        else:
            require(response.returncode != 0 and response.stdout == "" and
                    response.stderr == "Launcher bridge support is not compiled in\n",
                    "Bridge-disabled client must reject discovery before setup")

        # Every combination is rejected at the same early entry point, including
        # query last, repeated queries, transport arguments, config paths, and input.
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            destination = f"/v:127.0.0.1:{listener.getsockname()[1]}"
            for extra in ([destination], ["/u:query-test", "/p:synthetic-test-value"],
                          ["/launcher-fd:3", "/launcher-session:query-test"],
                          ["/args-from:stdin"], ["/from-stdin"], ["/version"],
                          [str(preferences / "sdl-freerdp.json")], [query]):
                for arguments in ([query, *extra], [*extra, query]):
                    response = run(binary, arguments, environment)
                    require(response.returncode != 0 and response.stdout == "" and
                            response.stderr == query + " must be the sole argument\n",
                            f"Combined query reached setup or accepted arguments: {arguments}")
            listener.settimeout(0.1)
            try:
                peer, _ = listener.accept()
            except TimeoutError:
                pass
            else:
                peer.close()
                raise AssertionError("Discovery attempted a network connection")
    print("Launcher capability query, headless setup, and standalone argument tests passed")


if __name__ == "__main__":
    main()
