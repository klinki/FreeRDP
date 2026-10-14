# VM protocol-loss before/after comparison

Each row compares one independent connection before and after. Trials are not pooled. Failed-prefix FPS is retained with its shorter duration.

| Scope | Client | Loss | Before outcome / seconds | After outcome / seconds | Before → after server FPS mean (N) |
|---|---|---:|---|---|---:|
| tcp | local | 0% | COMPLETE / 120.004 | COMPLETE / 120.001 | 3.339 (11) → 4.643 (11) |
| tcp | local | 25% | COMPLETE / 120.003 | COMPLETE / 120.001 | 7.588 (11) → 4.211 (11) |
| tcp | local | 50% | COMPLETE / 120.001 | COMPLETE / 120.001 | 7.578 (11) → 7.254 (11) |
| tcp | local | 70% | COMPLETE / 120.003 | SESSION_ENDED / 54.110 | 7.378 (11) → 6.857 (5) |
| tcp | pr | 0% | COMPLETE / 120.000 | COMPLETE / 120.001 | 13.469 (11) → 5.821 (10) |
| tcp | pr | 25% | COMPLETE / 120.003 | COMPLETE / 120.004 | 11.871 (11) → 5.524 (10) |
| tcp | pr | 50% | COMPLETE / 120.005 | COMPLETE / 120.003 | 12.520 (11) → 14.569 (11) |
| tcp | pr | 70% | SESSION_ENDED / 73.155 | COMPLETE / 120.005 | 10.563 (7) → 4.177 (11) |
| udp | local | 0% | COMPLETE / 120.003 | COMPLETE / 120.002 | 7.536 (11) → 4.848 (11) |
| udp | local | 25% | COMPLETE / 120.005 | COMPLETE / 120.001 | 6.844 (11) → 6.908 (11) |
| udp | local | 50% | COMPLETE / 120.003 | COMPLETE / 120.001 | 5.593 (11) → 9.052 (11) |
| udp | local | 70% | COMPLETE / 120.002 | COMPLETE / 120.001 | 4.838 (11) → 4.549 (11) |
| udp | pr | 0% | COMPLETE / 120.004 | COMPLETE / 120.001 | 11.462 (11) → 14.796 (11) |
| udp | pr | 25% | COMPLETE / 120.002 | COMPLETE / 120.004 | 14.903 (11) → 10.315 (11) |
| udp | pr | 50% | SESSION_ENDED / 117.455 | COMPLETE / 120.006 | 8.813 (11) → 9.294 (11) |
| udp | pr | 70% | SESSION_ENDED / 43.229 | COMPLETE / 120.004 | 3.727 (3) → 4.030 (11) |
| all (historical) | local | 0% | COMPLETE / 120.004 | COMPLETE / 120.004 | 8.111 (11) → 4.335 (11) |
| all (historical) | local | 25% | COMPLETE / 120.007 | COMPLETE / 120.001 | 6.645 (11) → 7.365 (11) |
| all (historical) | local | 50% | COMPLETE / 120.004 | COMPLETE / 120.002 | 5.552 (11) → 5.549 (11) |
| all (historical) | local | 70% | SESSION_ENDED / 66.206 | SESSION_ENDED / 41.156 | 3.668 (5) → 4.292 (3) |
| all (historical) | pr | 0% | COMPLETE / 120.002 | COMPLETE / 120.001 | 12.095 (11) → 14.549 (11) |
| all (historical) | pr | 25% | COMPLETE / 120.002 | COMPLETE / 120.003 | 11.440 (11) → 14.228 (11) |
| all (historical) | pr | 50% | COMPLETE / 120.001 | COMPLETE / 120.001 | 8.032 (11) → 11.223 (11) |
| all (historical) | pr | 70% | SESSION_ENDED / 65.215 | SESSION_ENDED / 120.005 | 2.693 (6) → 3.349 (11) |

Windows output FPS means frames made available by the server. Physical client FPS, CRC-verified goodput and latency p95/p99 are unknown.

Poll duration is retained separately from logged fatal time. The PR all-IPv470 timeout was logged before120s even though process polling crossed120s; it is not admitted as120s survival.

TCP-only/UDP-only means impairment selects that ingress protocol; both RDP transports remain enabled. Loss applies only to VM ingress; the return path is clean. No bandwidth cap or loss seed is used. Large selected packets retain 1ms delay and70% reordering even in the0% loss control. Actual large/small drops and nonselected bypass drops are separate in JSON.

The baseline and fixed protocol matrices retain identical production local binaries. PR code/source identity changes are explicit. The historical all-IPv4 comparison uses the older counter/cleanup harness; it is a separate exploratory comparison.

Provider errors before/during/after the admitted measurement window, partial visual prefixes, UDP/main-TCP termination, clocks, source hashes and cleanup remain in JSON. Diagnostic trace/capture runs never supply production comparison values.

One fixed TCP25 PR collector stop exceeded its15s wait and requested guarded fallback cleanup. Its final requested-stop footer is retained; footer presence does not claim every bounded stop wait succeeded.

| Phase | Scope | Client | Loss | Changed sampled intervals | Longest unchanged sampled span (s) | Visual attention |
|---|---|---|---:|---:|---:|---|
| baseline | tcp | local | 0% | 119 / 119 | 0.000 | False |
| baseline | tcp | pr | 0% | 118 / 119 | 0.313 | False |
| baseline | tcp | pr | 25% | 119 / 119 | 0.000 | False |
| baseline | tcp | local | 25% | 119 / 119 | 0.000 | False |
| baseline | tcp | local | 50% | 119 / 119 | 0.000 | False |
| baseline | tcp | pr | 50% | 118 / 119 | 1.000 | False |
| baseline | tcp | pr | 70% | 72 / 72 | 0.000 | unknown |
| baseline | tcp | local | 70% | 119 / 119 | 0.000 | False |
| baseline | udp | local | 0% | 119 / 119 | 0.000 | False |
| baseline | udp | pr | 0% | 119 / 119 | 0.000 | False |
| baseline | udp | pr | 25% | 119 / 119 | 0.000 | False |
| baseline | udp | local | 25% | 119 / 119 | 0.000 | False |
| baseline | udp | local | 50% | 119 / 119 | 0.000 | False |
| baseline | udp | pr | 50% | 84 / 117 | 15.002 | unknown |
| baseline | udp | pr | 70% | 27 / 42 | 14.995 | unknown |
| baseline | udp | local | 70% | 119 / 119 | 0.000 | False |
| fixed | tcp | local | 0% | 114 / 119 | 4.006 | False |
| fixed | tcp | pr | 0% | 118 / 119 | 0.890 | False |
| fixed | tcp | pr | 25% | 115 / 118 | 2.019 | False |
| fixed | tcp | local | 25% | 116 / 119 | 1.001 | False |
| fixed | tcp | local | 50% | 119 / 119 | 0.000 | False |
| fixed | tcp | pr | 50% | 118 / 119 | 0.168 | False |
| fixed | tcp | pr | 70% | 114 / 119 | 1.038 | False |
| fixed | tcp | local | 70% | 53 / 53 | 0.000 | unknown |
| fixed | udp | local | 0% | 119 / 119 | 0.000 | False |
| fixed | udp | pr | 0% | 119 / 119 | 0.000 | False |
| fixed | udp | pr | 25% | 119 / 119 | 0.000 | False |
| fixed | udp | local | 25% | 119 / 119 | 0.000 | False |
| fixed | udp | local | 50% | 119 / 119 | 0.000 | False |
| fixed | udp | pr | 50% | 119 / 119 | 0.000 | False |
| fixed | udp | pr | 70% | 119 / 119 | 0.000 | False |
| fixed | udp | local | 70% | 117 / 119 | 1.002 | False |
| fixed-all-ipv4 | all | local | 0% | 119 / 119 | 0.000 | False |
| fixed-all-ipv4 | all | pr | 0% | 119 / 119 | 0.000 | False |
| fixed-all-ipv4 | all | pr | 25% | 119 / 119 | 0.000 | False |
| fixed-all-ipv4 | all | local | 25% | 119 / 119 | 0.000 | False |
| fixed-all-ipv4 | all | local | 50% | 119 / 119 | 0.000 | False |
| fixed-all-ipv4 | all | pr | 50% | 119 / 119 | 0.000 | False |
| fixed-all-ipv4 | all | pr | 70% | 80 / 119 | 39.008 | unknown |
| fixed-all-ipv4 | all | local | 70% | 40 / 40 | 0.000 | unknown |

These are1Hz central-region pixel comparisons, excluding samples at/after the first logged terminal event. An unchanged sampled span is not continuous frame measurement. The10s attention threshold flags inspection of a surviving session, without declaring native failure.
