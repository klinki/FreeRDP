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

Production retains its 32 KiB owned-send queue, 31 active attempts by default,
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

Focused Python validation also passed five native-deadline tests (including
actual localhost read/send cancellation and cleanup), eleven failure-retention
tests, and eleven endpoint-evidence tests. They cover clock translation,
configuration errors, deadline-versus-transport classification, malformed
deadline scalars, sealed malformed artifacts, a final read crossing drain
end and cancellation of a stuck call without extending planned bounds.

## Campaign results

Pending the 25%, 50% and 70% loss rerun and the separate
[OpenThinRDP benchmark method](UPSTREAM-METHOD.md). Record those outcomes
with their actual method fidelity, calibration, verified goodput,
survivor-only latency and all failures before drawing performance conclusions.
