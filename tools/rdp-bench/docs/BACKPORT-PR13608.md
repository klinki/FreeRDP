# Selected UDP reliability backports from PR 13608

This branch adapts three parts of [FreeRDP PR 13608](https://github.com/FreeRDP/FreeRDP/pull/13608)
to the existing `rdpeudp` implementation: supported capability flags,
retransmission/peer-liveness policy, and receiver ACK timing. The reviewed
upstream snapshot is `d148cdb76`; this is a selective adaptation, not a
replacement of our transport with the PR's `rdpudp` implementation.

## Behavior

The command-line default, `/multitransport` and `+multitransport` advertise
only `TRANSPORT_TYPE_UDP_FECR`, the implemented reliable UDP transport.
`-multitransport` clears both support and flags. Enabling the switch no
longer advertises lossy UDP FECL or Soft-Sync/static-channel migration.
Repeated switches retain the last setting.

Each new outgoing logical chunk starts with
`RTO = min(120000 ms, max(300 ms, 2 × SRTT))`. Original, unambiguously
acknowledged transmissions update SRTT using `(7 × previous + sample) / 8`.
Karn's rule excludes every retransmitted logical chunk, including an explicit
ACK for its original alias. Retransmissions double their fallback timer up
to 120 seconds and share a ten-retry budget. This follows the selected
[upstream RTO change](https://github.com/FreeRDP/FreeRDP/commit/1a1e69b00f057e487e52a2dcf51212f03f861500),
the [retransmit timer rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp/78e889c9-26c3-4ffe-8c29-e9cb4ed34345),
and the documented [Windows exponential-backoff behavior](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp/cb93d5bf-25d1-4780-b58b-54c5579902d3).
Ten retries are the selected UDP2 sender policy; they are not the older
MS-RDPEUDP protocol's three-to-five-retry limit.

The transport sends keepalives after four seconds without outgoing traffic
and fails after sixteen seconds without a valid peer datagram. Malformed
input does not refresh liveness. A real incoming channel gap with later
bytes buffered has a separate sixteen-second progress deadline; an idle
desktop does not create that gap. These policies coordinate with
[UDP2 keepalive requirements](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/5f8a4b03-072d-48d4-95b5-d833de65e4f2).
Before receiving real DATA, keepalive uses a dummy AckOfAcks-only control
packet rather than inventing ACK(0) or consuming an outgoing DataSeq.

The former fixed ten-second chunk/admission deadlines are removed so they
do not defeat the larger adaptive timers. Cancellation remains prompt.
A peer that continues sending valid keepalives can leave an outgoing chunk
pending through the retry budget: about 393 seconds from the default floor,
or up to 22 minutes with an initially capped RTO. A silent peer still fails
within sixteen seconds.

ACK timestamps now describe packet arrival in the receiver's monotonic
clock, with 24-bit/4-microsecond wire encoding. ACK send gaps and scaled
delayed-packet arrival gaps are populated; no arrival is invented for a
sequence retired only by AckOfAcks. ACK vectors carry their latest genuine
arrival timing. See [ACK timestamp semantics](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/469cdb5f-5a53-4130-b3cd-36f02667092b)
and [ACK-vector fields](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/43183820-771d-4a00-89d6-58a3ecc80a78).

Receiver batching honors the peer's
[DelayAckInfo limits](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/eecd9c22-8d0b-4666-ae35-04b2f1ddea20).
Without a hint, the selected PR defaults are eight additional delayed ACKs
and half SRTT. The wire count is capped at fifteen. Zero limits request
immediate feedback. Gaps, duplicates and gap closure get prompt feedback;
a tighter hint shortens an existing deadline relative to its first pending
arrival. Piggybacking uses an immutable encoded snapshot. Main-loop timers,
blocking receives and full-queue waits service ACK deadlines even without
another incoming packet.

## Preserved behavior and excluded scope

Production retains its 32 KiB owned-send queue (with the existing atomic
oversize-write allowance only after preceding bytes drain), 31 active attempts by default,
bounded 63-packet storage, independent peer LogWindow 15 tracking, positive
ACK alias retirement, and ordered byte delivery. Three distinct higher
receipts plus a 25 ms reorder grace still enable fast recovery; fast and
timer recovery consume the same retry budget. Receiver channel-zero
inference and the tested nonblocking TLS-record drain remain intact.

This adaptation does not include the PR's larger receive backlog,
post-outage AckOfAcks resynchronization, outgoing channel-zero convention,
SYN retry expansion, Soft-Sync/DVC migration ordering, broker token fix or
full transport/congestion-control architecture. Queue tuning experiments
do not change the production default.

## Benchmark cancellation

`WITH_RDP_BENCH` adds a private campaign cancellation deadline so a capacity
wait can stop without changing production retry policy. The Python runner
arms it at planned drain end plus `NATIVE_DRAIN_CANCEL_GRACE_NS` (100 ms).
The grace lets an ordinary final five-millisecond read finish across the
boundary; it does not extend workload, probe, measurement or owner-loop
bounds. Endpoint results record the cancellation deadline and grace.

Deadline expiry retains partial delivery and queue state and marks an
incomplete SUT result. `benchmark_deadline_reached` distinguishes it from
a native transport failure even when cancellation sets `send_failed`.
An undrained measurement cohort receives a benchmark-drain telemetry note;
malformed artifacts and invalid observations remain harness failures.
This private API and its deadline check compile out of normal builds.

## Regression validation performed

The combined Mac Release build passed seven native targets in 3.73 seconds:
`TestRdpeUdp`, `TestRdpeUdpSend`, `TestRdpeUdpAckTiming`,
`TestRdpeUdpTlsDrain`, `TestRdpeUdpEvent`, `TestRdpeUdpQueueObserver` and
`TestClientCmdLine`. Loopback socket cases ran outside the filesystem/network
sandbox. Native coverage includes:

- SRTT smoothing, future-ACK rejection, Karn exclusion for both aliases,
  exact exponential deadlines, 120-second cap, ten retries, live-peer versus
  dead-peer handling, malformed input and cancellation during large backoff.
- Existing pipeline/window shrink/wrap/alias/ordered-delivery regressions,
  plus rapid fast-retry budget exhaustion.
- Five ACK timing cases covering batching, exact deadline dispatch,
  reordering/gap closure, dummy/ACK-only traffic, immutable callback snapshots,
  tighter hints, timestamp wrap, send-gap saturation, no-DATA keepalive and
  sparse main-loop wakeup with no socket event.
- Authenticated TLS draining, socket/manual wake ownership and queue
  accounting, with fixtures updated for the new timer and ACK wake semantics.
- Capability defaults, both enable spellings, disable and repeated-switch
  ordering.

The final Python regression gate passed **258 tests in 2.258 seconds**, including
actual native adapter checks. Focused validation includes five native-deadline tests (including
actual localhost read/send cancellation and cleanup), eleven failure-retention
tests, and eleven endpoint-evidence tests. They cover clock translation,
configuration errors, deadline-versus-transport classification, malformed
deadline scalars, sealed malformed artifacts, a final read crossing drain
end and cancellation of a stuck call without extending planned bounds.

## Native campaign results

The [tracked report](../results/pr13608-loss-20261013.md) and
[machine-readable evidence](../results/pr13608-loss-20261013.json) preserve all
twenty trials: five seeds per profile, the production 32 KiB queue, independent
loss in both directions and 10 Mbit/s each way. All artifacts audit intact;
nine trials have harness qualification failures. No accepted calibration
receipt is implied. No trial stopped because of the private benchmark deadline.

| Loss each direction | Full streams verified | Integrity-verified payload goodput Mbit/s (N) | Timing-qualified observed p95 / p99 ms (N) | Expected / injected / observed probes |
|---|---:|---:|---:|---:|
| 0.05% control | 5/5 | 3.540 (5) | 79.339 / 304.594 (5) | 3500 / 3454 / 3454 |
| 25% | 5/5 | 2.945 (4) | 744.438 / 1049.142 (4) | 3500 / 3473 / 3473 |
| 50% | 0/5 | Unavailable | Unavailable (0) | 3500 / 227 / 126 |
| 70% | 0/5 | Unavailable | Unavailable (0) | 3500 / 29 / 5 |

These are medians of per-trial statistics, not pooled latencies. Table rates
and latency exclude harness-invalid trials. The 25% cohorts exclude seed 11
because its native queue observer was invalid, although its complete payload
hashes matched. At 50%, neither timing-valid trial has measured probe replies;
the sole response cohort is timing-invalid. All five 70% trials have relay
timing failures. The linked raw report retains all diagnostic latency rows;
observed latency remains survivor-only and must be read beside missed probes.

Every trial still has a `FAIL` SUT outcome. Control and 25% completed all
payloads and observed every injected probe, but missed 46 and 27 scheduled
injections respectively. Accordingly the unchanged COMPLETE-qualified
goodput cohort is empty. The separate integrity-verified payload rate does
not promote these probe-service failures.

Relative to the [historical baseline](LOSS-STRESS-20261013.md), control payload
goodput fell from 4.059 to 3.540 Mbit/s (12.8%), and observed p95/p99 increased
from 67.1/96.6 to 79.3/304.6 ms. Baseline control served every probe. At 25%,
the baseline terminated all five transfers; this branch completed all five
streams, demonstrating improved sustained delivery in these runs while
probe service remained below the requested workload. These are historical
comparisons of a combined patch: identical seeds preserve packet-index loss
decisions, not identical logical drops when ACK traffic changes. They do not
isolate the effect of ACK batching or RTO policy.

At 50%, retained client logs show ten-retransmission exhaustion; receivers later
failed their sixteen-second channel-gap deadlines. Median first failure was
3.780 seconds, with three relay timing failures. Fast recovery necessarily
consumed part of the shared retry budget: timer-only recovery from the
300 ms floor cannot reach its tenth retransmission before 273.3 seconds.
Untraced main trials cannot reconstruct each timer/fast attempt or isolate
DATA loss from missing ACKs. At 70%, all transfers failed; median first
failure was 19.060 seconds, diagnostic only because every trial was
timing-invalid. Retained logs include retry-budget, gap and peer-silence failures.

Keep this branch available for review; these results do not establish a broad
performance win. The next investigation is the shared fast/timer retry budget
under severe loss and the control-profile latency/backpressure regression,
using trace and isolated ACK/RTO treatments. Resolve those mechanisms before
changing the production queue limit.

## Live Windows method

The separate [OpenThinRDP-method campaign](UPSTREAM-METHOD.md) used the Linux
X11 client at 1280×800 with progressive GFX, AVC420/AVC444 disabled and the
upstream Windows coloured-noise workload. Linux ingress netem impaired all
IPv4 arriving from the VM, without a rate cap: large packets additionally
received 1 ms delay and 70% reordering. An owned veth replaced IFB and the
upstream small/large ICMP verification was omitted; both deviations are recorded.

The first 25% step ended after **8.030 seconds**, exit code 147 and
`ERRCONNECT 0x0002000D`, with `UDP channel receive window overflow` in the
client log. This identifies collision in the **256-slot channel reassembly
map** (`RDPEUDP2_RECV_MAP`), not proof of a peer LogWindow or DataSeq ACK-window
overflow. The upstream stop-at-first-exit rule left 50% and 70% `NOT_RUN`.
All three loss levels were nevertheless exercised by the separate native suite.

Phase netem counters confirmed 25.218% completed-attempt drops for large
packets and 24.947% for small packets. The zero-loss preflight produced valid
values for all seven Windows PDH metrics and about 8.85 server output FPS;
its 1 ms/70% large-packet reorder treatment still applied. The 25% failure
preceded a full ten-second counter interval, so loss-phase FPS is unknown.
Server counters do not verify application goodput or physical client FPS.
See the [live report](../results/pr13608-upstream-method-20261013.md),
[JSON evidence](../results/pr13608-upstream-method-20261013.json), and
[consolidated results](PR13608-RESULTS-20261013.md).

The next live investigation is safe receive-map backlog/span handling with a
byte budget, alongside the native shared-retry-budget and control-tail work.
The branch remains unmerged.
