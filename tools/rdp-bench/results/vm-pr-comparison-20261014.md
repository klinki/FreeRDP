# Windows VM comparison

One trial per condition; server counters and sampled content progress. Verified goodput and latency tails remain unavailable.

| Client | Loss | Measured s | Outcome | UDP evidence | Server FPS mean (N) | Network-skipped FPS mean (N) | Visual evidence |
|---|---:|---:|---|---|---:|---:|---|
| local | 0% | 120.002 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 8.077 (11) | 0 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 0% | 120.003 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 14.905 (11) | 3.701 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 25% | 120.001 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 10.395 (11) | 8.970 (11) | COMPLETE_SAMPLED_PROGRESS |
| local | 25% | 120.000 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 7.942 (11) | 4.309 (11) | COMPLETE_SAMPLED_PROGRESS |
| local | 50% | 120.001 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 7.317 (11) | 6.446 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 50% | 120.000 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 7.695 (11) | 9.565 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 70% | 66.075 | SESSION_ENDED | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 4.344 (6) | 15.578 (6) | PARTIAL_PROGRESS_BEFORE_LOGGED_TERMINATION |
| local | 70% | 120.001 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 4.593 (11) | 14.332 (11) | COMPLETE_SAMPLED_PROGRESS |

| Client | Loss | Small completed-drop fraction | Large completed-drop fraction | Counter error events |
|---|---:|---:|---:|---:|
| local | 0% | 0.000 | 0.000 | 0 |
| pr | 0% | 0.000 | 0.000 | 0 |
| pr | 25% | 0.255 | 0.251 | 0 |
| local | 25% | 0.251 | 0.250 | 0 |
| local | 50% | 0.504 | 0.501 | 0 |
| pr | 50% | 0.501 | 0.499 | 0 |
| pr | 70% | 0.698 | 0.699 | 0 |
| local | 70% | 0.704 | 0.701 | 0 |

Capture tasks remained active across later trials, so changing capture overhead confounds these performance values. Preserve this entire first campaign as exploratory evidence, including the PR70 main-transport timeout. Use the complete separately labelled no-capture campaign for the client comparison; do not pool the campaigns or selectively replace individual trials.

- One fresh transport connection per client/loss condition; results are exploratory, with no statistical significance claim.
- Windows can reconnect the same user desktop; fresh transport connection does not imply a new Windows login session.
- Loss affects all IPv4 arriving from Windows only, including TCP and UDP; the return direction is clean.
- No 10 Mbit/s rate cap; large IP packets >=512 bytes additionally receive 1 ms delay and 70% reordering; netem limit 100,000 packets.
- Zero percent is a random-loss control retaining large-packet delay/reordering, not an unshaped control.
- Veth replaces unavailable IFB; original small/large ICMP verification is a recorded method deviation.
- Kernel loss/reordering and the upstream coloured-noise workload are unseeded; parameters repeat, exact packet histories do not.
- Fresh independent conditions replace the original sequential ramp; all attempts are retained without reruns.
- Server output FPS is frames made available by Windows, not physical client presentation FPS.
- RemoteFX RTT is a sampled server counter, not a per-response transport probe or input/display latency distribution.
- A cooked server RTT value of zero is retained as reported; it does not establish a zero-millisecond transport RTT.
- Server bandwidth/sent-rate counters and encrypted packet counts do not establish CRC-verified application goodput.
- Sampled content progress is separate from observer health, client survival and UDP tunnel evidence; it is not an automatic native pass.
- Process polling bounds termination detection; visual samples after a logged terminal error are retained separately from the pre-error content prefix.
- Raw capture lifecycle errors and missing PDH collector-end receipts are preserved; contained valid PDH intervals do not establish complete capture or collector termination.
- Metric summaries are within each trial only; no pooling across different conditions or survivor-only aggregate.
- The two full clients differ in their FreeRDP base versions and other changes; this is not an isolated UDP-patch effect.
- The published Dell/Debian/Hyper-V environment differs from this Mac/Docker/Windows ARM VM; absolute FPS is environment-dependent.

Capture health: 8 invalid, 0 unknown trials. Recorded kernel drops cover whole overlapping capture lifetimes, not measured phases. PDH collector-end receipt absent in 8 trials.

All planned conditions, counter/observer errors, clock qualifications and raw references are retained in JSON; CSV blanks represent unknown values.
