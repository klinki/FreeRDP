#!/usr/bin/env python3
"""Observe killpg of an exited, unreaped owned process without timing guesses."""
import collections
import json
import os
import platform
import signal
import subprocess
import sys

outcomes = collections.Counter()
exits = collections.Counter()
for index in range(32):
    process = subprocess.Popen([sys.executable, "-c", "pass"], start_new_session=True)
    os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOWAIT)
    try:
        os.killpg(process.pid, signal.SIGTERM)
        outcome = "signal-sent"
    except OSError as error:
        outcome = f"{type(error).__name__}:{error.errno}"
    outcomes[outcome] += 1
    exits[str(process.poll())] += 1
    process.wait(timeout=5)
print(json.dumps(dict(platform=platform.platform(), python=platform.python_version(),
                     attempts=32, signal_outcomes=outcomes, exits_observed_by_poll=exits), indent=2))
