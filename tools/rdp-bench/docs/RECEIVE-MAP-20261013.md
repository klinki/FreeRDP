# Bounded UDP2 channel-backlog retention

Source commit `90b02e86b` replaces the fixed 256-slot channel reassembly map.
The previous map indexed `ChannelSeqNum % 256`, so buffered channels N and
N+256 collided behind a missing earlier channel. The client terminated even
though both payloads could legitimately be waiting for ordered delivery.
The first Windows receive-direction campaign reproduced this at 25% loss.

The [receiver reliability controller](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/a1d1f871-c282-4f97-8f51-f821aca00c8d)
requires later channel bytes to wait until the missing channel arrives.
AckOfAcks retirement updates datagram history; it does not authorize
discarding those required channel bytes. The separate
[sequence-number rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/ea4b6ac5-f574-4631-89a0-42a98ef78551)
describe the bounded active coded-packet range. A channel-delivery backlog
and the DataSeq ACK window are distinct structures.

## Storage and bounds

The new map addresses the complete 16-bit channel label using two levels:
the upper eight bits select one of 256 page pointers, and the lower eight
bits select one of 256 slots within that page. Pages allocate only when
needed. Channels N and N+256 therefore have different storage rather than
overwriting one another.

Production admits at most **32,768 buffered channels**, with a shared payload
budget of **32,768 × negotiated downstream MTU**. At MTU 1232 this is
40,370,176 bytes (38.5 MiB). Both unordered chunks and delivered bytes still
unread by TLS count against this budget. The budget describes owned payload;
page metadata and allocated stream capacity are separately bounded and
observable. Individual payloads larger than the downstream MTU are rejected.

Forward channel distances 0 through 32,767 are unambiguous. A label exactly
half the 16-bit space away is ignored without allocation or positive DATA
receipt; valid subsequent packets can continue. Chunk/byte budget exhaustion,
oversize payloads and allocation failure are terminal, preserving the
existing reconnect path rather than claiming receipt of unstored bytes.

Storage succeeds before DataSeq receipt is recorded or an ACK is scheduled.
Fresh DataSeq retries for an already retained channel reuse its original
bytes without another allocation or budget charge. Existing legitimate
channel-zero and Windows omitted-zero inference remain intact; larger
storage does not infer a missing channel from elapsed time.

## Reuse and cleanup

Empty pages are released, with at most one empty page cached for the current
expected channel. Ordinary in-order traffic can reuse that page through its
256 labels instead of repeatedly allocating a multi-KiB metadata page.
Transport teardown frees every remaining page and payload.

Before appending ordered bytes, the consumed prefix is compacted away.
Production reads release the unread-byte charge. Once the ordered stream is
fully consumed, its allocation shrinks back toward the 64 KiB idle reserve.
The existing sixteen-second actual-gap watchdog remains; an idle stream does
not create a gap. Sender queue size, active window, retry budget and ACK
timing policy are unchanged by this receive-storage fix.

An INFO teardown record for established socket transports reports peak
retained chunks/span/owned bytes, page allocations, remaining bytes and
effective limits. This supplies evidence of actual live retention beyond
256 slots without per-packet logging.

## Regression validation

Seven native targets and **265 Python tests passed**. Eight new receiver
cases in `TestRdpeUdp.c` cover:

1. Six hundred channels behind a lost first channel, modulo-256 aliases,
   AOA advancement, conflicting duplicate bytes, exact late-hole delivery
   and actual-reader cleanup.
2. Independent chunk/byte limits, duplicates at the limit, the valid far
   edge and unacknowledged half-space ambiguity.
3. Unread ordered bytes consuming the byte budget until the reader drains them.
4. Page/payload allocation failures before ACK, and duplicates succeeding
   with subsequent allocation disabled.
5. One thousand ordinary in-order packets with bounded page allocations
   and at most one cached empty page.
6. Partial-read compaction plus a 128 KiB ordered stream that verifies exact
   bytes and returns to the 64 KiB idle reserve after consumption.
7. Oversize input receiving neither allocation nor a positive ACK.
8. Lost channel `ffff`, DataSeq wrap through zero, Windows omission of
   channel zero and three hundred later channels preserved until recovery.

The existing true-zero wrap case also now retains channels 1 and 257,
then recovers a genuine missing zero and verifies all 258 bytes in order.
This directly exercises the old collision. Sender, ACK timing,
authenticated TLS drain, socket-event, queue-observer and command-line
regressions remain in the native gate.

## Live results

The repeat used source `90b02e86b` and the uncapped Windows receive-direction
method described in [UPSTREAM-METHOD.md](UPSTREAM-METHOD.md). Warmup had a
45-second minimum: observed workload warmup was about 144.3 seconds for the main ramp,
46.3 seconds at 50% and 48.1 seconds at 70%, including setup overhead. The original ramp stopped after
the 25% session exited; one fresh session each then exercised 50% and 70%.

| Loss | Observed session seconds | Server output FPS mean (valid intervals) | Peak retained chunks / span | Peak owned payload bytes | Missing channel / logged gap age |
|---|---:|---:|---:|---:|---|
| 25% main | 19.1126 | Unknown (0) | 3252 / 3262 | 3,676,913 | `6297` / 16.856 s |
| 50% fresh | 67.1836 | 5.3922 (3) | 2076 / 2077 | 2,382,964 | `874c` / 16.061 s |
| 70% fresh | 58.1829 | 3.1656 (3) | 2616 / 2619 | 3,018,135 | `0443` / 16.118 s |

All three sessions exited with code 147 after the sixteen-second channel-gap
watchdog fired. None reported the previous modulo-256 collision, a payload
budget breach, an allocation failure or an oversize-payload failure. The
reported peak payloads remained within the 40,370,176-byte limit. Logged gap
ages measure time without ordered channel progress; they are separate from
the full session duration and process-exit observation.

Observed netem completed-attempt drop fractions were:

| Configured loss | Small packets | Large packets |
|---|---:|---:|
| 25% | 24.6134% | 24.5096% |
| 50% | 50.6733% | 50.0326% |
| 70% | 69.6105% | 70.1063% |

These fractions exclude packets still queued at the snapshot; they are not
application-goodput measurements. The method applies loss to all IPv4
arriving from the VM, including TCP and UDP, with no rate cap. Large packets
also retain the 1 ms/70% reorder treatment. Loss is unseeded. Veth replaces
IFB and upstream ICMP verification is omitted; these deviations remain recorded.

Counter collection paired two `Get-Counter` samples. Its loop actually ran
about every twenty seconds rather than providing continuous ten-second
coverage; the three valid rate intervals per fresh run are ten-second PDH
windows. Only fully contained, valid intervals contribute to the FPS means.
At 25%, the candidate interval extended to about twenty seconds and its
provider disappeared after disconnection, leaving no fully contained valid
interval. FPS is therefore unknown; cross-boundary or preflight values are
not substituted. Server output FPS is neither physical client FPS nor
end-to-end latency, and this method does not verify application goodput.

The 25% session lasted longer than the previous 8.030-second collision run
and demonstrably retained thousands of chunks without aliasing. The fresh
50%/70% sessions are separate attempts, not a pooled before/after comparison
or successful completion of their requested 120-second intervals.

The remaining failure is an unresolved channel-delivery gap, a separate
mechanism from storage collision. The available evidence does not distinguish
lost DATA from missing ACKs, a retired transmission or an incorrect receipt.
The next step is tracing that channel across receiver storage, ACK generation
and Windows retransmissions before changing recovery or watchdog policy.

[Machine-readable combined evidence](../results/receive-map-20261013.json)
retains the session identities, network receipts, counter intervals and failures.
