# PR13608 missing-channel recovery audit

Channel `0a1f`: observed_channel_recovery.

Offered aliases: 25; postloss alias observations: 0.

| DataSeq | Postloss matches | Native RX statuses | Emitted ACK claims | Qualified unretained claims |
|---|---:|---|---:|---:|
| `0cb8` | 0 |  | 2369 | 0 |
| `0d39` | 0 |  | 2366 | 0 |
| `0ddb` | 0 |  | 2363 | 0 |
| `0e81` | 0 |  | 2360 | 0 |
| `1003` | 0 |  | 2352 | 0 |
| `1196` | 0 |  | 2344 | 0 |
| `133b` | 0 |  | 2335 | 0 |
| `1453` | 0 |  | 2331 | 0 |
| `151d` | 0 |  | 2327 | 0 |
| `15ab` | 0 |  | 2324 | 0 |
| `1616` | 0 |  | 2322 | 0 |
| `16c7` | 0 |  | 2317 | 0 |
| `173a` | 0 |  | 2314 | 0 |
| `17c1` | 0 |  | 2312 | 0 |
| `183c` | 0 |  | 2309 | 0 |
| `18ad` | 0 |  | 2307 | 0 |
| `1926` | 0 |  | 2304 | 0 |
| `1978` | 0 |  | 2303 | 0 |
| `19b6` | 0 |  | 2302 | 0 |
| `19e7` | 0 |  | 2301 | 0 |
| `1a10` | 0 |  | 2300 | 0 |
| `1a2e` | 0 |  | 2298 | 0 |
| `1a42` | 0 |  | 2297 | 0 |
| `1a51` | 0 |  | 2295 | 0 |
| `1a5b` | 0 |  | 2293 | 0 |

Capture/native qualifications: {"offered": {"complete": true, "kernel_drops": 0, "header_error_count": 0, "serial_ambiguous": false, "absence_qualified": true, "body_snap_truncations": 549383}, "received": {"complete": true, "kernel_drops": 0, "header_error_count": 0, "serial_ambiguous": false, "absence_qualified": true, "body_snap_truncations": 165337}}; {"retained_valid": true, "complete_from_arm": true, "overwritten_events": 0, "errors": [], "header_errors": [], "rows_truncated": false, "alignment": {"offset_min_ns": 1791971429509980028, "offset_max_ns": 1791971429509991695, "bracket_ns": 11667, "wallclock_api": "GetSystemTimeAsFileTime"}, "clock_slop_ms": 20}

- Native retry/retire concern client outbound chunks, not Windows aliases.
- ACKVEC below-base labels are not explicit positives; AoA retirement is separate from reception.
- Peer AoA retires an old DataSeq attempt, not the ChannelSeq stream chunk; another alias may still be due.
- Absence through a 16-second stream watchdog does not prove Windows abandoned a chunk; sender retry timers are not observable here.
- Nearest half-space epochs are conservative; resync, wide reverse jumps, duplicate labels and overwritten native prefixes restrict verdicts.
- Epoch assignment treats reverse jumps beyond4096 as ambiguous; this is an analysis safeguard, not the protocol receive-window size.
- Wallclock association uses the arm API bracket plus the declared clock-slop envelope; independent drift/clock-step validation is still required.
- Complete visible headers remain usable with encrypted-body-only snap truncation.
- A qualified no-postloss-match is bounded capture absence, not proof of a specific netem random-drop decision.
- Wire ACK observations prove emission/observation, not Windows acceptance or application rendering.
