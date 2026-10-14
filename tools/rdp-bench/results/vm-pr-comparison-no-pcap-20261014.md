# Windows VM comparison

One trial per condition; server counters and sampled content progress. Verified goodput and latency tails remain unavailable.

| Client | Loss | Measured s | Outcome | UDP evidence | Server FPS mean (N) | Network-skipped FPS mean (N) | Visual evidence |
|---|---:|---:|---|---|---:|---:|---|
| local | 0% | 120.004 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 8.111 (11) | 2.544 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 0% | 120.002 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 12.095 (11) | 3.002 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 25% | 120.002 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 11.440 (11) | 6.839 (11) | COMPLETE_SAMPLED_PROGRESS |
| local | 25% | 120.007 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 6.645 (11) | 2.676 (11) | COMPLETE_SAMPLED_PROGRESS |
| local | 50% | 120.004 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 5.552 (11) | 0.136 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 50% | 120.001 | COMPLETE | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 8.032 (11) | 8.577 (11) | COMPLETE_SAMPLED_PROGRESS |
| pr | 70% | 65.215 | SESSION_ENDED | MEASURED_UDP_FAILURE_LOGGED | 2.693 (6) | 9.527 (6) | PARTIAL_PROGRESS_BEFORE_LOGGED_TERMINATION |
| local | 70% | 66.206 | SESSION_ENDED | READY_NO_MEASURED_UDP_FAILURE_RECORDED | 3.668 (5) | 11.130 (5) | PARTIAL_PROGRESS_BEFORE_LOGGED_TERMINATION |

| Client | Loss | Small completed-drop fraction | Large completed-drop fraction | Counter error events |
|---|---:|---:|---:|---:|
| local | 0% | 0.000 | 0.000 | 0 |
| pr | 0% | 0.000 | 0.000 | 0 |
| pr | 25% | 0.251 | 0.250 | 0 |
| local | 25% | 0.250 | 0.250 | 0 |
| local | 50% | 0.504 | 0.500 | 0 |
| pr | 50% | 0.500 | 0.500 | 0 |
| pr | 70% | 0.698 | 0.699 | 0 |
| local | 70% | 0.703 | 0.701 | 0 |

Use these matched trials to assess Windows-downlink RDP survival and sampled graphics progress. Do not transfer an FPS advantage to CRC goodput or latency-tail claims. Any apparent winner from one unseeded connection per condition warrants repeated matched trials before changing production policy.

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

Packet capture was disabled; no capture kernel-drop metric is available. PDH collector-end receipt absent in 8 trials.

Provider error events: 1 in the whole raw collection, including 1 after measurement; 0 inside nominal measured intervals.

All planned conditions, counter/observer errors, clock qualifications and raw references are retained in JSON; CSV blanks represent unknown values.
