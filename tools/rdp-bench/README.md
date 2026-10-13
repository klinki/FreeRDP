# FreeRDP transport benchmark

This integrates the transport-only portion of `~/Downloads/rdp_bench_suite.zip`.

Severe-loss stress suites add 25%, 50% and 70% independent loss in both
directions, with the original 10 Mbit/s profile as a control. See
[the matrix, failure semantics and commands](docs/LOSS-STRESS-20261013.md).
`configs/type1.loss-stress.json` runs native bulk plus probes;
`configs/type2.loss-stress.json` prepares the corresponding live text workload.
Use one candidate variant; production queue and retry defaults are unchanged.
It runs the actual `rdpeudp.c` reliable UDP2 sender/receiver through real local
sockets, in two independent processes. No Windows machine is required. Python
3.9+ and a POSIX shared-library build are required; the Python harness uses only
the standard library.

The private adapter starts a pre-established IPv4 association. It bypasses SYN,
TLS, tunnels, DVC framing and graphics. Results describe the raw reliable UDP2
stream plus application-driver overhead. They do not establish interoperability
with Windows, desktop FPS or full-RDP performance. `WITH_RDP_BENCH` defaults to
**OFF**; use a separate development build.

## Build and run

From the checkout root:

```sh
python3 -B tools/rdp-bench/build.py
```

The helper builds `freerdp` in `build/rdp-bench`, prints the absolute library
path and saves its binary hash, commit, tracked-diff hash, adapter hash and
actual CMake options in `rdp-bench-build.json`. The runner imports that metadata
when the binary hash matches. Use `--build /absolute/build/path` for another
build directory. Additional CMake flags follow `--`; preserve the dependencies
and codec options appropriate to your checkout. Both macOS and Linux are
supported by the adapter; Linux `netem` requires its own privileged runner.

```sh
# macOS; on Linux use libfreerdp3.so instead.
DUT="$PWD/build/rdp-bench/libfreerdp/libfreerdp3.dylib"

# Independent receiver fixtures, including natural sequence wrap.
python3 -B tools/rdp-bench/bench.py replay \
  --library "$DUT" --wrap --output build/rdp-bench/replay.json

# Three-second smoke measurements: upload, download, mixed and probes.
# Includes clean and lightly impaired 10 Mbit/s profiles.
python3 -B tools/rdp-bench/bench.py type1 \
  --library "$DUT" --peer-library "$DUT" \
  --suite tools/rdp-bench/configs/type1.smoke.json \
  --output tools/rdp-bench/runs/smoke

# All five workloads with the Python relay removed.
python3 -B tools/rdp-bench/bench.py type1 \
  --library "$DUT" --peer-library "$DUT" --direct \
  --suite tools/rdp-bench/configs/type1.direct-smoke.json \
  --output tools/rdp-bench/runs/direct
```

Each run creates a unique trial directory; existing trials are preserved.
`--case ten-clean-up` selects a single case. Omitting `--suite` runs the frequent
10-second, three-seed preset. `type1.release.json` uses 30-second measurements
and five seeds; smoke results are for checking operation, not performance claims.
Warmup and integrity drain are configured separately. Outage offsets in a
profile are milliseconds from **trial start, including warmup**.

## Profiles and workloads

The caps in `configs/profiles.json` include UDP datagrams plus 28 bytes of
IPv4/UDP overhead. Zero rate means uncapped. Directions are candidate-to-peer
(`c2s`, upload) and peer-to-candidate (`s2c`, download). Delays are added one-way
delay; total RTT also includes processing and scheduling.

| Profile | Added RTT | Upload/download cap | Loss per direction |
|---|---:|---:|---:|
| LAN | 2 ms | 100/100 Mbit/s | 0 |
| Office | 20 ms | 100/100 Mbit/s | 0.01% |
| Home | 40 ms | 20/100 Mbit/s | 0.10% |
| Remote | 60 ms | 10/50 Mbit/s | 0.05% |
| `ten_mbit_clean` | 30 ms | **10/10 Mbit/s** | 0 |
| `ten_mbit` | 30 ms | **10/10 Mbit/s** | 0.05% |

There are also reordering, brief blackout, deep-queue stress and unshaped
loopback profiles. Normal queue budget is 30 ms per direction. The relay records
random, queue and blackout drops separately. Seeded decisions are reproducible
for the same arrival trace; changed packetization/timing does not imply the same
individual packets will be lost. There is no second Python reliability stack.

Upload/download continuously send seeded payload records without waiting for
application echoes. Duplex sends bulk both ways. Probe generates scheduled
64-byte probes, and mixed combines outgoing probes with incoming bulk. Probes
are scheduled independently of replies. Driver backpressure, missed deadlines,
timeouts and duplicate responses remain visible. Each binary's production write
policy and byte order are preserved: probes and application echoes can wait
behind accepted reliable bytes. The harness records that backpressure and adds
no alternate sender or parallel application writer.

## Results and history

Queue-cap experiments use `queue_sweep.py` with a frozen library built by
`build.py`. The runner generates 32/48/64/128 KiB variants, fixes the reference
peer at 128 KiB, and preserves the default 31-packet window. Its main campaign
runs upload and outgoing bulk-plus-probes (`upload_mixed`) on both 10 Mbit/s
profiles, with five seeds and 30-second measurements. Separate shorter campaigns
check unloaded probes and queue-observer overhead. For example:

```sh
python3 -B tools/rdp-bench/queue_sweep.py \
  --library /absolute/path/to/frozen/libfreerdp3.dylib \
  --build-metadata /absolute/path/to/rdp-bench-build.json \
  --output build/queue-sweep-new
```

The private adapter validates `RDP_BENCH_SEND_QUEUE_BYTES` as one of 32768,
49152, 65536 or 131072. `RDP_BENCH_QUEUE_OBSERVER=0` disables only queue
observation; unavailable measurements remain missing. These options exist only
in `WITH_RDP_BENCH` builds. Normal builds retain the 32 KiB queue bound.

Queue residence measures transport bytes from successful ownership acceptance
to the first successful send invocation. Its timestamp precedes the nonblocking
send call and is counted only after success; kernel completion and wire departure
are outside this interval. Retransmissions add no residence weight.
Bytes accepted during the measurement phase are followed through drain.
Admission timing separately measures calls started during measurement until
acceptance, including capacity waiting. Histograms provide 0.125 ms percentile
intervals; scalar percentiles are their upper bounds and maxima are exact.
Overflow, clock misalignment or incomplete cohort accounting invalidates queue
telemetry. Scheduled probe RTT/deadline accounting remains separate because
byte-weighted residence is dominated by bulk data. Histograms are collected only
after the endpoint loop ends. Probe injection is timestamped before send
admission, so its RTT includes admission blocking; scheduled RTT also includes
owner scheduling delay.

`queue_report.py --root build/your-sweep` audits the main campaign and exports
JSON, per-trial CSV, grouped CSV and a report with strict paired comparisons.
`queue_observer_report.py --root build/your-sweep` compares observer on/off at
each cap. Both retain failed and invalid trials and expose valid-only subsets.
The [Mac queue sweep record](docs/QUEUE-SWEEP-20261011.md) includes the measured
tradeoffs and every scheduling/timing failure.

`ack_timing_sweep.py` repeats the 48/64 KiB comparison with the same 128 KiB
peer and both 10 Mbit/s profiles. Baseline has five seeds and 5/30/10-second
warmup/measurement/drain phases for upload and outgoing bulk-plus-probes. ACK
diagnostics have three seeds and 3/15/10-second phases for outgoing
bulk-plus-probes at 0, 5, 10 and 20 ms additional S→C feedback delay. For example:

```sh
python3 -B tools/rdp-bench/ack_timing_sweep.py \
  --library /absolute/path/to/frozen/libfreerdp3.dylib \
  --build-metadata /absolute/path/to/rdp-bench-build.json \
  --output build/ack-timing-new
```

The default serial sequence runs 8 smoke, 40 baseline and 48 ACK diagnostic
trials. `--prepare-only` writes the complete machine-readable plan, variants,
fixed peer environment, source/binary/profile fingerprints and phase suites.
`--phase smoke`, `baseline` or `ack-timing` permits separate invocations;
`--phase control` adds 12 unloaded-probe controls. Existing phases and changed
provenance are rejected. Each completed phase generates audited queue reports
with 48/64 KiB pairs and all failures. No acceptance receipt is invented.

`ack_timing_report.py` audits completed phases and independently reconciles
every ACK dispatch ledger with the requested offsets, delivery counts and
timing summaries. It exports portable JSON/CSV/Markdown and protects source,
binary and campaign inputs from output overwrites:

```sh
python3 -B tools/rdp-bench/ack_timing_report.py \
  --root build/ack-timing-new --output build/ack-timing-new/report
```

The [peer-window and ACK timing record](docs/PEER-WINDOW-ACK-20261011.md)
contains the measured results, calibration limits and failures; committed
[JSON](results/peer-ack-20261011.json) and [CSV](results/peer-ack-20261011.csv)
retain per-trial metrics, subset counts and strict paired comparisons.

This is a controlled feedback-delay experiment, not Windows ACK batching.
The case-level `ack_feedback` object accepts `c2s_delay_ms` and
`s2c_delay_ms`. The relay reverses the UDP2 prefix protection and validates the
visible header layout; only ACK/ACKVEC packets without DATA get a monotonic
deadline offset. DATA with piggybacked ACKs, control, dummy and unknown packets
receive none. Datagram bytes, network serialization, queue/drop rules and
seeded impairment decisions are unchanged. Delayed feedback can arrive after
DATA whose original network deadline was later; this is an intentional effect
of the treatment. The treatment enters each comparison key and is rejected for
direct sockets, Linux netem and calibration-only endpoints.

Baseline omits classification. Every ACK diagnostic, including 0 ms, collects
the same classification/timing observations. `shaper.json` records requested
delay, classifications, delivered counts, actual extra delivery delay above
the original network deadline, and overshoot above the treated deadline.
Overshoot remains subject to the normal relay timing validity threshold.
`ack-feedback.jsonl` records every selected delivered packet's direction, kind,
length, original deadline, treated deadline and actual dispatch timestamp.
Records are buffered during the trial and written after the relay stops; ledger
overflow or any unclassified native datagrams invalidates the experiment.
These timestamps describe local relay dispatch, not receiver ACK-generation
delay or physical NIC departure. Trials preserve CRC/hash/order verification,
scheduled/injected/observed probe counts and the existing integrity drain.

Both native lab endpoints retain their genuine default LogWindow 5 and a
31-packet effective window. Production-path regression fixtures cover tracking
the Windows-advertised LogWindow 15; this small lab peer does not advertise a
capacity it lacks. Neither a larger peer window nor an ACK delay changes the
normal production 32 KiB queue default. A short zero-ms diagnostic and the
longer baseline provide descriptive observer evidence; their different
measurement durations prevent a strict cross-phase paired claim.

The classifier also has an optional test against actual native ACK and DATA
wire bytes, rather than only hand-encoded layouts:

```sh
RDP_BENCH_TEST_LIBRARY=/absolute/path/to/frozen/libfreerdp3.dylib \
  python3 -B -m unittest discover -s tools/rdp-bench/tests -p test_ack_feedback.py
```

`loss_stall_sweep.py` compares frozen reference/candidate libraries at 48/64 KiB
on both 10 Mbit/s profiles, using the same old 128 KiB peer. Its 40-trial main
campaign is outgoing bulk-plus-probes with five seeds and 5/30/10-second phases.
Separate traced timer/fast controls use the same candidate binary, and separate
trace-on/off controls measure instrumentation cost. The complete plan has 56
trials. `--prepare-only` records provenance; `--phase` permits resumable phases
without replacing existing evidence.

```sh
python3 -B tools/rdp-bench/loss_stall_sweep.py \
  --candidate /absolute/path/to/candidate/libfreerdp3.dylib \
  --reference /absolute/path/to/reference/libfreerdp3.dylib \
  --candidate-metadata /absolute/path/to/candidate-build.json \
  --reference-metadata /absolute/path/to/reference-build.json \
  --output build/loss-stalls-new
python3 -B tools/rdp-bench/loss_stall_report.py \
  --root build/loss-stalls-new --output build/loss-stalls-new/report
python3 -B tools/rdp-bench/loss_stall_compact.py \
  --input build/loss-stalls-new/report/aggregate.json \
  --output tools/rdp-bench/results/loss-stalls-new
```

`RDP_BENCH_LOSS_TRACE=1` enables bounded, buffered native/application/relay
ledgers; logs are written after endpoint and relay loops stop. Overflow, clock
alignment or incomplete ledger accounting invalidates the trace. Visible UDP2
header identifiers link actual drops to native gap evidence, retry timing and
blocked admission. `RDP_BENCH_LOSS_RECOVERY_MODE=timer` disables early recovery
only in the benchmark adapter. Normal builds contain neither option nor trace
hooks. The runner pins treatment and trace settings; the main comparison omits
loss tracing. Reporters retain native evidence, relay-confounded waits, unknown
intervals and every failure. The compact exporter consumes an already audited
aggregate; it does not repeat or replace the artifact audits.

The [loss-stall record](docs/LOSS-STALLS-20261012.md) explains the conservative
three-receipt/25 ms recovery rule, both profile reruns, calibration limits and
failed preflight. Committed [JSON](results/loss-stalls-20261012.json) and
[CSV](results/loss-stalls-20261012.csv) preserve strict pair counts, all-observed
diagnostics, missed probes and sparse causal evidence. The production 32 KiB
queue and 31-packet default effective window are unchanged.

Each trial contains `manifest.json`, phase clocks, endpoint JSONL traces,
endpoint result JSON, stderr logs, `shaper.json`, `summary.json`, and
`SHA256SUMS.json`. The campaign also writes `campaign.json`. Successful transfers
are verified with record CRCs, seeded content, sequence ordering and matching
sender/receiver SHA-256 hashes and byte counts after a bounded drain.

The main fields in `summary.json.metrics` are:

- `c2s.verified_goodput_mbps` / `s2c.verified_goodput_mbps`: verified payload
  bytes read during the measurement window divided by its full wall-clock
  duration. Record headers, probes, retransmissions and drain bytes are excluded.
  A record may finish validation during drain without moving its measured bytes
  out of their original window. Failed integrity verification yields `null`.
- `client.probe_rtt_ms.p50/p95/p99/max`: injected-to-response latency, on the
  candidate's monotonic clock.
- `client.probe_scheduled_ms.p50/p95/p99/max`: intended-deadline-to-response
  latency, including local backpressure.
- Probe expected/injected/observed/missed counts and timeouts. Percentiles
  describe observed responses; missing responses are never converted to zero.
- `client/server.send_call_ms`, bulk record delivery gaps, total process CPU
  time and native counters. CPU includes Python/ctypes, warmup and drain;
  completed-record gaps are distinct from individual datagram gaps.

```sh
python3 -B tools/rdp-bench/bench.py report \
  --input tools/rdp-bench/runs --output tools/rdp-bench/runs/dashboard
python3 -B tools/rdp-bench/bench.py audit /absolute/path/to/one/trial
python3 -B tools/rdp-bench/ci/check_results.py tools/rdp-bench/runs/smoke
```

The reporter emits `data.json`, long-form `metrics.csv`, append-only
`history.sqlite` and a self-contained `index.html`. Trial points include invalid
and failed runs. Open the HTML in a browser; it has case/configuration/metric
selectors. Tampered artifacts are flagged. Hash manifests detect changes but
are not authenticity signatures. Keep reports outside the trial directory.

`sut_outcome` and timing validity are separate. Missed probe deadlines or
undrained data make the trial fail; excessive relay dispatch lateness marks
`INVALID_HARNESS`. A completed native trial without a calibration receipt is
`PROVISIONAL_UNCALIBRATED`. The campaign exits nonzero for failed trials or
invalid harness timing, while retaining their artifacts and metrics.

## Comparing implementations

Copy `configs/variants.example.json` and set actual absolute library paths,
commits, build options and environment settings. Run variants in one campaign
with an explicit **fixed reference peer**. The peer environment defaults to an
empty overlay on the inherited environment; `--peer-env /absolute/peer.json`
specifies it independently of candidate tuning.

```sh
python3 -B tools/rdp-bench/bench.py type1 \
  --variants /absolute/variants.json --peer-library /absolute/reference/library \
  --suite tools/rdp-bench/configs/type1.release.json \
  --output tools/rdp-bench/runs/comparison
```

Case, profile, durations, load, peer hash/environment, harness hash and host
identity define a comparison group. Variant order is shuffled reproducibly per
seed. The same candidate library may be used as the peer for an initial smoke
run, but shared bugs and a slow reference peer can conceal problems. Freeze the
reference binary and establish its headroom for comparisons. Keep inherited
environment settings, dependencies, power and background load fixed.

The comparison command admits only calibrated, complete, untampered trials:

```sh
python3 -B tools/rdp-bench/bench.py compare \
  --input tools/rdp-bench/runs/comparison --baseline baseline --candidate candidate \
  --metric c2s.verified_goodput_mbps --output tools/rdp-bench/runs/comparison.json
```

It pairs case/configuration/seed, reports a paired median-difference bootstrap
interval and requires at least five pairs before classifying a change. Failed
runs remain in the exclusion list and must be reviewed separately.

## Calibration and Linux kernel shaping

The portable relay records dispatch lateness. Its p99 budget is
`max(0.5 ms, 10% of configured one-way delay)`; socket errors invalidate the run.
It is unsuitable for attributing high-rate CPU/batching changes unless its
capacity and timing have been independently calibrated. Use direct sockets for
native capacity/CPU investigation and Linux `netem` for kernel-shaped checks.
Results from different engines stay in different comparison groups.

```sh
# Run these on a dedicated Linux machine/VM with iproute2 and netem support.
sudo python3 tools/rdp-bench/linux/ns_lab.py create --state /tmp/rdpbench-ns.json
sudo python3 -B tools/rdp-bench/bench.py type1 \
  --library /absolute/candidate/libfreerdp3.so \
  --peer-library /absolute/reference/libfreerdp3.so \
  --suite tools/rdp-bench/configs/type1.kernel.json \
  --netns /tmp/rdpbench-ns.json --output /tmp/rdpbench-kernel-results
sudo python3 tools/rdp-bench/linux/ns_lab.py destroy --state /tmp/rdpbench-ns.json
```

The lab creates owned endpoint/router namespaces and veths. It changes their
queues, not the host's ordinary NIC queues. It records qdisc and offload results.
Timed blackouts and the portable relay's exact extra-delay reordering model are
refused by the kernel adapter. Kernel queue limits approximate time budgets;
calibrate the actual path. Linux CPU measurements do not prove macOS CPU gains.

Create a **nonapproved** calibration template with:

```sh
python3 -B tools/rdp-bench/ci/make_acceptance_template.py \
  --engine python-relay --output /absolute/calibration.json
```

Retain independent rate/delay/loss, at least 2x packet-rate headroom, reference
peer/driver headroom and instrumentation-overhead evidence. Fill the named
operator approval, host/engine, expiry, fixed peer SHA-256, calibrated limits
and at least two hashed evidence files. Pass `--calibration` to the runner.
`VALIDATED_LAB` means local operator acceptance with matching evidence, not
independent certification. No approved receipt is supplied.

## Tests and CI

```sh
python3 -B -m unittest discover -s tools/rdp-bench/tests -v
cmake --build build/rdp-bench --target TestCore --parallel 8
ctest --test-dir build/rdp-bench -R '^TestRdpeUdp$' --output-on-failure
```

Set `DUT_LIBRARY`, `PEER_LIBRARY`, `DUT_COMMIT` and `OUTPUT_ROOT` to actual
absolute paths/values, then run `bash tools/rdp-bench/ci/run_type1.sh`. Optional
variables are `SUITE_CONFIG`, `DIRECT_SOCKETS=1`, `NETNS_STATE`,
`CALIBRATION_RECEIPT`, and `REQUIRE_CALIBRATION=1`. The script publishes reports
even after a benchmark failure. Frequent checks allow provisional results;
release admission should require calibration.

See [SPECIFICATION.md](SPECIFICATION.md) for measurement/admission rules and
[VALIDATION.md](VALIDATION.md) for execution actually performed in this checkout.

## Mac calibration and baseline campaigns

`calibrate.py --output /absolute/evidence` independently checks the portable
relay using raw UDP packet CRCs, sequences and receive timestamps, both 10 Mbit/s
profiles, paced/saturated directional rates, unloaded latency/loss, and packet
headroom with bursts. It writes empirical criteria and an **unapproved** evidence
record. A successful transfer alone does not approve the lab receipt.

The private adapter defaults to enabled counters. `RDP_BENCH_COUNTERS=0` disables
those counters, and `RDP_BENCH_OBSERVER=0` suppresses periodic/probe-event observer
logging while preserving integrity checks, phase/error/missed-probe events and
aggregate metrics. Both are benchmark-only calibration options and are recorded
in manifests. Missing native counter observations remain unknown and prevent
lab admission. Use the normal enabled defaults for the baseline.

`baseline_report.py --input /absolute/campaign --output /absolute/report`
aggregates all repeats and retains failures. Case tables report medians of
per-trial goodput/percentile values, rather than incorrectly pooling percentiles.
Calibrated and uncalibrated subsets remain explicit.

## Live Windows desktop campaigns

`bench.py type2` runs a provisioned Windows browser fixture through the actual
SDL3 client, records numbered input-to-capture probes, source progress, render
samples and transport traffic, and retains missing responses and failures.
It requires a verified network adapter and an observer preflight; live traffic
rates remain separate from transport-only verified goodput. See
[LIVE-WINDOWS.md](docs/LIVE-WINDOWS.md) for provisioning, both 10 Mbit/s profiles,
credential handling, metric boundaries and the machine-readable campaign format.

For the Windows server-counter loss ramp and packet/ACK recovery evidence,
see [UPSTREAM-METHOD.md](docs/UPSTREAM-METHOD.md) and
[UDP-RECOVERY-20261013.md](docs/UDP-RECOVERY-20261013.md). The latter documents
bounded optional core tracing, the independent PCAP header audit, and the
25%/50%/70% campaign; its server counters have separate metric boundaries.
The matched transport rerun on both 10 Mbit/s profiles is recorded in
[RECOVERY-TEN-MBIT-20261013.md](docs/RECOVERY-TEN-MBIT-20261013.md), including
verified payload goodput, probe p95/p99 and retained failures.

## Packet-loss and outstanding-packet matrix

[FLIGHT-MATRIX.md](docs/FLIGHT-MATRIX.md) describes the benchmark-only exact
16/32/48/64 outstanding DATA limits, fixed byte queue, 0/25/50/70% loss
profiles and transport dashboard. Use `flight_sweep.py` to freeze and run the
80-condition matrix, then `flight_report.py` to re-audit the sealed evidence.
