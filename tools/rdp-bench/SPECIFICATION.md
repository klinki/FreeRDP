# Transport benchmark specification, version 1

## Measurement boundary

The DUT is this checkout's production raw reliable UDP2 byte stream. Two
processes each load one FreeRDP shared library with a build-gated private adapter.
Each association starts connected, using the existing default sequence state,
window settings and MTU/payload calculation. Sending, socket I/O, ACK processing,
retransmission and reassembly run in production code. The adapter does not
implement reliability. Its single owner drives sending and receiving; no
parallel application writer is added to bypass a synchronous production sender.

Exclude TLS, negotiation, tunnels, DVC, graphics and Windows interoperability
from all claims. The socketless replay verifies independent receive fixtures
and is not a virtual-time simulation of the real sender. The `udp-calibration`
driver sends plain UDP, has no ARQ, and is never a FreeRDP performance result.

## Trial protocol

1. Validate configuration and create an independent trial directory.
2. Record exact profile, case, seed, library hashes, known build metadata,
   candidate/reference environment overlays and host/runtime identity.
3. Apply kernel shaping or start a seeded portable relay; initialize both
   endpoints and require their readiness before arming the common clock.
4. Warm up from the planned monotonic start. Begin measuring at the configured
   boundary, and stop generating new bulk/probes at its end. Preserve both
   planned boundaries and observed endpoint phase events.
5. Continue driving the production transport through bounded drain. Verify
   content, ordering, record CRCs, byte counts, hashes and pending probes.
6. Close producers, preserve endpoint/relay errors and process exits, summarize,
   then seal every trial file with SHA-256. Never discard failure artifacts.

All clocks are local monotonic clocks on one physical host. Linux namespaces
share that host clock. Start/warmup/measure/stop/drain controls are absolute
nanoseconds, independently observed by both endpoint processes. Outage offsets
are relative to trial start, including warmup.

## Goodput and latency

Goodput counts unique, verified application bulk payload bytes observed at the
receiver's read boundary inside `[measure_ns, stop_ns)`. The parser tracks byte
attribution across fragmented records; a record finishing during drain can
validate already-measured bytes. Headers, probes/echoes, retransmissions,
warmup-only bytes and drain-only bytes do not count. Divide by the full configured
measurement duration, including stalls, and use decimal Mbit/s. Read-buffer
timing is an application observation, not a physical packet-arrival timestamp.

Seeded payload comparison plus sequential record IDs detect wrong content,
duplicates and reordered application delivery. CRC covers record metadata and
payload. End-to-end hashes and byte totals cover the whole trial after drain.
Raw receiver rates remain diagnostic on failures; `verified_goodput_mbps` is
`null` if integrity could not be verified. Missed probe service may still fail
the trial even when bulk integrity and its goodput are verified.

Probes are scheduled independently of replies. Their IDs, scheduled and injected
timestamps and payload must match a pending request. Record actual RTT and
scheduled-to-observed latency, p50/p95/p99/max, sample counts, missed deadlines,
timeouts and every probe event. Percentiles use nearest rank and cover observed
responses only. Missing responses are shown separately, never represented as
zero latency. Low sample counts, especially smoke-run p99, are diagnostic.

Bulk gaps are completed-record delivery gaps, including a final censored gap at
measurement end. They are sensitive to record size and distinct from packet
arrival gaps. Total endpoint CPU includes driver, ctypes and native work across
warmup/measurement/drain. It is not isolated library CPU.

## Network profiles and repeatability

Profiles separately specify c2s and s2c one-way delay, uniform jitter, independent
random loss, duplication, extra-delay reordering, decimal Mbit/s cap, queue budget,
IP/UDP overhead and half-open blackout intervals. The portable model serializes
datagrams before propagation delay and records each drop category and timing
error. Directional seeds are recorded; a fixed seed repeats decisions for the
same trace, not identical packet losses across changed packetizations.

Use the included clean and lightly impaired 10/10 Mbit/s, 30 ms added RTT
profiles for limited-bandwidth comparisons. A 10 Mbit/s configured cap includes
wire overhead and does not promise 10 Mbit/s application goodput. Deep queue
stress is separate from normal 30 ms directional queue budgets.

Portable, direct and kernel engines must not be pooled. Direct mode refuses
profiles requiring shaping. Linux kernel queue budgets are packet-count
approximations and need calibration. Time-dependent blackouts and exact portable
reordering are explicitly unsupported there.

## Admission and comparison

`sut_outcome=COMPLETE` requires complete endpoints, matching integrity evidence,
served/observed probes and no partial records after drain. `FAIL` retains the
failure reason. Excessive relay p99 dispatch lateness, relay/socket errors or
runner failures produce `INVALID_HARNESS`. Artifact modification/missing/unsealed
files produce `INVALID_ARTIFACT` on ingestion. These states remain visible in
history; invalid timing cannot be interpreted as a transport regression.

Uncalibrated native completion is `PROVISIONAL_UNCALIBRATED`. `VALIDATED_LAB`
requires a named operator, matching host/engine/engine-source fingerprints,
unexpired receipt, fixed peer binary fingerprint, calibrated rate, at least 2x
observed packet-rate headroom, and at least two retained hashed evidence files.
This gate verifies receipt consistency; the operator must evaluate the evidence
itself, including effective shaping, peak/burst headroom, driver/reference-peer
capacity, instrumentation overhead, environmental stability and baseline noise.
No runtime can certify those properties by merely checking a transfer succeeded.

Comparison uses only calibrated, complete, untampered trials with identical
case/configuration/host/reference/harness keys and paired seeds. Use at least
five paired 30-second runs, vary order reproducibly, and retain all seeds and
failures. The paired-run median-difference percentile bootstrap is exploratory
for small samples. A statistical interval excluding zero is not by itself a
useful optimization: the comparator also applies a 5% practical threshold
(and a 5 ms floor for latency). Review correctness/reliability failures alongside
any speed improvement.

## Initial investigation bands

These are versioned starting product policies, not Microsoft requirements or
universal usability limits. Establish calibrated baselines before making gates.

| Metric | High target | Initial acceptable target | Investigate |
|---|---:|---:|---:|
| Clean goodput / calibrated payload ceiling | ≥90% | ≥70% | <40% |
| Loaded p95 excess over unloaded same-path p95 | ≤20 ms | ≤50 ms | >150 ms |
| Confirmed corruption, duplicate delivery or unexplained missing bytes after drain | 0 | 0 | Any event fails integrity |
| Unserved or unobserved probes | 0 | 0 | Any event; inspect backpressure/timeouts |

Threshold evaluation is manual; the suite does not invent a payload-capacity
denominator or claim a universal hard unusability boundary. Batch residence and
syscall telemetry are future native additions; the current adapter exports
datagram, retransmission, loss-observation, outstanding-entry and buffer counters.

## Remaining validation requirements

Calibrate relay and kernel-link timing/capacity on their actual hosts. Verify
fixed-peer/driver headroom, including mixed/duplex and instrumentation on/off.
Run the Windows desktop/interoperability suite separately before claiming better
desktop experience or protocol conformance. See VALIDATION.md for what was
actually executed here; the Linux namespace engine has not been exercised on
this macOS machine.
