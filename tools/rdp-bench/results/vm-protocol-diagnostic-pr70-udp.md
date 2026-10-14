# Protocol-selected Windows VM loss phase

Phase: baseline-pr70-udp-diagnostic. Kind: diagnostic. No pooling with other phases.

| Protocol | Client | Loss | Outcome | Seconds | Server FPS mean / median (N) | Termination |
|---|---|---:|---|---:|---:|---|
| udp | pr | 70% | COMPLETE | 120.001 | 3.902 / 3.895 (11) | none recorded |

Server output FPS is frames made available by Windows, not physical client presentation FPS. Verified CRC goodput and transport/input-display p95/p99 remain unknown.

Only selected VM-ingress protocol receives random loss and large-packet delay/reordering. Other protocol traffic uses the explicit bypass band; its drops are retained separately. Return traffic is clean, with no bandwidth cap.

Source declarations, actual binary/library/source hashes, selector snapshots, before/after clocks, raw provider errors, contained counter windows, visual prefixes, cleanup and bounded diagnostic coverage remain in JSON.

One unseeded connection per condition. No selective retries within a phase; diagnostic runs and older full campaigns remain separately labelled.
