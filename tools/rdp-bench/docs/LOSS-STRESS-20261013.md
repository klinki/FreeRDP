# Severe packet-loss stress tests

New profiles apply independent **25%, 50% and 70% loss on each UDP
direction**, including DATA, ACKs, retries and keepalives. These are fixed
random-loss levels, not burst loss or time-varying transitions. The existing
`ten_mbit` profile is the 0.05% control. Each direction retains 10 Mbit/s,
15 ms delay, ±1 ms jitter, a 30 ms queue and 28-byte IPv4/UDP overhead;
duplication and deliberate reordering remain zero.

Seeds reproduce packet-index decisions. The reverse direction has an
independent random lane/seed offset. Different traffic does not guarantee
identical logical losses. Random drops consume serialization capacity;
queue and blackout drops remain separate.

## Matrix

| Parameter | Native transport | Live Windows configuration |
|---|---|---|
| Profiles | Control, 25%, 50%, 70% | Control, 25%, 50%, 70% |
| Seeds | 11, 29, 47, 71, 101 | 11, 29, 47, 71, 101 |
| Variants | One current candidate | One current candidate |
| Workload | Outgoing bulk plus echo probes | Text scrolling, 400 px/s |
| Warmup / measurement / drain | 5 / 30 / 12 seconds | 5 / 60 / 3 seconds |
| Application load | Continuous 16 KiB records | 30 Hz source target |
| Probes | 20 Hz | 2 Hz |
| Conditions | 20 | 20 |

Suite configs are `configs/type1.loss-stress.json` and
`configs/type2.loss-stress.json`; use a one-entry variants file. The current
native campaign keeps the production 32 KiB queue and 31 active packets.
This test addition changes no production retry budget or recovery policy.
Native trials bypass SYN, TLS, DVC, Windows and rendering. Live trials need
the usual VM/display/foreground preflight; the previously used live relay
shapes UDP while leaving TCP unshaped.

## Run and inspect

Build and pin a benchmark-enabled library with `build.py`, then:

```sh
python3 -B tools/rdp-bench/bench.py type1 \
  --suite tools/rdp-bench/configs/type1.loss-stress.json \
  --variants /absolute/path/candidate-only-variants.json \
  --peer-env /absolute/path/pinned-peer-env.json \
  --output build/new-loss-stress/main
python3 -B tools/rdp-bench/loss_stress_report.py \
  --root build/new-loss-stress/main \
  --output build/new-loss-stress/report/main
```

The campaign exits nonzero on service/transport/timing failures and still
writes sealed artifacts and campaign rows. The reporter audits seals,
reconciles relay accounting and checks complete-stream bytes/hashes. Outputs
are JSON, one-row-per-trial CSV and Markdown. Preserve frozen sources,
binaries/configs and sealed trials; choose a new directory for another run.

Terminal failures retain prefix bytes/hashes, native retry/failure flags,
queue state, partial records and failing operation/time. Attempted probe
calls reconcile with injection events, with admission/failure counts
separate. Early exit marks future probes unserved and pending replies
unobserved; it does not invent timeouts before the drain deadline.
Snapshot/cleanup errors and runner-enforced termination remain qualification
failures. A terminal transport failure's incomplete queue cohort remains
telemetry evidence rather than a supposed relay scheduling failure.

Observed p95/p99 cover surviving replies only. Group values are medians of
per-trial percentiles; samples are never pooled. Read latency beside
expected/injected/observed/unserved/unobserved counts. Goodput summaries
require timing-valid COMPLETE integrity-verified transfers. Failed transfers
are unavailable; received prefix bytes are diagnostic evidence.

Configured and realized loss remain separate. Realized random loss uses
offers eligible after queue/blackout drops; those drops are also reported.
Pending copies reconcile across directions. No accepted calibration receipt
is implied by the local stress campaign.

## Mac campaign

Current native sources are `94ee369e8`, compiled in a separate Release build.
Frozen harness/configs, source fingerprints, native build metadata and all
trial artifacts are in `build/loss-stress-20261013/`. Four separately traced
smoke trials use seed 11 and 1/3/12-second intervals. Main has 20 untraced
trials. The four smoke trials have two COMPLETE and two FAIL outcomes, with
intact artifacts and no timing-invalid trials. The 25% smoke transferred
all 80 probes; the 50% and 70% smoke traces reached five retries and retained
terminal native send failures. A short successful run does not establish
sustained service at that loss level.

Main results are medians of whole-trial statistics. Failure times include
all observed artifact-intact trials; six stress trials also fail the relay
dispatch timing budget and remain explicitly invalid.

| Loss each direction | Complete / failed | Verified goodput Mbit/s | Observed p95 / p99 ms | Expected / injected / observed | Median first failure |
|---|---:|---:|---:|---:|---:|
| 0.05% control | 5 / 0 | 4.059 (N=5) | 67.098 / 96.612 (N=5) | 3500 / 3500 / 3500 | None |
| 25% | 0 / 5 | Unavailable | 679.781 / 806.116 (N=4, survivors only) | 3500 / 1063 / 998 | 8.351 s |
| 50% | 0 / 5 | Unavailable | Unavailable | 3500 / 49 / 15 | 1.267 s |
| 70% | 0 / 5 | Unavailable | Unavailable | 3500 / 17 / 2 | 1.554 s |

All 15 stress trials record native terminal failure flags; all complete
control streams pass byte/hash integrity. No queue drops occur. Realized
random-loss medians (forward/reverse) are 25.389%/24.727%,
53.353%/47.603%, and 70.930%/63.265% for the three stress levels. Severe
runs end quickly and have small packet cohorts, so their realized fraction
can differ noticeably from the configured probability.

Unserved/early-unobserved counts are 2437/65 at 25%, 3451/34 at 50%, and
3483/15 at 70%. There are no fabricated deadline timeouts: pending replies
are censored by termination. At 50% and 70%, the client fails during the
five-second warmup, so no measured-response latency is available.

Timing-invalid main trials are 25% seeds 29/101, 50% seeds 29/71/101, and
70% seed 71. Their p99 dispatch lateness exceeds the unchanged 1.5 ms
budget. Failed-trial latency/failure-time summaries remain diagnostic and
do not establish exclusive causation or accepted lab calibration.

The new tests therefore expose the current reliable transport's severe-loss
service limit. They do not justify enlarging queues or silently increasing
retry limits. Retry/failure behavior under sustained loss is the next
production investigation. The live Windows configuration is prepared but
was not run in this campaign; no desktop/session reliability claim is made.

Validation: 228 Python tests, 226 passed and two optional native-fixture
tests skipped; a fresh benchmark-enabled native build; four smoke and 20
main raw native trials; all 24 artifact sets independently audited. No
production transport source or the previously delivered Sheltie build was
changed. Endpoint/report parsing refinements after freezing the campaign
affect malformed-evidence handling only; raw trials and their executed
source snapshot are preserved.

Committed [JSON](../results/loss-stress-20261013.json),
[CSV](../results/loss-stress-20261013.csv) and
[Markdown](../results/loss-stress-20261013.md) retain every main trial.
