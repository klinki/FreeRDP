# Protocol-selected Windows VM loss phase

Phase: fixed. Kind: performance. No pooling with other phases.

| Protocol | Client | Loss | Outcome | Seconds | Server FPS mean / median (N) | Termination |
|---|---|---:|---|---:|---:|---|
| tcp | local | 0% | COMPLETE | 120.001 | 4.643 / 2.884 (11) | none recorded |
| tcp | pr | 0% | COMPLETE | 120.001 | 5.821 / 6.060 (10) | none recorded |
| tcp | pr | 25% | COMPLETE | 120.004 | 5.524 / 5.881 (10) | none recorded |
| tcp | local | 25% | COMPLETE | 120.001 | 4.211 / 4.461 (11) | none recorded |
| tcp | local | 50% | COMPLETE | 120.001 | 7.254 / 7.288 (11) | none recorded |
| tcp | pr | 50% | COMPLETE | 120.003 | 14.569 / 16.264 (11) | none recorded |
| tcp | pr | 70% | COMPLETE | 120.005 | 4.177 / 3.036 (11) | none recorded |
| tcp | local | 70% | SESSION_ENDED | 54.110 | 6.857 / 6.882 (5) | MAIN_TRANSPORT_TIMEOUT |
| udp | local | 0% | COMPLETE | 120.002 | 4.848 / 4.794 (11) | none recorded |
| udp | pr | 0% | COMPLETE | 120.001 | 14.796 / 14.860 (11) | none recorded |
| udp | pr | 25% | COMPLETE | 120.004 | 10.315 / 10.592 (11) | none recorded |
| udp | local | 25% | COMPLETE | 120.001 | 6.908 / 7.282 (11) | none recorded |
| udp | local | 50% | COMPLETE | 120.001 | 9.052 / 8.985 (11) | none recorded |
| udp | pr | 50% | COMPLETE | 120.006 | 9.294 / 9.379 (11) | none recorded |
| udp | pr | 70% | COMPLETE | 120.004 | 4.030 / 3.994 (11) | none recorded |
| udp | local | 70% | COMPLETE | 120.001 | 4.549 / 4.595 (11) | none recorded |

Server output FPS is frames made available by Windows, not physical client presentation FPS. Verified CRC goodput and transport/input-display p95/p99 remain unknown.

Only selected VM-ingress protocol receives random loss and large-packet delay/reordering. Other protocol traffic uses the explicit bypass band; its drops are retained separately. Return traffic is clean, with no bandwidth cap.

Source declarations, actual binary/library/source hashes, selector snapshots, before/after clocks, raw provider errors, contained counter windows, visual prefixes, cleanup and bounded diagnostic coverage remain in JSON.

One unseeded connection per condition. No selective retries within a phase; diagnostic runs and older full campaigns remain separately labelled.
