# Protocol-selected Windows VM loss phase

Phase: baseline. Kind: performance. No pooling with other phases.

| Protocol | Client | Loss | Outcome | Seconds | Server FPS mean / median (N) | Termination |
|---|---|---:|---|---:|---:|---|
| tcp | local | 0% | COMPLETE | 120.004 | 3.339 / 3.299 (11) | none recorded |
| tcp | pr | 0% | COMPLETE | 120.000 | 13.469 / 12.691 (11) | none recorded |
| tcp | pr | 25% | COMPLETE | 120.003 | 11.871 / 12.163 (11) | none recorded |
| tcp | local | 25% | COMPLETE | 120.003 | 7.588 / 7.583 (11) | none recorded |
| tcp | local | 50% | COMPLETE | 120.001 | 7.578 / 7.692 (11) | none recorded |
| tcp | pr | 50% | COMPLETE | 120.005 | 12.520 / 13.563 (11) | none recorded |
| tcp | pr | 70% | SESSION_ENDED | 73.155 | 10.563 / 10.487 (7) | MAIN_TRANSPORT_TIMEOUT |
| tcp | local | 70% | COMPLETE | 120.003 | 7.378 / 7.587 (11) | none recorded |
| udp | local | 0% | COMPLETE | 120.003 | 7.536 / 7.884 (11) | none recorded |
| udp | pr | 0% | COMPLETE | 120.004 | 11.462 / 11.388 (11) | none recorded |
| udp | pr | 25% | COMPLETE | 120.002 | 14.903 / 14.880 (11) | none recorded |
| udp | local | 25% | COMPLETE | 120.005 | 6.844 / 6.683 (11) | none recorded |
| udp | local | 50% | COMPLETE | 120.003 | 5.593 / 5.698 (11) | none recorded |
| udp | pr | 50% | SESSION_ENDED | 117.455 | 8.813 / 8.483 (11) | UDP_FAILURE |
| udp | pr | 70% | SESSION_ENDED | 43.229 | 3.727 / 3.695 (3) | UDP_FAILURE |
| udp | local | 70% | COMPLETE | 120.002 | 4.838 / 5.187 (11) | none recorded |

Server output FPS is frames made available by Windows, not physical client presentation FPS. Verified CRC goodput and transport/input-display p95/p99 remain unknown.

Only selected VM-ingress protocol receives random loss and large-packet delay/reordering. Other protocol traffic uses the explicit bypass band; its drops are retained separately. Return traffic is clean, with no bandwidth cap.

Source declarations, actual binary/library/source hashes, selector snapshots, before/after clocks, raw provider errors, contained counter windows, visual prefixes, cleanup and bounded diagnostic coverage remain in JSON.

One unseeded connection per condition. No selective retries within a phase; diagnostic runs and older full campaigns remain separately labelled.
