# Bounded UDP2 sender experiment

The benchmark implementation and original Mac baseline are committed on
`perf-bench` at `94dd73b5d`. The sender experiment is on
`codex/udp-windowed-sender`, based on that commit.

## Behavior and defaults

Established reliable writes copy the whole byte range before returning and use
an ACK-clocked pipeline. Successful BIO writes mean local acceptance; a later
failure is terminal. Stream order is preserved, including probes behind already
accepted bulk bytes. TLS setup writes drain synchronously because the normal
main-loop maintenance timer is not yet active.

The default advertised LogWindow remains 5, permitting at most 31 active DATA
sequence numbers, following the
[protocol sequence-number bound](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/ea4b6ac5-f574-4631-89a0-42a98ef78551).
New data also observes the peer's smaller window and the
63-entry local storage bound. If a peer shrinks its window while old attempts
are outstanding, retries may replace those attempts within their existing span;
they cannot widen it or admit new channels using that allowance.

Normal queued and in-flight payload ownership together is limited to 32 KiB.
A single valid write up to 60,000 bytes can be admitted atomically after all
preceding bytes drain. Backpressure services incoming feedback and retries.
Each logical channel chunk owns one payload and at most six transmission
aliases. Five retries use 200/400/600/800/1000 ms timeouts, with a 10-second
overall deadline. Cumulative ACKs retire the latest attempt; explicit positive
ACKVEC bits may retire a received older alias. AOA cannot prove payload receipt:
the [sender window](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/d0a98be6-8063-4445-a6e2-9c5725d17d06)
can advance past attempts declared lost, which still require reliable delivery.

Pure ACK/AOA packets advance the receive boundary monotonically while preserving
channel reassembly. Dummy packets retain transport control processing but never
deliver higher-layer bytes. Short protected ACK vectors are parsed using their
encoded layout length, excluding protection padding. These follow the
[packet network format rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/9a92f0eb-7353-4c6a-859e-eab072c36c30).

Active main-loop maintenance runs every 20 ms and returns to one second after
drain. Queue wake generations protect writers from event-reset races. Socket
readiness and manual buffered/send notifications are separate events exposed to
the main loop: POSIX socket-attached events cannot carry synthetic notifications.
Packet state snapshots and bookkeeping use the recursive transport lock.

## Paired Mac measurements

Three seeds (11, 29, 47), 2-second warmup, 10-second measurement, 8-second drain,
16 KiB payload records and 20 Hz probes. Both profiles cap each direction at
10 Mbit/s with 30 ms added RTT; the impaired profile adds +/-1 ms one-way jitter
and 0.05% loss. Tables give medians across repeats, not pooled percentiles.

Upload and unloaded probes hold the original frozen peer fixed. Download and
mixed comparisons hold the frozen windowed peer fixed. Comparisons across those
two peer groups do not isolate a DUT improvement. These are exploratory results;
the Mac calibration remains unapproved and no acceptance threshold was relaxed.

| Verified payload goodput, Mbit/s | Original | Windowed | Paired ratio |
|---|---:|---:|---:|
| Clean upload | 0.266 | 3.971 | 14.944x |
| Impaired upload | 0.261 | 3.952 | 15.328x |

| Injected-to-echo RTT p95 / p99, ms | Original | Windowed |
|---|---:|---:|
| Clean unloaded probes | 32.360 / 32.530 | 32.313 / 32.560 |
| Impaired unloaded probes | 33.546 / 33.988 | 33.627 / 33.970 |
| Clean mixed | 90.687 / 91.030 | 75.117 / 76.496 |
| Impaired mixed | 94.405 / 149.868 | 76.120 / 159.563 |

All 48 transfers verified payload order, contents, byte counts and hashes after
drain. All 5,760 expected probes were injected and observed, with no missed
injections or response timeouts. Forty-six trials passed timing checks. Two
original-DUT clean downloads were **INVALID_HARNESS**: seed 11 relay p99 lateness
was 70.621 / 94.228 ms (forward/reverse), and seed 47 was 2.710 / 2.403 ms, against
the unchanged 1.5 ms budget. Their values remain in diagnostic artifacts and
are excluded from valid paired ratios. Only one clean-download pair is valid;
download is roughly 4.07--4.13 Mbit/s under the fixed peer's sender limit, with
no clear receiver throughput improvement supported by this experiment.

The impaired mixed p99 median **regressed** by 9.694 ms. Seed 11 worsened, while
seeds 29 and 47 improved. The worst candidate mixed p99 was **227.956 ms**.
Unloaded impaired p99 also reached **195.694 ms** in one candidate trial. The
experiment supports a substantial upload gain and a mixed p95 gain, without
establishing a general improvement in loss-related tails.

The 32 KiB owned-byte limit barely misses holding two framed 16 KiB records;
clean upload sends therefore often wait one RTT (~33 ms) for capacity. Testing
queue size together with pacing and tail latency is the next tuning step.
Negative ACKVEC observations rose under pipelined jitter; they describe transient
missing/reordered attempts, not measured network packet loss. Native retries,
actual shaper drops and negative feedback remain separate in the detailed data.

## Validation scope

`TestRdpeUdp` and all 19 deterministic `TestRdpeUdpSend` scenarios pass. The new
cases capture real protected datagrams and cover pipelining, selective ACKs,
window shrink with loss, storage bounds, copied payload ownership, retry limits,
late aliases, sequence wrap, AOA/cumulative-ACK retirement, dummy control,
cancellation and terminal failure. The seven production receiver replay cases,
including natural wrap, and all 39 benchmark Python tests pass. The production
core also builds and passes the native test targets with `WITH_RDP_BENCH=OFF`;
the resulting library exports no private benchmark symbols.

Four POSIX `TestRdpeUdpEvent` scenarios use real socket-attached events: queued
DATA loss wakes the separate manual event, both handles reach the actual
main-loop event list with capacity/deduplication checks, a writer racing the
generation snapshot retains its wake, and buffered bytes remain signaled.
Consumed generations clear readiness rather than spinning on pending bytes;
the health pump retries lost data after 200 ms.

The native campaign exercises the production raw UDP2 sender and receiver over
local sockets. It does not execute the multitransport maintenance callback,
TLS setup, or Windows interoperability. Those integration paths received
static review. This bounded ACK-clocked pipeline is not a complete
adaptive congestion controller.

The paired campaign froze raw candidate binary SHA-256
`9cfe9040e6070b1e51f1b1ed5181c301a2ac8ef3cab448afa0a67dd361a10645`.
The separate manual/socket-event integration fix followed that campaign; it
changes a path outside its raw-harness scope. Final integration candidate
SHA-256 is `22ba8790cc7cbd21e410c830949466dabf7a7963ff45ccc331dd4cce5c2e2842`.
Eight fresh final-binary smoke trials (one seed, 3-second measurements) pass
timing, integrity and service checks under both profiles and both peer groups.
Upload remains 3.965 / 4.063 Mbit/s (clean/impaired). Clean mixed p95/p99 is
75.456 / 76.366 ms; impaired mixed is 145.484 / 246.927 ms. Clean unloaded p99
reaches 70.752 ms in this short follow-up. These additional tail observations
reinforce the need for longer repeats and approved calibration before making a
release decision. They are retained in `final-smoke-summary.json` and do not
replace the paired campaign.

Machine-specific binary snapshots, source patch, manifests, checksums, JSON,
CSV and detailed results are retained under `build/udp-windowed-20261011/`.
The original 30-second baseline remains independently retained under
`build/perf-bench-20261011-1447/`.
