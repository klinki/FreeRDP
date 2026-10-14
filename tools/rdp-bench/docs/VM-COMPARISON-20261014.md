# Windows VM comparison — 2026-10-14

The complete no-capture repeat is the main comparison. Both production clients
completed the 0%, 25% and 50% loss conditions with sampled desktop progress. At
70%, the PR client stopped after an ordered UDP stream stall; the local client
stopped after a main TCP transport timeout. These are exploratory observations,
not an isolated UDP-engine comparison or a general performance ranking.

## Clean repeat

There was one fresh transport association per client/loss condition, a minimum
45-second workload warmup and a requested 120-second measurement. `N` counts
contained, clock-qualified 10-second server counter intervals, not independent
trials. FPS is Windows RemoteFX **server output FPS**, not client presentation
FPS. The two 70% means cover different partial intervals and are not directly
comparable full-duration performance estimates.

| Downlink loss | Local mean server FPS (N) | PR mean server FPS (N) | Local survival / first failure | PR survival / first failure |
|---|---:|---:|---|---|
| 0% | 8.111 (11) | 12.095 (11) | Complete, 120.004 s | Complete, 120.002 s |
| 25% | 6.645 (11) | 11.440 (11) | Complete, 120.007 s | Complete, 120.002 s |
| 50% | 5.552 (11) | 8.032 (11) | Complete, 120.004 s | Complete, 120.001 s |
| 70% | 3.668 (5, partial) | 2.693 (6, partial) | TCP timeout; detected at 66.206 s | UDP ordered-stream stall; detected at 65.215 s |

The six completed trials had complete 1 Hz central-content sampling. Both 70%
observers returned valid partial evidence without capture errors; neither
completed its requested sampling window. Their final all-black teardown frames
are retained and excluded from pre-terminal content progress:

| Clean 70% trial | Pre-terminal samples / comparisons | Substantial changes | Evidence near termination |
|---|---:|---:|---|
| Local | 66 / 65 | 65 | Last noise change at 65.002 s; TCP error followed about 173 ms later |
| PR | 65 / 64 | 46 | Last change at 49.004 s; samples 50–64 were identical, spanning about 15.001 s |

The raw observer summaries respectively contain 67/66/66 and 66/65/47
samples/comparisons/changes because each includes one transition to black.
Pixel changes establish sampled central-content progress only: they do not
measure physical FPS, input latency, transport tails or plaintext integrity.

[Clean report](../results/vm-pr-comparison-no-pcap-20261014.md) ·
[JSON with raw references and qualifications](../results/vm-pr-comparison-no-pcap-20261014.json) ·
[CSV](../results/vm-pr-comparison-no-pcap-20261014.csv).

## Failure evidence

The PR's first terminal warning was at **08:27:22.440 UTC**:
`rdpudp_fail` reported a stuck stream, followed by `emt_fail` and the
multitransport session-ending event. In frozen `rdpudp.c`, the guard checks
`buffered > 0` and more than 16 seconds since the later of `bufferingSince` and
`lastDelivery`. The sampled frozen image supports an ordered-progress stall.
The warning's wording does **not** establish that Windows stopped resending,
which channel was missing, whether retransmissions were lost, or whether an
acknowledgement or sequence decision was incorrect. No packet capture was
running in the clean repeat.

The local client's first terminal error was at **08:29:39.117 UTC**:
main-transport `BIO_read` returned errno 110, `Connection timed out`, then
`ERRCONNECT_CONNECT_TRANSPORT_FAILED`. It had continued changing the sampled
noise region immediately before that error. UDP teardown reported
`recv_failed=0`, with 210 retained chunks / 240,909 bytes and no unread ordered
stream bytes. That retained backlog does not establish a UDP watchdog failure.
Subsequent window, event and terminal-IO warnings are cleanup, not the initial
failure. Both unplanned client exits were 147. Durations above are bounded by
the runner's process polling, rather than exact logged failure instants.

Both frozen clients enable TCP keepalive with delay 5 s, interval 2 s and three
retries, and configure `TcpAckTimeout=9000` through Linux `TCP_USER_TIMEOUT`.
These policies are relevant because this method loses TCP as well as UDP, but
the logs do not identify the particular TCP timer or missing acknowledgement
that caused the timeout. Increasing those limits would change the experiment;
these results alone do not justify changing production defaults.

Raw evidence: [PR70 client log](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/campaign-no-pcap/pr-loss70-f4aeb006/client.log),
[PR70 image samples](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/campaign-no-pcap/pr-loss70-f4aeb006/image-progress.jsonl),
[local70 client log](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/campaign-no-pcap/local-loss70-8b255cd7/client.log),
[local70 image samples](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/campaign-no-pcap/local-loss70-8b255cd7/image-progress.jsonl).

## Sources, builds and method

- Local production source: `79ee244a4d5b2d5a280e165be4c643478484bf9e`, FreeRDP
  `3.31.2-dev0`, with `WITH_RDP_BENCH=OFF`.
- PR production source: `a306a67c198f4493d8d2000bc10efd75b2432020`, FreeRDP
  `3.32.3-dev0`, with `WITH_RDPEUDP=ON`; no private benchmark ABI.
- Method scripts pinned to OpenThinRDP
  `142434adf715c3f3352c0e6dd7b7ece90244aeec`:
  [README](https://github.com/OpenThinRdp/openthinrdp/blob/142434adf715c3f3352c0e6dd7b7ece90244aeec/tests/udp-bench/README.md),
  [line shaping](https://github.com/OpenThinRdp/openthinrdp/blob/142434adf715c3f3352c0e6dd7b7ece90244aeec/tests/udp-bench/line.sh),
  [workload](https://github.com/OpenThinRdp/openthinrdp/blob/142434adf715c3f3352c0e6dd7b7ece90244aeec/tests/udp-bench/load.ps1).

The Linux clients share Release X11 build settings, four container CPUs / 4 GiB,
Xvfb 1280×800×24, RDP 32 bpp and progressive graphics with AVC disabled. The
server is Windows 11 Pro `10.0.26200` in the Windows ARM VM on this Mac. Exact
source trees, binary/library hashes, package versions and runtime linkage are
retained in [build receipt](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/linux-setup.json),
[runtime receipt](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/linux-runtime-linkage.json),
[VM receipt](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/vm-preflight-identity.json) and [method receipt](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/upstream-source.json).

Loss applies to **all IPv4 arriving from Windows**, including TCP and UDP; the
return direction is clean. There is **no 10 Mbit/s cap**. IP packets at least
512 bytes retain 1 ms delay and 70% reordering, including the 0% random-loss
control. Netem's limit is 100,000 packets and MTU is 1500. An owned veth replaces
IFB; the original ICMP preflight is omitted. Fresh independent conditions
replace the published sequential ramp. Kernel loss/reordering and coloured
noise are unseeded: configuration is repeatable, packet history is not.

Source defaults differ: local advertises reliable UDP FECR (`0x1`), while the
PR client additionally advertises `SOFTSYNC_TCP_TO_UDP` (`0x200`, combined
`0x201`, subject to server negotiation). Local uses core migration gates;
post-ACTIVE outbound DVCs can stay on TCP while incoming UDP DVCs enable the
receive path. PR uses Soft-Sync and per-channel tunnel associations learned
from incoming CREATE/DATA as well. Both graphics plugins send frame
acknowledgements through their DVC `Write` method, so actual negotiated routing
and readiness timing matter. The source proves different policies, not which
policy caused these FPS values. The complete clients also differ in base
version and other changes; no native flags were forced to erase those
production differences.

## First campaign and evidence limitations

The earlier [capture campaign](../results/vm-pr-comparison-20261014.md) is
preserved separately. Capture shutdown guards rejected `/proc/<pid>/exe` after
tcpdump dropped privileges; capture tasks continued across later trials.
Changing, overlapping capture overhead confounds that campaign's performance.
Its PR70 ended at 66.075 s on a **TCP** timeout, whereas local70 completed
120.001 s. These observations remain useful failure evidence but are not
pooled with the clean repeat or selectively substituted into its matrix.
Capture completeness/drop receipts and recorded drops across overlapping
capture lifetimes do not establish per-phase capture health.

The measured coordinator was preserved before a **future-only** guard repair;
the repair was applied after both timed campaigns. It validates exact process
ownership, birth time and arguments and uses a pidfd, accommodating tcpdump's
UID drop. [Measured source receipt](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/measured-coordinator-receipt.json) and
[future source receipt](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/future-capture-guard-sha256.json) distinguish the two.

PDH collector-end receipts are absent in all eight trials of each campaign.
Contained valid intervals can support the table, but do not prove complete
collector termination. The clean collection has one retained provider error
after local70's terminal event and after measurement, with zero provider error
events inside nominal measured intervals. Cross-boundary or invalid intervals
are excluded; absent values remain unknown. The local70 partial FPS mean has
five valid intervals, not six or a 120-second average.

Verified application goodput, CRC integrity, transport RTT p95/p99 and
input/display p95/p99 remain **unknown**. Server UDP sent-rate/bandwidth and
RTT counters are separate diagnostics; a reported zero RTT does not establish
a zero-millisecond link.

The [published PR test report](https://github.com/FreeRDP/FreeRDP/pull/13608#issuecomment-6131161999)
reported 19–21 server FPS at 50% loss and 18–19 at 70%, on revision
`d148cdb76`. Its Dell/Debian/Hyper-V environment and revision differ from this
Mac/Docker/Windows ARM setup and PR `a306a67`. These single unseeded trials
neither verify nor refute those absolute reported ranges.

## Validation, cleanup and next work

The private helpers passed **24 offline fixture tests**: 10 process-guard,
eight exporter and six image-observer tests. One actual approximately
one-second local-loopback capture smoke exercised tcpdump's UID drop and
pidfd shutdown, with one captured fixture packet, zero kernel drops and no
remaining capture. Its original automated result remains false because a
plural-only footer matcher missed `1 packet captured`; the retained
[offline validation](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/capture-guard-smoke-8842d07c/offline-validation.json)
establishes the observed pass without rerunning or rewriting that receipt.
This smoke did not use the VM or RDP workload.

Owned network state was restored, the runner container removed, and owned
SSH and power resources closed. All 16 per-trial guest cleanup receipts are
retained in [cleanup summary](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/windows-cleanup-summary.json), alongside
[network](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/network-cleanup.json), [container](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/container-cleanup.json),
[SSH](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/ssh-cleanup.json) and [power](/Users/david/projects/FreeRDP/build/vm-pr-comparison-20261014/power-cleanup.json) receipts.

Next, separate TCP liveness from UDP recovery with bounded TCP diagnostics and
an explicitly labelled UDP-only-loss control. For the PR ordered-stream
stall, trace the missing channel, DATA retransmission aliases and ACK/AckOfAcks
decisions before changing retry or watchdog defaults. Establish actual
graphics/acknowledgement routing and repeat matched conditions before assigning
an FPS advantage to the UDP engine.
