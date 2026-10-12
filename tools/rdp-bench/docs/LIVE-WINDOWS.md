# Live Windows RDP campaign

`bench.py type2` runs the actual SDL3 client against a provisioned Windows
desktop. It uses a local browser fixture to generate text scrolling or canvas
motion and numbered keyboard probes to measure input-to-visible-response
latency. The transport-only suite and its hash-verified goodput remain separate.

The live runner was adapted from the supplied benchmark ZIP. Copying those
sources does not establish that a particular VM, observer, or profile works;
retain each actual run's preflight and raw artifacts as that evidence.

## Provision the source

Copy `windows/` and `fixtures/` to an owned directory such as `C:\RdpBench`.
Install and record a fixed Python/Playwright version before the campaign. The
agent uses an installed Edge browser through `browser_channel: "msedge"`; it
does not download a browser during a trial.

From the test account's administrator PowerShell session, register the limited
interactive task:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\RdpBench\windows\Install-Task.ps1 `
  -User 'COMPUTER\david' -Python C:\Path\To\python.exe -Root C:\RdpBench
```

The process-only execution-policy switch runs this trusted local script without
changing Windows' global execution policy. The task stores no password. It can
start only in the account's existing interactive session; the agent rejects
Session 0 and records its session ID, process ID, random process UUID, Python,
Playwright, browser, viewport and fixture hashes. Use one test RDP session.

The controller communicates with the task through atomic command/status files
over SSH. A provisioned SSH ControlMaster may be reused through the lab's
`ssh_control_path`; both SSH and SCP use BatchMode and strict host-key checking.
Normal password-authenticated SSH must be established outside the timed run.
Command publication uses .NET `File.Replace` for an existing destination and
`File.Move` only for initial creation; status publication uses `os.replace`.
Each writes a unique closed temporary file before replacement. Explicit Windows
sharing/lock errors 32 and 33 retry, up to 24 attempts with 25–100 ms sleeps
(2.15 seconds of prescribed delay). Other path, permission, malformed-input
and existence errors fail immediately. There is no target delete/copy fallback
and a failed temporary file remains available for diagnosis. A command retains
one request ID and one payload throughout retries, so publication never replays
the agent operation. Python's `MoveFileEx` can report error 5 for a missing
DELETE-sharing permission on an open target. In that case a non-mutating
DELETE-access handle probe must report kernel sharing/lock error 32 or 33
before retrying; actual ACL/read-only access denial still fails immediately.
The probe never deletes the target or uses DELETE_ON_CLOSE. PowerShell passes
`[NullString]::Value` to `File.Replace`, preserving a real null backup path
instead of the empty string produced by its `$null` string binding. The agent
acknowledgement timeout remains 40 seconds; existing SSH-call timeouts remain
unchanged, and the bounded publication retry is separate.

## Build and preflight the observer

```sh
bash tools/rdp-bench/macos/build_observer.sh
tools/rdp-bench/macos/bench-observer --list
```

The script uses a local module cache, overridable with
`RDP_BENCH_SWIFT_MODULE_CACHE`. Configure the lab observer command with
`--display <ID> --rect X,Y,W,H --fps 120 --target-pid {client_pid}`. The
controller substitutes the owned client's PID. Probe events target that process
and require it to be foreground; losing focus produces a method error before
posting input. The rectangle encloses the fixture's
112×64 marker, including its border, in display-relative points. Screen
Recording and Accessibility access, correct foreground client focus and the
US keyboard mapping must work for the actual helper process.
The helper's correlated `foreground_status` stdin operation reads only target
and frontmost PIDs plus existence/termination/activation flags. After requesting
focus, the controller confirms the exact owned live foreground PID within
three seconds before warmup, the keyboard preflight and measured injection.
It may repeat focus requests during those unmeasured checks. There is no
automatic refocusing during measurement: focus loss stops injection and keeps
the PID diagnostics in `probe_error` and the retained failed trial.

The helper emits CRC-valid markers and numbered `probe_injected` records using
macOS monotonic timestamps. A requested 120 Hz capture rate is not proof of
120 Hz sampling. Before measurement the runner requires a valid warmup marker
and an independent keyboard-probe response. Incorrect crop, focus, layout or
permissions stop the trial before its scored phase. Capture timestamps measure
the compositor callback, not physical display scanout.

For unattended campaign admission, set the optional lab
`observer.environment_command` to the same frozen helper with
`--check-environment`. The runner executes this read-only command before any
trial creation/launch and again before each later trial, with credentials
removed from its environment. The helper must exit successfully and explicitly
return JSON `available: true`; false, missing, unknown or malformed evidence
stops the campaign. A nullable private macOS lock field does not override the
helper's computed availability. The complete plan remains visible as incomplete
trials, and `preflight-environment.json` retains the minimal prerequisite
checks. Its read-only CoreGraphics inventory maps active physical display UUIDs
to display IDs. Every selected lab UUID and the observer's calibrated
`--display` ID must remain active; that ID must belong to a selected UUID when
selections exist. Unknown, missing or ambiguous inventory stops the campaign
before the next trial. A built-in display cannot substitute for a disconnected
external display, and the runner does not change display IDs, source geometry
or crop calibration automatically. Configured displays require this environment
command. Without a configured display or this optional command, availability stays unknown and the
ordinary CRC/input preflight remains required. Full live campaigns require an
unlocked interactive desktop and the owned RDP client in the foreground for
the targeted input roundtrip.

## Freeze the client and profiles

Build the checkout's SDL3 client, record the executable and matching dynamic
library hashes, and describe them in a variants JSON:

```json
{"variants":[{"id":"candidate","client":"/absolute/frozen/sdl-freerdp",
 "commit":"ACTUAL_COMMIT","binary_sha256":"ACTUAL_SHA256",
 "build_options":{"WITH_RDP_BENCH":"OFF"}}]}
```

Normal clients keep the production 32 KiB send queue and effective 31-packet
window. The transport adapter's experimental queue environment variables are
not live-client configuration options. A copied executable still needs its
matching libraries and a working rpath; build metadata alone does not prove
that an old executable matches a newer checkout.

Adapt `configs/live-lab.example.json` with the independently verified VM
certificate SHA256 fingerprint, account, SSH control socket, client arguments,
observer command and network adapter. Credentials are supplied through
`RDP_BENCH_PASSWORD` and the launcher's accepted authentication response. They
are removed from every helper/client child environment and never journaled or
included in command-line arguments. Each client uses a fresh private
`XDG_CONFIG_HOME`; measurement requires the expected SHA256 fingerprint to have
been observed in its certificate callback, so a pre-existing certificate cache
cannot silently satisfy the pin claim.

The live network adapter is mandatory. The three command arrays
`apply_command`, `status_command` and `restore_command` receive one final
base64-encoded JSON argument. An apply request includes:

```json
{"op":"apply","run_id":"OWNED_TRIAL_ID","profile_id":"ten_mbit_clean",
 "profile":{"c2s":{},"s2c":{}},"seed":11,"lease_s":300,
 "trial_dir":"/absolute/private/trial"}
```

Return a JSON receipt with `verified: true`, matching run/profile IDs, actual
profile scope and counters/artifact paths. Apply occurs before client startup;
status is checked after warmup, around measurement and after response drain.
Ownership verification also requires live/valid state, matching profile/seed,
balanced packet accounting, a healthy ledger and error-free I/O. Restore runs after
client shutdown, including when apply changed state and then failed. The
adapter must independently expire its lease and avoid changing unrelated
traffic. A relay that shapes UDP while forwarding TCP unchanged must say so:
it is a UDP-profile RDP experiment, not a fully shaped TCP+UDP link.
Both directions must show positive packet deltas during measurement. The closed
ledger must additionally contain exact measured-phase offer/dispatch activity,
server-to-client UDP DATA and p99 dispatch lateness within the default 1.5 ms
budget. Client-to-server DATA is not required: input may use TCP while the UDP
direction carries acknowledgements. These checks still do not establish which
DVC supplied the DATA. Invalid status/timing remains visible as diagnostic
evidence and prevents strict comparisons rather than removing the run.

## Run and inspect

Supply the credential environment through the authorized local secret source,
then start with a new private smoke directory:

```sh
python3 tools/rdp-bench/bench.py type2 \
  --suite tools/rdp-bench/configs/type2.smoke.json \
  --lab /absolute/live-lab.json --variants /absolute/live-variants.json \
  --output /absolute/new-private-smoke
```

The longer `configs/type2.json` has five seeds, both 10 Mbit/s profiles,
60-second measured phases, two probes per second and text/canvas workloads.
Run one client at a time and keep host/guest background activity comparable.
The fixture generates deterministic logical content; Windows still chooses
encoding and changed rectangles, so live repeats are observational.

Each trial retains a manifest, network receipts, received bridge events,
observer and scheduling events, per-window render samples, Windows source and
counter logs, a final summary and SHA256 seals. The agent acknowledges exit
only after all artifact producers close, then SCP collects its files. Optional
`capture.command` starts a packet capture before connection; command arguments
may contain `{trial_dir}` and `{run_id}`. `tls_keylog: true` creates the client's
0600 TLS secrets file inside the private trial directory. Keys and desktop
payloads belong with ignored raw artifacts, not committed compact reports.

`live-results.json` uses schema `freerdp.live_benchmark`, version 1. Every
planned trial remains present, including interrupted, incomplete and failed
runs. Modified sealed artifacts suppress their metrics in the aggregate.
The offline dashboard accepts this file through `dashboard.py --live`.
Comparison keys include actual merged client arguments and relevant nonsecret
environment settings. Campaign provenance pins controller, Windows agent,
fixtures, observer, adapter, executable and declared library/metadata files;
source/binary changes prevent admission. Windows attests its agent/task/counter
sources at startup and again at the end. Per-trial output and certificate-store
paths do not make otherwise matching configurations incomparable.

Metric boundaries:

* `input_visible_ms.*` measures the start of the injected key train to its first
  matching, CRC-valid marker, including responses during bounded drain.
* `input_scheduled_ms.*` includes scheduling and injection delay. Late
  opportunities are recorded as missed injections rather than replayed in a
  burst. Confirmed injections without a response become timeouts; inadequate
  drain becomes explicit censoring. Neither is assigned a latency of zero.
* `progress.*` counts distinct source ticks and measured-phase gaps, including
  censored start/end gaps. Probe-only marker changes are not fresh animation.
  Windows `source` counters cover source phase 2 through its stop, including
  drain and controller communication; they are not aligned to the host's exact
  measured-phase boundaries.
* `traffic.*` reports interval-weighted bridge byte rates grouped by receipt
  time. UDP counts protected datagrams, headers and retransmissions. These
  rates are diagnostic; `verified_goodput_mbps` stays unavailable.
* `transport_rtt_ms.*` describes inferred fresh RDP readings from periodic
  snapshots. Millisecond age quantization is deduplicated conservatively;
  those readings are separate from numbered input latency.
* `render.*` uses only complete measured-phase intervals. Render p95/p99 comes
  from retained samples, never an average of interval percentiles. Texture
  upload bytes are separate from network bytes.

The marker proves that small region's identity and responsiveness. Full-image
fidelity and per-DVC graphics routing need independent capture/replay evidence.
Transport connectivity alone cannot prove graphics were carried over UDP.
Calibration and those limits remain explicit in the dashboard, and per-trial
percentiles are never pooled across repeats.

## Boundary regression tests

```sh
python3 -B -m unittest discover -s tools/rdp-bench/tests -p 'test_live.py' -v
```

These fixtures cover scheduling without catch-up bursts, missing injections,
response drain and censoring, source identity and phase filtering, complete
render intervals, traffic versus goodput, certificate pins, secret-free
journaling, SSH ControlMaster options, restoration after partial apply, and
retention of incomplete planned trials, receipt/accounting/timing admission,
measured UDP DATA evidence, source mutation and WINPR integer-valued float
sequence coverage. Desktop-gate regressions verify that false/unknown
availability launches nothing, strips child credentials and preserves every
planned trial. Display-availability fixtures cover missing UUIDs, stale or
substituted observer IDs, ambiguous inventory and a display disappearing
between trials without further launches. They do not substitute for a live VM
run or an actual permission/observer preflight.

`tests/windows_ipc_atomic.py --root C:\Path\To\NewOwnedRegressionDirectory`
checks actual Windows file sharing and delayed lock release for both publishers.
Run it only after the current campaign has stopped. Use an isolated copy of
`rdpbench/live.py`, `bridge.py`, `common.py`, `windows/agent.py` and this tests
directory, preserving the running agent's attested sources until its campaign
ends. It creates only the supplied new directory, never the real command/status
paths, and retains its JSON evidence. Unit fixtures separately cover bounded
exhaustion, permanent-error rejection and malformed status serialization. The
2026-10-12 VM validation passed both delayed-lock releases, bounded lock failure
and read-only error-5 rejection. An ACL-only denial could not be reproduced
under that test token; its result is explicitly unsupported, without a PASS.
