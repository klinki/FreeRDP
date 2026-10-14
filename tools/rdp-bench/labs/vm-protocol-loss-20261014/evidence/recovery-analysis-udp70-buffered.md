# PR13608 missing-channel recovery audit

Channel `0465`: observed_channel_recovery.

Offered aliases: 19; postloss alias observations: 1.

| DataSeq | Postloss matches | Native RX statuses | Emitted ACK claims | Qualified unretained claims |
|---|---:|---|---:|---:|
| `0d7b` | 0 |  | 99 | 0 |
| `0ea6` | 0 |  | 91 | 0 |
| `1011` | 0 |  | 81 | 0 |
| `123c` | 0 |  | 68 | 0 |
| `14c2` | 0 |  | 56 | 0 |
| `16f8` | 0 |  | 45 | 0 |
| `188a` | 0 |  | 35 | 0 |
| `1a23` | 0 |  | 27 | 0 |
| `1b38` | 0 |  | 22 | 0 |
| `1bff` | 0 |  | 19 | 0 |
| `1c94` | 0 |  | 16 | 0 |
| `1d03` | 0 |  | 15 | 0 |
| `1d58` | 0 |  | 13 | 0 |
| `1d92` | 0 |  | 11 | 0 |
| `1db9` | 0 |  | 10 | 0 |
| `1dd1` | 0 |  | 9 | 0 |
| `1ddf` | 0 |  | 7 | 0 |
| `1de9` | 0 |  | 2 | 0 |
| `1df4` | 1 | 1 | 2 | 0 |

Capture/native qualifications: {"offered": {"complete": true, "kernel_drops": 0, "header_error_count": 0, "serial_ambiguous": false, "absence_qualified": true, "body_snap_truncations": 590064}, "received": {"complete": true, "kernel_drops": 0, "header_error_count": 0, "serial_ambiguous": false, "absence_qualified": true, "body_snap_truncations": 240188}}; {"retained_valid": true, "complete_from_arm": true, "overwritten_events": 0, "errors": [], "header_errors": [], "rows_truncated": false, "alignment": {"offset_min_ns": 1791971429577093765, "offset_max_ns": 1791971429577130140, "bracket_ns": 36375, "wallclock_api": "GetSystemTimeAsFileTime"}, "clock_slop_ms": 20}

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
