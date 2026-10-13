# Validation performed in this checkout

## Calibration and repeated baseline on perf-bench

A subsequent Mac run on 11 October 2026 created and checked out `perf-bench`,
froze candidate/reference binaries and measurement sources, and completed:

- 15 direct native capacity trials, all passing integrity and delivery checks.
- 12 paired native instrumentation on/off trials, all passing.
- 36 independent raw-relay calibration samples: 35 passed; one clean-profile
  unloaded sample exceeded the existing dispatch/RTT timing limits. No threshold
  was relaxed or lab receipt approved.
- 40 native baseline trials: both 10 Mbit/s profiles with upload, download,
  unloaded probes and mixed traffic, five seeds, 5 s warmup, 30 s measurement,
  15 s drain and 20 Hz probes. All data verified after drain. Three trials were
  timing-invalid and two missed scheduled probe injections (three missed
  injections total, all during measurement); every injected probe was observed.
- Artifact hashes audited successfully for all 103 native/raw sample directories;
  frozen source and binary fingerprints remained unchanged.
- The expanded pure regression suite has 39 passing tests.

The baseline and CI gate returned 1 because failures were retained. The run is
provisional; direct CPU headroom does not remove the fixed reference sender's
stop-and-wait limit under RTT. Download figures describe the association, not
isolated DUT receiver capacity. The summary, machine-readable results, full
per-trial ranges and offline dashboard are retained at
`build/perf-bench-20261011-1447/summary.md` and its sibling files/directories.

## Initial integration validation

Executed on 11 October 2026 on the local macOS arm64 machine. The source ZIP
was read from `~/Downloads/rdp_bench_suite.zip`; only transport components were
integrated. These are actual executions, not the archive's earlier Linux/mock
validation claims. All logs and trial artifacts are retained under the
gitignored `build/rdp-bench` directory.

## Build and correctness checks

- Release `freerdp` and `TestCore` compiled successfully with the private adapter.
- An OFF build exported no `freerdp_bench_v1_open`; after restoring ON, all five
  private functions were present. The final runnable build is ON. Its helper
  metadata and actual CMake options were imported into trial manifests.
- **27 Python regression tests passed**, covering 10 Mbit/s serialization,
  deterministic faults, queue bounds, measurement-boundary byte attribution,
  corruption, missing/tampered artifacts, failed initialization/cancellation,
  missing-adapter rejection, campaign failure status, independent peer options,
  calibration checks and paired reporting/comparisons.
- Native `TestRdpeUdp` passed through CTest (5.62 seconds in the final run).
- All seven production receiver replay cases passed, including natural sequence
  wrap. The final replay ran with Python optimization enabled, confirming that
  correctness checks do not disappear under `-O`.
- Python source parsing, Bash syntax and `git diff --check` passed.

## Actual native socket trials

The direct smoke campaign completed upload, download, duplex, probe and mixed
workloads with matching integrity evidence, no missed probes and no trial
failures. Its exit status was zero. The artifact/correctness CI gate accepted
these provisional results; the stricter calibration gate rejected them because
no approved calibration receipt exists.

The final portable-relay smoke campaign retained five trials:

| Case | Outcome | Observed result |
|---|---|---|
| LAN upload | COMPLETE | 3.081 Mbit/s verified upload |
| Clean 10 Mbit/s upload, 30 ms added RTT | COMPLETE | 0.265 Mbit/s verified upload |
| Clean 10 Mbit/s download, 30 ms added RTT | COMPLETE | 0.265 Mbit/s verified download |
| Lightly impaired 10 Mbit/s mixed | COMPLETE | 0.131 Mbit/s verified download; probe p95 1058 ms, p99 1134 ms |
| Remote probes, 60 ms added RTT | FAIL | Observed RTT p95 63.38 ms, p99 63.74 ms; unserved scheduled probes retained |

All five retained byte integrity. The remote probe failure is visible alongside
observed-response percentiles, and the campaign exited nonzero. The current
synchronous sender cannot sustain the configured 20 Hz probe schedule on that
path. Mixed latency includes application echo scheduling behind bulk reliable
writes; it is not an RDP input-to-visible measurement.

A further native download trial through the `burst` profile completed with
matching hashes after **two blackout-dropped datagrams and eight native
retransmissions**. The trial used 0.5 seconds warmup, 5 seconds measurement and
3 seconds drain. The blackout at 3 seconds was exercised; the profile's second
blackout at 6 seconds fell after this trial's measurement window.

All these native results are **PROVISIONAL_UNCALIBRATED**. An earlier smoke trial
was correctly marked `INVALID_HARNESS` for relay dispatch lateness; thresholds
were not relaxed. The final smoke trials stayed inside the per-trial relay
timing budget, which is necessary but insufficient for lab acceptance.

## Reporting and CI behavior

JSON, CSV, SQLite and offline HTML exports were produced from actual native
trials. Trial hashes audited successfully. Missing trials/summaries and artifact
tampering are covered by regression checks. Uncalibrated trials are excluded
from the statistical comparison command.

The complete Bash CI entry point was executed with a failing remote-probe case.
It ran the unit tests and native replay, preserved the failed trial, generated
the dashboard and returned exit status **1**. That run also retained an excessive
relay timing error (s2c p99 dispatch lateness 20.26 ms versus a 3 ms budget).
The script's empty optional arrays were verified/fixed for macOS Bash 3.2.

## Not established

No lab acceptance receipt was approved and no release-sized performance
comparison was certified. Reference-peer/driver capacity, effective shaping and
instrumentation overhead still require independent calibration evidence. No
Linux namespace/netem execution occurred on this Mac. No Windows desktop,
TLS, DVC, handshake, graphics or physical display latency was benchmarked.

Retained locations: `python-tests-final.log`, `native-core-tests.log`,
`native-replay.log`, `direct-native/`, `final-shaped/`, `burst-native/`,
`dashboard/`, `shaped-dashboard/`, and `ci-failure/`, under `build/rdp-bench`.
