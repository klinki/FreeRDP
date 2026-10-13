# Native flight-count and loss campaign, 13 October 2026

All 80 declared conditions ran once against frozen commit `0818ab87feed1063287c39a00dc071304cb9b983`. The treatment is outstanding reliable DATA packets: 16, 32, 48 or 64. This is transport-only UDP2, with no Windows, TLS, rendering or display measurement.

**Qualification is weak: only 1/80 trials passed every per-trial gate; no accepted calibration receipt exists.** Thirteen passed relay timing; 67 did not. Thirty-nine streams verified completely; 50 trials failed their SUT checks, including probe failures. Forty native failures occurred across the 50%/70% loss profiles. No benchmark cancellation deadline or artifact-audit error occurred.

Both directions use 10 Mbit/s, 15 ms one-way delay, uniform ±1 ms jitter, a 30 ms shaping queue and independent loss of 0/25/50/70% on all datagrams, including ACKs/retries. The same seeds 11/29/47/71/101 run each cell. Warmup/measurement/drain are 5/30/12 seconds, bulk records 16 KiB, echo probes 64 bytes at 20 Hz, wire MTU 1232, byte queue 128 KiB, both peers LogWindow 7, and the fixed peer has a 64-packet active limit. The final native DATA payload ceiling was 1132 bytes. Variant order is deterministically shuffled per seed.

## Diagnostic medians

These rows include timing-invalid or failed observations. Payload rates require full reciprocal byte/hash/order/CRC integrity. RTTs describe surviving measurement-scheduled responses; their trial N may differ from the goodput N. They include blocking from the send attempt. Missing values are unavailable, not zero. Five seeds support descriptive medians/ranges only.

| Loss | Flight limit | Verified payload Mbit/s (N) | Probe p95 / p99 ms (N) | SUT failures | Timing invalid | Native failures |
|---|---:|---:|---:|---:|---:|---:|
| 0% | 16 | 3.605 (5) | 323.433 / 340.869 (5) | 1/5 | 5/5 | 0/5 |
| 0% | 32 | 7.365 (5) | 163.925 / 172.829 (5) | 0/5 | 5/5 | 0/5 |
| 0% | 48 | 9.634 (5) | 124.952 / 128.879 (5) | 0/5 | 5/5 | 0/5 |
| 0% | 64 | 6.386 (5) | 358.802 / 507.678 (5) | 3/5 | 5/5 | 0/5 |
| 25% | 16 | 2.424 (5) | 1212.990 / 1538.853 (5) | 5/5 | 5/5 | 0/5 |
| 25% | 32 | 5.161 (4) | 920.226 / 1395.808 (5) | 1/5 | 4/5 | 0/5 |
| 25% | 48 | 7.138 (5) | 914.263 / 1063.150 (5) | 0/5 | 5/5 | 0/5 |
| 25% | 64 | 7.221 (5) | 966.562 / 1628.392 (5) | 0/5 | 5/5 | 0/5 |
| 50% | 16 | — (0) | 6278.312 / 6808.252 (5) | 5/5 | 4/5 | 5/5 |
| 50% | 32 | — (0) | 2422.756 / 2523.021 (3) | 5/5 | 2/5 | 5/5 |
| 50% | 48 | — (0) | — / — (0) | 5/5 | 2/5 | 5/5 |
| 50% | 64 | — (0) | 2502.741 / 2745.806 (1) | 5/5 | 2/5 | 5/5 |
| 70% | 16 | — (0) | — / — (0) | 5/5 | 5/5 | 5/5 |
| 70% | 32 | — (0) | — / — (0) | 5/5 | 4/5 | 5/5 |
| 70% | 48 | — (0) | — / — (0) | 5/5 | 4/5 | 5/5 |
| 70% | 64 | — (0) | — / — (0) | 5/5 | 5/5 | 5/5 |

## Failures and interpretation

At zero random loss, all 20 streams verified, but four trials missed nine probe injections. The 48-packet median was 9.634 Mbit/s with p95/p99 124.952/128.879 ms; 64 packets had more shaping-queue drops and retries, with median 6.386 Mbit/s and p99 507.678 ms. All zero-loss trials were timing-invalid, so this is a diagnostic pattern, not a production recommendation.

At 25% loss, 19/20 streams verified. All five 16-packet trials failed probe checks. The 32-packet seed-11 trial lacked two complete bulk records after drain and retained 7400 partial-record bytes; its endpoints exited normally but reciprocal hashes did not match. Its prefix rate is separate from verified goodput. The 32-packet seed-71 p99 reached 8011.868 ms. The only qualified provisional trial was 32 packets/seed101: 5.161724 Mbit/s, p95/p99 920.226/1421.375 ms.

All 20 trials at 50% and all 20 at 70% had native unhealthy/failure flags and incomplete streams; verified goodput is unavailable. Some failures occurred during warmup, producing no measured responses. Ten 50% and two 70% trials passed relay timing, so relay lateness alone cannot account for every failed delivery. Native snapshots do not identify the first causal failure; a read error with send_failed=1 does not establish a receiver-map defect.

The saturated upload and probes share a reliable stream and owner loop. Admission delay and stream blocking are part of RTT and missed-injection accounting. This opt-in LogWindow7/128KiB setup is different from Windows LogWindow15 and unchanged production defaults (LogWindow5/31 active packets, 32KiB queue). Next investigate fatal native recovery under high loss with tracing and independently controlled shaping before changing production defaults.

## Independent network checks and report provenance

Two complete sets of 14 raw UDP checks were retained: initial and a follow-up under one owned temporary caffeinate assertion. Both passed only 5/14 strict checks. The initial set verified rate/loss/CRC/accounting, with timing/delay failures; the repeat also suffered large scheduling interruptions, unintended queue drops and rate/accounting failures at 50%. Power control did not certify the harness or prove a cause. The single observer-off/on pair cannot establish instrumentation cost. No threshold was loosened and no failed cell was retried to select a pass.

All four short zero-loss native smokes verified data and reached their exact configured flight count. Three passed timing; 16 packets did not. Smokes are outside the 80-trial main matrix.

The frozen reporter expected explicit zero-valued relay counters although Link.summary emits sparse collections.Counter fields. After the campaign, the reporter was corrected with exact engine/source/manifest guards: only recognized omitted counter fields become zero, explicit null/unknown telemetry remains unknown, inferred fields retain provenance, and pending is never invented. The original report remains in raw evidence; final JSON names both reporter SHA256 identities. No measurement or frozen source changed.

Seven development native targets and six production targets passed before measurement; production exports excluded benchmark symbols. Python coverage passed 302 tests before the campaign and 306 after the reporting fix (2.395s), including native fixtures and the added sparse-counter/provenance regressions. Final audit covered all 80 seals and found no artifact errors.

Machine results: `results/flight-matrix-20261013.json`, `.csv`, `.md`. Raw evidence and frozen source/libraries are bundled under `results/flight-matrix-raw-20261013` in the Downloads package.
