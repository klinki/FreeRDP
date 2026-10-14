# PR13608 missing-channel recovery audit

Channel `9b31`: observed_channel_recovery.

Offered aliases: 20; postloss alias observations: 1.

| DataSeq | Postloss matches | Native RX statuses | Emitted ACK claims | Qualified unretained claims |
|---|---:|---|---:|---:|
| `6546` | 0 |  | 600 | 0 |
| `65cc` | 0 |  | 597 | 0 |
| `6667` | 0 |  | 594 | 0 |
| `66d2` | 0 |  | 592 | 0 |
| `67b1` | 0 |  | 587 | 0 |
| `6851` | 0 |  | 583 | 0 |
| `68ee` | 0 |  | 580 | 0 |
| `6952` | 0 |  | 578 | 0 |
| `69cb` | 0 |  | 576 | 0 |
| `6a71` | 0 |  | 573 | 0 |
| `6ae1` | 0 |  | 570 | 0 |
| `6b28` | 0 |  | 568 | 0 |
| `6bac` | 0 |  | 564 | 0 |
| `6c85` | 0 |  | 560 | 0 |
| `6d1d` | 0 |  | 558 | 0 |
| `6d8e` | 0 |  | 556 | 0 |
| `6e02` | 0 |  | 551 | 0 |
| `6ed9` | 0 |  | 546 | 0 |
| `6fa3` | 0 |  | 542 | 0 |
| `7108` | 1 | 1 | 534 | 0 |

Capture/native qualifications: {"offered": {"complete": true, "kernel_drops": 0, "header_error_count": 0, "serial_ambiguous": false, "absence_qualified": true, "body_snap_truncations": 702161}, "received": {"complete": true, "kernel_drops": 0, "header_error_count": 0, "serial_ambiguous": false, "absence_qualified": true, "body_snap_truncations": 212249}}; {"retained_valid": true, "complete_from_arm": true, "overwritten_events": 0, "errors": [], "header_errors": [], "rows_truncated": false, "alignment": {"offset_min_ns": 1791971429506615276, "offset_max_ns": 1791971429506632068, "bracket_ns": 16792, "wallclock_api": "GetSystemTimeAsFileTime"}, "clock_slop_ms": 20}

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
