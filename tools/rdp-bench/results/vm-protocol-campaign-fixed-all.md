# Protocol-selected Windows VM loss phase

Phase: fixed-all-ipv4. Kind: performance. No pooling with other phases.

| Protocol | Client | Loss | Outcome | Seconds | Server FPS mean / median (N) | Termination |
|---|---|---:|---|---:|---:|---|
| all | local | 0% | COMPLETE | 120.004 | 4.335 / 4.390 (11) | none recorded |
| all | pr | 0% | COMPLETE | 120.001 | 14.549 / 14.661 (11) | none recorded |
| all | pr | 25% | COMPLETE | 120.003 | 14.228 / 14.685 (11) | none recorded |
| all | local | 25% | COMPLETE | 120.001 | 7.365 / 8.085 (11) | none recorded |
| all | local | 50% | COMPLETE | 120.002 | 5.549 / 4.594 (11) | none recorded |
| all | pr | 50% | COMPLETE | 120.001 | 11.223 / 11.279 (11) | none recorded |
| all | pr | 70% | SESSION_ENDED | 120.005 | 3.349 / 4.592 (11) | MAIN_TRANSPORT_TIMEOUT |
| all | local | 70% | SESSION_ENDED | 41.156 | 4.292 / 4.097 (3) | MAIN_TRANSPORT_TIMEOUT |

Server output FPS is frames made available by Windows, not physical client presentation FPS. Verified CRC goodput and transport/input-display p95/p99 remain unknown.

Only selected VM-ingress protocol receives random loss and large-packet delay/reordering. Other protocol traffic uses the explicit bypass band; its drops are retained separately. Return traffic is clean, with no bandwidth cap.

Source declarations, actual binary/library/source hashes, selector snapshots, before/after clocks, raw provider errors, contained counter windows, visual prefixes, cleanup and bounded diagnostic coverage remain in JSON.

One unseeded connection per condition. No selective retries within a phase; diagnostic runs and older full campaigns remain separately labelled.
