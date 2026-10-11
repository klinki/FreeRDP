# FreeRDP transport benchmark

This integrates the transport-only portion of `~/Downloads/rdp_bench_suite.zip`.
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
timeouts and duplicate responses remain visible. The current production sender
is synchronous: probes and application echoes can block behind reliable writes.
That behavior is measured rather than replaced with a faster benchmark sender.

## Results and history

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
