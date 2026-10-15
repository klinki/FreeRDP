# Parallel harness regressions

The runner now obtains its RDP endpoint from the Microsoft process that owns the
listening socket. It no longer reserves and releases a port before the driver
starts. A private per-case record carries a random token, the bound loopback
endpoint and the driver PID. The adapter verifies those fields, the owned
process group and the exact connection-request endpoint before launching the
client. Readiness checks do not open a TCP connection.

Each runner process already copied its own suite/configuration. Default result
paths now use an atomic private-directory allocation, so simultaneous starts
cannot collide on the previous timestamp-only path. Control dispatch starts
after the driver process-group identity is available. Per-case `startup.json`
retains endpoint/process provenance, launch counts and adapter failures without
copying the handoff token.

[The eleven regressions](../../../test_parallel.py) cover:

- Eight simultaneous processes retaining eight RDP and eight control listeners,
  unique private output directories, exclusive port ownership, and no readiness
  connection reaching the accept queue.
- Invalid or stale tokens, non-loopback/invalid ports, wrong process groups,
  endpoint changes, missing/nonregular files and reused explicit output paths.
- Two timed-out driver processes releasing their owned listeners while an
  unrelated listener remains bound.
- Child-exit cleanup races, live-child permission failures and command/traceback
  retention for adapter errors.

## macOS cleanup race

A child can exit between `Popen.poll()` and `killpg()`. On this host, signaling
that exited but unreaped process group returns `EPERM` instead of `ESRCH`.
The [standalone reproduction](cleanup-race.py) waits for exit with
`waitid(WNOWAIT)`, without reaping or guessed sleeps. It observed
[32/32 EPERM responses followed by exit code zero](cleanup-race.json).

The deterministic regression arranges exit after cleanup's initial poll and
before the real signal. It [fails against the old implementation](baseline-cleanup-regression.log)
from commit `054b42e81` and passes with the repair. Cleanup suppresses `EPERM`
only after the owned child has been reaped and its exit confirmed. The first
repair used an immediate poll; a later live failure retained a full traceback
proving that `killpg(SIGTERM)` can report EPERM while `poll()` still returns
`None`. The [native-client exit sample](cleanup-transition.json) independently
confirmed that transition in 3/100 `/version` executions: bounded waits reaped
all three with exit code zero after 0.000044–0.004578 seconds. The other 97
children were already reapable. [The sampler](cleanup-transition.py) does not
connect to an RDP server and intentionally does not reap before the signal.

The final repair uses the existing five-second cleanup phase to wait for exit
following EPERM. If that wait expires, it raises the original PermissionError;
it does not ignore the denial or retry the signal. Tests cover both TERM and
KILL phases, including children that remain running. A controlled regression
uses a real child held alive until the bounded wait, with only the signal denial
injected; this is distinct from the actual kernel observations above.

The original three live cleanup logs lacked a syscall traceback, so attributing
those particular failures to this reproduced mechanism remains an inference.
New failures retain the SUT command, stage, exception type and traceback.

## Commands and evidence

Run from the FreeRDP repository root:

```sh
python3 -m unittest discover -s tools/windows-protocol-tests -p 'test_parallel.py' -v
python3 tools/windows-protocol-tests/investigations/2026-10-15-parallel-listener/harness/cleanup-race.py
python3 tools/windows-protocol-tests/investigations/2026-10-15-parallel-listener/harness/cleanup-transition.py
python3 -m unittest discover -s tools/windows-protocol-tests -p 'test_*.py' -v
```

[The final focused run](parallel-cleanup-final-tests.log) passed all eleven checks.
[The earlier ten-check run](parallel-cleanup-tests.log) is retained as intermediate evidence. The retained
[pre-qualification run](prequalification-harness-tests.log) passed 44/45 checks;
its only failure correctly rejected the then-stale eight-patch qualification
record after registration of the ninth patch. It is an intermediate record,
not final integration qualification. See the [parent investigation](../README.md)
and [final parallel validation](../../../validation-parallel.json) for the final
driver, harness and live standard/bridge results.

The process tests use small local listeners and a fake stalled driver where
appropriate; they do not substitute for live Microsoft protocol runs. The
handoff prevents stale/cross-case confusion, not another same-user process
modifying private files. Parallel execution means independent runner processes;
cases within each process remain serial. The historical listener failure's
underlying socket error was discarded and cannot be reconstructed.
