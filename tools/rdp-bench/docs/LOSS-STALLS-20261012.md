# UDP2 loss stalls and recovery on this Mac

The sender received selective ACK evidence of a missing packet but waited for
its 200 ms retransmission timer. The missing packet retained reliable-stream
ownership, later packets filled the bounded sequence window, and application
admission blocked. Scheduled probes behind that work could become more than
200 ms overdue and never be injected. Owner-pump events continued during the
waits. The traces support a native recovery delay; they also preserve relay
lateness and intervals that cannot be measured with the old fixed peer.

## Change and defaults

`rdpeudp.c` now permits early recovery after three distinct, valid, generated
later receipts identify a hole and 25 ms have elapsed since the first meaningful
gap. Duplicate vectors, future bits, stale negative aliases and trailing FALSE
bitmap padding cannot supply this evidence. The evidence resets for each
transmission attempt. Positive ACKs for retained aliases still retire the same
owned data. Retries retain the existing channel order, replacement allocation,
window/storage limits and retry budget.

Three receipts avoid treating one repeated vector as multiple confirmations.
The 25 ms grace preserves the deliberate 15 ms reordering fixture and tolerates
the profiles' ±1 ms jitter. It is a conservative fixed grace for this change,
not an adaptive RTT estimator. The original 200/400/600/800/1000 ms fallback,
five-retry budget and 10 s send timeout remain available when later receipts
are absent. Sparse traffic can therefore still wait for timer recovery.
Production defaults remain a 32 KiB owned-byte queue, local LogWindow 5 and
31 effective active packets. The 48/64 KiB caps below are benchmark overrides.
Peer LogWindow 15 tracking and the 63-packet storage ceiling are preserved.
The diagnostic loss counter now ignores bitmap padding; a meaningful gap
observation is still not proof of an actual network drop.

`WITH_RDP_BENCH` adds optional buffered native events for sends, gaps, ACKs,
retirement, retries, admission, waits and owner pumps. Python records application
call spans, missed injections and relay offers/drops/dispatches. Protected UDP2
header identifiers link the ledgers without retaining payloads. Native and
Python monotonic clocks are aligned and checked. Logs are written after loops
stop; bounded-buffer overflow or incomplete accounting invalidates the trace.
Normal builds compile these trace hooks away. Benchmark tracing is opt-in via
`RDP_BENCH_LOSS_TRACE=1`; `RDP_BENCH_LOSS_RECOVERY_MODE=timer` supplies a
benchmark-only causal control. Neither setting exists in normal builds.

`loss_stall_report.py` audits artifact seals and independently reconciles native,
application and relay records. It separates native admission with matched timer
recovery evidence, relay-confounded waits, and unknown intervals. It retains
failed trials and reports both all-observed and strict complete paired subsets.
`loss_stall_compact.py` exports that already audited report as portable JSON/CSV
and Markdown, including sparse causal chains with a reconstructable clock epoch.

## Conditions and provenance

- Apple M4 MacBook Air, macOS 27.0.1, Python 3.14.5. Timed trials ran serially
  from frozen source without builds or tests competing with the campaign.
- Both directions capped at 10 Mbit/s, including 28 bytes of IPv4/UDP overhead.
  Clean adds 30 ms RTT; impaired adds ±1 ms one-way jitter and 0.05% random loss
  per direction. No additional ACK-delay treatment was applied.
- MTU 1232, maximum UDP2 payload 1132, 16 KiB application records and 20 Hz
  scheduled probes. DUT caps 48/64 KiB, fixed reference peer 128 KiB. Both lab
  endpoints advertise LogWindow 5 and retain a 31-packet effective window.
- Reference native sources match commit `a01c6f6dc`. Its original build metadata
  records the earlier parent plus dirty sources; that metadata is retained with
  the explicit resolved source commit. Candidate was built from `a01c6f6dc`
  plus this change. Source snapshots, patches, actual CMake options, hashes and
  frozen adjacent WinPR libraries are retained with the raw campaign.
- Reference/fixed-peer SHA-256:
  `b4c64d6d1342e65fc7402d57aa2cecb8c62e2256f0324bdca9a67ee6103c2bfd`.
  Candidate SHA-256:
  `8ea2df26cd91236e1957caf5301014b13e243225a1d8023feab164104caded4f`.
- Main: 40 outgoing bulk-plus-probes trials, five seeds (11,29,47,71,101),
  5/30/10 s warmup/measurement/drain. Reference and candidate use the same old
  fixed peer. Variant order is deterministically shuffled within profile/seed.
  Main omits loss tracing. Seeds reproduce packet-index impairment decisions;
  changed traffic does not guarantee identical logical packets are dropped.
- Separate same-candidate-binary timer/fast diagnostics: four smoke trials,
  four impaired seed-29 trials at 5/30/10 s, and four impaired seed-101 trials at
  3/15/10 s. Four clean 48 KiB trace-on/off controls use seeds 11/29 and 3/10/5 s.
- Goodput counts measured application payload verified by byte/hash/order checks
  after drain. Tables show medians of whole-trial statistics without pooling
  percentiles. RTT starts before native admission; scheduled RTT starts at the
  intended injection deadline. Percentiles cover observed measurement probes;
  missed injections remain separate. The miss threshold remains >200 ms.
- This is raw native reliable UDP2 over local sockets and a user-space relay.
  SYN, TLS, tunnels, DVC, graphics and Windows interoperability are outside the
  measured path. No accepted calibration/headroom receipt exists.

## Main results

Strict same-seed pairs require intact artifacts, timing validity, integrity and
complete service on both sides. Values are reference → candidate. Excluded
reference failures are listed below and remain in the machine-readable results.

| Profile / cap | Paired N | Verified goodput Mbit/s | RTT p95 ms | RTT p99 ms | Scheduled p99 ms |
|---|---:|---:|---:|---:|---:|
| Clean / 48 KiB | 5 | 8.216 → 8.208 | 48.396 → 48.280 | 50.338 → 49.928 | 64.027 → 64.025 |
| Clean / 64 KiB | 5 | 8.813 → 8.798 | 74.931 → 75.014 | 75.808 → 76.439 | 88.860 → 89.177 |
| Impaired / 48 KiB | 3 | 7.728 → 8.179 | 64.141 → 64.459 | 232.035 → 87.310 | 241.025 → 102.375 |
| Impaired / 64 KiB | 4 | 8.037 → 8.495 | 78.686 → 80.645 | 246.750 → 105.628 | 260.709 → 123.327 |

For the impaired profile, strict-pair goodput increases about 5.8% at 48 KiB and
5.7% at 64 KiB; RTT p99 falls about 62% and 57%. Clean goodput changes by less
than 0.2%. Five-pair all-observed impaired diagnostics, including failed reference
service, give goodput 7.732 → 8.213 / 8.048 → 8.497 Mbit/s and RTT p99
232.035 → 87.310 / 246.935 → 104.588 ms at 48/64 KiB. These subsets must not be
mixed when comparing metrics.

All 20 candidate main trials pass: 14,000/14,000 scheduled probes are injected
and observed, with zero misses or timeouts. The reference misses one injection
in each of impaired 48 KiB seeds 29/101 and impaired 64 KiB seed 101. All
injected reference probes are observed. There are no main timing-invalid trials.
Median per-trial maximum blocked admission on impaired 48/64 KiB falls from
188.885/188.185 ms to 42.038/52.216 ms in the all-observed comparison.
The largest individual candidate main send still takes 180.779 ms. This run
shows fewer loss waits and no candidate misses; it does not eliminate every
long admission or scheduling interval.

48 KiB remains the leading live-validation candidate: it keeps lower loaded
latency than 64 KiB while recovering much of the loss-related goodput deficit.
The production queue default is unchanged by this commit.

## Native recovery versus relay timing

All 111 traced recovery records reconcile with the original native and relay
ledgers: 55 timer retries and 56 fast retries. Each has matched C→S drop evidence,
valid gap feedback, sequence/channel identifiers and continued owner-pump events.
Across these records, median first-gap-to-retry time is 169.38 ms for timer
recovery and 25.41 ms for fast recovery. From the third confirming receipt,
the medians are 166.64 and 23.39 ms. The grace is implemented on the existing
millisecond clock; nanosecond trace differences can straddle a clock tick.

There are 54 timer-control admission calls lasting at least 100 ms. Of these,
52 have matched native recovery evidence with overlapping relay dispatch
lateness at most 1.371 ms. Two remain relay-confounded, with 27.162 and 8.507 ms
lateness. Fast diagnostics have zero admission calls at least 100 ms. Three of
the four timer diagnostic misses link to native recovery calls; the fourth
remains relay-confounded. All four matching fast diagnostics pass without misses.

For example, the impaired smoke timer attempt retries at age 204 ms, 171.451 ms
after gap feedback. The fast attempt retries at age 59 ms, 25.851 ms after the
gap. Both link to actual random C→S drops. This supports repairing delayed
native recovery instead of changing relay deadlines to conceal a stall.
It does not establish exclusive causation for every slow interval. Fixed-peer
ACK creation, unmeasured source/socket intervals and host scheduling remain
explicitly unknown.

The four separate trace-on/off controls pass all checks. Across their two pairs,
median endpoint CPU total rises about 4.9% with tracing (DUT alone about 7.4%);
goodput changes by about +0.4%. These short controls describe instrumentation
cost rather than certifying its effect on every tail event. The main comparison
is untraced.

## Failures and validation

| Phase | Trials | Integrity passes | Service failures / missed injections | Timing / artifact / trace invalid |
|---|---:|---:|---:|---:|
| Corrected smoke | 4 | 4 | 0 / 0 | 0 / 0 / 0 |
| Main | 40 | 40 | 3 / 3 | 0 / 0 / 0 |
| Diagnostic seed 29 | 4 | 4 | 2 / 2 | 0 / 0 / 0 |
| Diagnostic seed 101 | 4 | 4 | 2 / 2 | 0 / 0 / 0 |
| Trace overhead | 4 | 4 | 0 / 0 | 0 / 0 / 0 |
| Total | 56 | 56 | 7 / 7 | 0 / 0 / 0 |

All 33,593 injected probes are observed; seven of 33,600 deadlines are not
injected and there are zero response timeouts. The four diagnostic service
failures are timer controls at both caps for seeds 29/101. Candidate fast
recovery has no service failure in any completed phase.

Fresh relay calibration passes 29/36 samples. Seven failures include unloaded
RTT, dispatch and one-way delay envelopes. The clean paced S→C seed-47 sample
also loses 37 packets to unintended relay queue drops and fails independent
seeded-loss accounting. Accepted headroom is zero; no receipt is issued.
Calibration was collected before the trace-only argument-name repair below;
its source fingerprint is retained separately rather than claimed to certify
the final harness. The final native campaign passes its per-trial timing and
artifact checks, but these calibration limits make performance claims provisional.

An earlier four-trial trace smoke attempt failed when a trace method's `kind`
argument collided with protected-header metadata of the same name and stopped
the relay. The argument is now `event_type`; a real-socket native-DATA regression
covers the collision. All four failed trials, their source and reports remain
under `failed-trace-smoke-v2/` and in the compact result sidecar. They are harness
failures, separate from the 56 corrected campaign trials. An earlier reporter
preflight is retained as well. Nothing was discarded to produce a passing subset.

Validation actually performed: benchmark build passes all four native UDP test
targets; normal build passes the three applicable targets and has no private
`freerdp_bench` exports. Five new native sender regressions cover distinct
evidence/padding/future bits, 15 ms reordering, ordered first-loss recovery,
retry attempts/aliases, wrap/window shrink/multiple holes; existing no-feedback
timer tests still pass. Sequence-wrap replay passes. All 105 Python tests pass
against the frozen candidate, including actual wire bytes, native traces,
real relay sockets, ledger tampering/count/deadline checks, trace validity and
compact sparse timestamps. Independent source and raw-ledger reviews pass.

## Reproduce and inspect

Build and freeze candidate and reference libraries with their adjacent WinPR
dependencies and build metadata, then run from the checkout root:

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

`--prepare-only` writes frozen provenance and all phase plans. Separate `--phase`
invocations resume completed phases only with identical inputs; unfinished
directories and changed provenance are rejected. Service failures return nonzero
while preserving completed trials and reports. Tracing, calibration and control
failures are never converted to successful numeric samples.

[JSON](../results/loss-stalls-20261012.json),
[per-trial CSV](../results/loss-stalls-20261012.csv),
[compact audited report](../results/loss-stalls-20261012.md).
Raw seals, ledgers, calibration, failed preflight, frozen libraries/WinPR, source,
metadata, replay and test logs remain under `build/udp-loss-stalls-20261012/`.
