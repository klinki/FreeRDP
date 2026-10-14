# Protocol-separated Windows VM loss recovery — 2026-10-14

The PR's premature UDP v3 disconnect is fixed in native commit
`49fcbb72c0b6fbfdb00822f98a9ff3697af6a1ab`, on
`codex/pr13608-protocol-loss-recovery`. The post-fix UDP-only 50% and 70% runs
completed their requested 120 seconds with changes in every one of 119 central
pixel comparisons. Severe combined loss still caused TCP failures, including a
39-second sampled freeze in the PR client. This fixes the invalid UDP deadline;
it does not establish that all high-loss stalls have been eliminated.

## Campaign results

All 40 new production conditions were measured: 16 baseline, 16 post-fix and
eight post-fix combined-loss conditions. There were 34 completed conditions and
six session failures. Three additional old-code trace/capture runs are diagnostic
only. An authentication-prompt handoff was cancelled before any network or client
started; its zero-measurement startup records are preserved separately.

| New phase | Conditions | Completed 120 s | Session failures |
|---|---:|---:|---:|
| TCP-only / UDP-only loss baseline | 16 | 13 | 3 |
| TCP-only / UDP-only loss post-fix | 16 | 15 | 1 |
| Combined TCP and UDP loss post-fix | 8 | 6 | 2 |

The PR UDP-only comparison is below. FPS is **Windows server output FPS**, not
physical client presentation FPS. `N` counts admitted ten-second counter
intervals within one connection; it is not a count of independent trials.
Baseline failure means cover shorter prefixes and cannot be compared as
full-duration throughput estimates.

| UDP loss | Baseline PR FPS (N) | Fixed PR FPS (N) | Baseline outcome | Fixed outcome |
|---|---:|---:|---|---|
| 0% | 11.462 (11) | 14.796 (11) | Complete | Complete |
| 25% | 14.903 (11) | 10.315 (11) | Complete | Complete |
| 50% | 8.813 (11) | 9.294 (11) | UDP watchdog, detected at 117.455 s | Complete |
| 70% | 3.727 (3) | 4.030 (11) | UDP watchdog, detected at 43.229 s | Complete |

Both failed baseline UDP runs had approximately 15-second unchanged sampled
spans before their watchdog exits. Fixed PR UDP50/70 each had 119/119 changed
comparisons and no unchanged sampled span. This is sampled progress at 1 Hz;
it does not prove continuous rendering between samples.

Every new session failure remains in the dataset:

| Phase | Impaired protocol | Client | Loss | First logged fatal (s) | Process-poll detection (s) | Cause |
|---|---|---|---:|---:|---:|---|
| Baseline | TCP | PR | 70% | 72.977 | 73.155 | Main TCP timeout |
| Baseline | UDP | PR | 50% | 117.290 | 117.455 | UDP ordered-gap watchdog |
| Baseline | UDP | PR | 70% | 42.477 | 43.229 | UDP ordered-gap watchdog |
| Fixed | TCP | Local | 70% | 53.685 | 54.110 | Main TCP timeout |
| Fixed combined | TCP + UDP | PR | 70% | 119.784 | 120.005 | Main TCP timeout |
| Fixed combined | TCP + UDP | Local | 70% | 40.633 | 41.156 | Main TCP timeout |

The PR combined-loss fatal was logged before 120 seconds on the same Linux
clock as the phase start. The later process poll is preserved, but this run is
**not** counted as 120-second survival. Its server FPS was 3.349 (N=11), with
80/119 changed comparisons and a longest/trailing unchanged sampled span of
39.008 seconds. The local combined-loss failure had 4.292 FPS (N=3), with
40/40 changed comparisons before its TCP failure. Those partial rates cover
different durations.

The unchanged local binary also varied substantially between phases: it
survived baseline TCP70 and failed post-fix TCP70. The PR completed post-fix
TCP70 after failing it in the baseline. The native change affects UDP, so these
TCP outcomes are independent observations rather than evidence of a TCP fix.

[Full 40-condition / 24-pair comparison](../results/vm-protocol-loss-before-after-20261014.md),
[machine-readable JSON](../results/vm-protocol-loss-before-after-20261014.json),
[40-row CSV](../results/vm-protocol-loss-before-after-20261014.csv),
[24-pair CSV](../results/vm-protocol-loss-before-after-20261014-pairs.csv).
The eight historical combined-loss pairs use the earlier no-capture campaign
and its older collector/cleanup harness; they remain separately labelled.

## Cause and native change

The old v3 timer failed any buffered ordered gap after 16 seconds without
delivery, even when the peer remained active. The warning that the server
“no longer resends” was an inference, not a measured fact. The new deterministic
regression reproduced failure at a 16,027 ms gap with only 706 ms of peer silence,
before a scheduled valid repair at 17.5 seconds.

The fix applies the existing ordered-progress watchdog only to v1/v2. V3 waits
for repair while peer traffic continues. The existing 16-second v3 peer-silence
deadline, keepalives, own retransmission limits/RTO and bounded receive range
remain unchanged. A genuinely unrepaired channel can consequently remain
stalled while the peer is active; receiver storage remains bounded by the
32,768-slot map and 32,767 forward reach.

The GREEN live-peer regression retains `b`, receives late `a`, delivers `ab`
in order and checks the emitted ACK. Separate cases verify failure after
16.5 seconds of actual peer silence and negative feedback for an out-of-range
channel, with later valid repair still releasing retained data. All 19 native
UDP cases passed in the separate Linux CI build. The old policy's new live-peer
case failed as expected. Legacy watchdog preservation follows from the version
guard; no new legacy delayed-gap runtime case was added.

This follows the v3 receiver's distinction between
[retained channel data](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/a1d1f871-c282-4f97-8f51-f821aca00c8d)
and [peer-silence timeout](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/5f8a4b03-072d-48d4-95b5-d833de65e4f2).
ACK/Ack-of-Acks policy was not changed. An AoA-retired packet attempt is not
proof that its logical channel chunk was delivered: a repair can use a fresh
DataSeq while retaining the same ChannelSeq.

## ACK and retransmission tracing

Optional `RDPEUDP_RECOVERY_TRACE` tracing records bounded header/state events
for receipt, channel retention/delivery, actual ACK send results, AoA retirement
and client retry aliases. It excludes payload and TLS material, defaults off,
and dumps exclusively at failure/teardown. Prefix overwrite and clock/sequence
coverage are explicit. Default-off execution still has null checks and argument
extraction; it is not claimed to have zero overhead.

The three traced old-code PR70 attempts all survived. They therefore do not
provide a captured wire trace of the failed production connection. The native
RED/GREEN regression proves the invalid deadline; the captures investigate
real Windows repairs and acknowledgement decisions without inventing a missing
failure trace.

| Old-code diagnostic | Native longest recovered gap | Wire/profile qualification |
|---|---:|---|
| UDP70, initial capture | 12.059 s | Repair is 7.616 s after the received capture ends and after zero-loss reset; not captured recovery under 70% loss |
| Combined70 | 0.820 s | 20 visible aliases; final repair captured and accepted inside the profile |
| UDP70, buffered captures through teardown | 10.848 s | 19 visible aliases; final repair captured and accepted inside the profile; receiver resync restricts ACK/absence qualification |

The buffered run's receiver resync rebases DataSeq 7651 to 7662 just before the
repair. The conservative analyzer marks aliases crossing that resync ambiguous:
18 missing post-loss observations are **not** labelled proven packet drops.
Historical AoA-retired positive references are separated from active receipt
claims. No qualified active false-positive ACK was established; that is a scoped
finding, not proof that every acknowledgement across resync was audited.

All six captures have zero kernel drops and remain below their count bounds;
their 256-byte snapshots intentionally truncate encrypted bodies. Native traces
have complete-from-arm sequence coverage and zero overwrites. Trace and capture
observers can affect timing, so none of these runs supplies performance values.
Native retry events describe client outbound packets; Windows repair aliases
come from offered/received packet headers and native reception.

## Method, validation and limits

Clients are production Release Linux ARM64 X11 builds in a four-CPU / 4 GiB
Debian container on this Mac, against the existing Windows 11 ARM VM. The local
control is `79ee244a4d5b2d5a280e165be4c643478484bf9e`; the PR baseline is pinned
to `a306a67c198f4493d8d2000bc10efd75b2432020`, rather than a moving PR head.
The fixed native source is bound to commit `49fcbb72c` by c/h/test hashes and
binary/library hashes. Complete clients differ in base version and transport
policies; this is not an isolated engine ranking. The primary checkout remains
unchanged.

The workload is pinned to OpenThinRDP
`142434adf715c3f3352c0e6dd7b7ece90244aeec`: 240×135 coloured noise scaled to
1280×800, a nominal 33 ms timer, progressive graphics with AVC disabled,
45 seconds of warmup and 120 seconds of measurement per fresh connection.
An existing Windows login/desktop can be reused. Server counters use contained,
valid ten-second intervals with session corroboration and clock bounds; image
sampling uses a central 384×240 region at 1 Hz and excludes terminal teardown
frames.

“TCP-only” and “UDP-only” mean **selected-protocol loss**, with both transports
enabled. Loss affects IPv4 arriving from Windows; return traffic is clean.
Selected packets of at least 512 bytes additionally receive 1 ms delay and 70%
reordering, including the 0% loss control. Smaller selected packets receive loss
alone; the other protocol has an explicit clean bypass. There is no rate cap,
including no 10 Mbit/s cap. The owned veth substitutes for IFB, and the original
ICMP preflight is omitted. Loss/reordering and noise are unseeded: parameters are
repeatable, packet histories are not matched.

All 32 selected-protocol production conditions have known zero bypass drops.
Realized large/small loss and sample counts are retained separately. Some TCP
large bands have no offered packets, so their loss fraction is unknown rather
than zero. The combined profile has no nonselected band, so bypass values are
not applicable. Actual AF_PACKET fixtures verify all/TCP/UDP classification at
100% loss, and regression fixtures preserve duplicate tc JSON keys.

Seven raw provider errors remain: six after the admitted measurement window and one
boundary-uncertain. None is admitted into measurement metrics. All 40 production
collectors have final footers. One fixed PR TCP25 footer-observation wait timed
out after 15.041 seconds; the later requested-stop footer and exact guest cleanup
were verified separately. Original process-bound reports remain alongside the
stricter first-fatal-bound reports. No thrown execution or cleanup error occurred.

Native regressions, protocol-selection/counter fixtures, report-boundary and
comparison fixtures, and header/compact analyzer fixtures passed. Their commands,
counts, hashes and logs are recorded in `validation-final.json` in the dated lab.
An independent audit recomputed all 40 visual summaries and verified source
identity, 24 distinct descriptive pairs, null metric fields and cleanup evidence.

CRC integrity, verified application goodput, physical client FPS and transport
or input-to-display p95/p99 are unknown in this method and remain JSON null / CSV
blank. One unseeded connection per condition, large variation in unchanged local
controls, observer effects and differing failure-prefix durations prevent a
statistical speedup claim. These VM figures also do not reproduce the PR's
[published FPS claim](https://github.com/FreeRDP/FreeRDP/pull/13608#issuecomment-6131161999),
which used a different revision and environment.

Owned Windows tasks/processes were verified absent. The owned Linux network was
restored, its container removed, the SSH control socket closed and the bounded
power assertion stopped. Guest evidence files and local full captures are
retained; persistent power settings were unchanged. The benchmark ZIP preserves
every earlier member and adds this lab, raw receipts/traces, compact analyses and
production runtimes. Full encrypted captures remain local with hashed omissions
in the package manifest.
