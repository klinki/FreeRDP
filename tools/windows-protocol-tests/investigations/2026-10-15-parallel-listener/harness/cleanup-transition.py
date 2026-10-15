#!/usr/bin/env python3
"""Sample the native client's exit transition without reaping before killpg.

Run manually against the known local SDL binary; /version must terminate.
stdout EOF is intentionally observed before wait(), to expose the exit window.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--client", type=Path, default=Path(__file__).resolve().parents[5] /
                    "build/protocol-tests/client/SDL/SDL3/sdl-freerdp")
parser.add_argument("--attempts", type=int, default=100)
args = parser.parse_args()
if not 1 <= args.attempts <= 1000:
    parser.error("--attempts must be between 1 and 1000")
client = args.client.resolve()
outcomes = collections.Counter()
examples = []
for index in range(args.attempts):
    process = subprocess.Popen([str(client), "/version"], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, start_new_session=True)
    process.stdout.read()
    try:
        os.killpg(process.pid, signal.SIGTERM)
        key = "sent"
    except PermissionError:
        code = process.poll()
        key = f"EPERM:poll={code}"
        if code is None:
            started = time.monotonic()
            code = process.wait(timeout=5)
            examples.append(dict(pid=process.pid, exit_code=code,
                                 wait_seconds=round(time.monotonic() - started, 6)))
    except ProcessLookupError:
        key = "ESRCH"
    outcomes[key] += 1
    process.wait(timeout=5)
    process.stdout.close()
with client.open("rb") as stream:
    digest = hashlib.file_digest(stream, "sha256").hexdigest()
print(json.dumps(dict(client=str(client), client_sha256=digest, command=[str(client), "/version"],
                     attempts=args.attempts, outcomes=outcomes, pending_examples=examples), indent=2))
