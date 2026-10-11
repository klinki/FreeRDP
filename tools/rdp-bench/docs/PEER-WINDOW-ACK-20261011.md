# Peer-window tracking and ACK timing on this Mac

Windows captures advertise UDP2 LogWindow 15. The receiver now retains valid peer values 2–15 independently of the local implementation limit of 10. The sender continues to use the smaller local/peer window and its existing storage bound: production defaults remain LogWindow 5, 31 active packets and a 32 KiB owned-byte queue. A peer advertisement of 15 allocates no larger queue. Native fixtures feed Microsoft wire bytes through the production receive path and cover tracking, invalid values, shrink/recovery, unchanged outgoing advertisement and the 63-packet storage ceiling.

`ack_timing_sweep.py` adds separately identified feedback-delay experiments. The relay strictly parses protected UDP2 ACK/ACKVEC-only packets and adds a direction-specific deadline offset after the existing network serialization, loss and queue decisions. DATA and piggybacked ACKs keep their original bytes and deadlines. This is feedback-delay sensitivity, rather than Windows receiver batching. Classification, actual dispatch timing, scheduler overshoot, per-ACK ledgers and the treatment enter provenance and artifact audits.

`ack_timing_report.py` independently reconciles sealed artifacts and ACK dispatches, preserves failed/missing trials, checks drop/duplicate bounds, and exports JSON/CSV/Markdown with conservative paired comparisons. Selected ACK drops cannot be identified individually from the existing aggregate shaper counters. Reports check truthful bounds and retain that limitation.

## Conditions and provenance

- Both directions capped at 10 Mbit/s, counting IPv4/UDP overhead; clean adds 30 ms RTT. Impaired adds ±1 ms one-way jitter and 0.05% random loss per direction.
- Same frozen native library and WinPR dependency throughout. Native SHA-256: `b4c64d6d1342e65fc7402d57aa2cecb8c62e2256f0324bdca9a67ee6103c2bfd`. Source parent: `794f568d7`; exact source snapshot, patch, build options and hashes are retained in `build/udp-peer-ack-20261011/`.
- Candidate queues 48/64 KiB; fixed peer 128 KiB; both peers honestly advertise LogWindow 5 and both effective windows remain 31. MTU 1232 gives the current 1132-byte payload ceiling. Tracking 15 is tested independently with native wire fixtures.
- Main: upload and outgoing bulk-plus-probes, five seeds (11,29,47,71,101), 5 s warmup / 30 s measurement / 10 s drain. ACK diagnostics: outgoing bulk-plus-probes, three seeds (11,29,47), 3/15/10 s, offsets 0/5/10/20 ms in S→C only. Every ACK condition includes classification; main trials omit it. Eight short smoke trials remain separate.
- Application records are 16 KiB and probes 20 Hz. Probe counters cover warmup and measurement; percentile samples use probes scheduled during measurement. The existing driver marks injections more than 200 ms overdue as missed. Observed percentiles omit missed probes; scheduled latency and missing counts remain explicit.
- Goodput is received application payload verified by complete byte/hash/order checks after drain. Each table value is a median of whole-trial statistics, with no pooling of percentiles. The raw suite bypasses TLS, SYN, tunnels, DVC, graphics and a real Windows peer.

## Main campaign

All-observed diagnostics, five trials per cell. Goodput Mbit/s and loaded observed RTT ms; failed service trials remain in this table.

| Profile | Cap | Upload goodput | Loaded goodput | Loaded p95 | Loaded p99 | Missed probes |
|---|---:|---:|---:|---:|---:|---:|
| Clean | 48 KiB | 8.240 | 8.302 | 47.745 | 49.197 | 0 |
| Clean | 64 KiB | 8.850 | 8.890 | 74.410 | 75.081 | 0 |
| Impaired | 48 KiB | 7.802 | 7.784 | 63.926 | 238.822 | 3 |
| Impaired | 64 KiB | 8.102 | 8.100 | 78.627 | 250.151 | 3 |

Strict same-seed, timing-valid COMPLETE comparisons retain 3 clean-upload pairs, 5 clean-loaded pairs, 5 impaired-upload pairs and only 2 impaired-loaded pairs. The last comparison has matched 48/64 KiB goodput 7.845/8.146 Mbit/s, p95 63.709/78.208 ms and p99 235.010/248.997 ms. The unpaired valid-group medians in the generated report have different sample counts and must not be substituted for that matched comparison.

## ACK feedback diagnostics

Three timing-valid COMPLETE trials per setting. Each percentile is observed probe RTT in ms; goodput Mbit/s.

| Profile | Added ACK delay | 48 KiB goodput | 48 KiB p95 / p99 | 64 KiB goodput | 64 KiB p95 / p99 |
|---|---:|---:|---:|---:|---:|
| Clean | 0 ms | 8.308 | 47.765 / 48.175 | 8.870 | 74.622 / 75.379 |
| Clean | 5 ms | 7.141 | 56.804 / 56.907 | 7.676 | 70.182 / 70.716 |
| Clean | 10 ms | 6.275 | 66.922 / 67.247 | 6.789 | 80.191 / 81.031 |
| Clean | 20 ms | 5.243 | 52.366 / 52.836 | 5.652 | 105.092 / 105.937 |
| Impaired | 0 ms | 7.960 | 63.377 / 232.961 | 8.176 | 78.330 / 248.860 |
| Impaired | 5 ms | 6.816 | 62.015 / 218.076 | 7.160 | 73.068 / 240.595 |
| Impaired | 10 ms | 6.082 | 72.954 / 220.481 | 6.441 | 83.022 / 255.707 |
| Impaired | 20 ms | 5.129 | 84.541 / 198.682 | 5.433 | 107.377 / 239.070 |

All 48 diagnostic trials pass timing, integrity and service checks. Across them, 1,367,375 datagrams were classified with zero unclassified packets; 683,514 selected successful ACK dispatches reconcile with their ledgers. Median whole-trial ACK added-delay p99 values equal the requested offset plus 0.22–0.54 ms scheduler overshoot. Every recorded deadline difference exactly equals its configured offset. Smoke adds 25,113 audited dispatches; 708,627 total.

Throughput falls as feedback is delayed, reaching 5.13/5.43 Mbit/s on the impaired 20 ms condition. RTT does not rise monotonically: clean 48 KiB observed p99 falls from 67.247 ms at 10 ms delay to 52.836 ms at 20 ms delay, while scheduled p99 rises from 83.769 to 92.530 ms and bulk admission p95 rises from 25.000 to 45.125 ms. Observed RTT alone therefore does not establish improved responsiveness. The shorter diagnostic trials and changed packet arrival/loss traces also prevent interpreting their zero misses as a repair of the main campaign service failures.

## Failures and validation

| Phase | Trials | Integrity verified | Timing-valid COMPLETE | Service failures | Timing invalid | Artifact / ACK audit invalid |
|---|---:|---:|---:|---:|---:|---:|
| Smoke | 8 | 8 | 5 | 0 | 3 | 0 / 0 |
| Main | 40 | 40 | 32 | 5 | 3 | 0 / 0 |
| ACK timing | 48 | 48 | 48 | 0 | 0 | 0 / 0 |

The five service failures are impaired loaded trials: 48 KiB seeds 29/101 and 64 KiB seeds 29/71/101. Each cap missed three injections; all 31,914 injected probes across the campaign were observed, with zero timeouts. The six timing-invalid trials exceed the existing 1.5 ms relay p99 lateness tolerance. All failures remain in JSON/CSV and the generated Markdown report.

Fresh relay calibration passes 30/36 samples. All 36 pass packet integrity and accounting; every applicable rate check passes. Six failures are timing: one clean unloaded probe sample, four paced delay-envelope samples and one headroom dispatch sample. All six headroom samples deliver 6720.2 packets/s without drops, but the failed dispatch check makes accepted headroom zero under the existing all-samples gate. External journal observer overhead has median CPU increase 6.214% and no rate decrease. No acceptance receipt is issued; results remain provisional local measurements.

Validation actually performed: production build UDP codec/sender/socket-event tests; benchmark build all four native UDP targets including queue observer; replay with sequence wrap; 74 Python tests including actual native ACK/DATA bytes and report tampering/count/deadline/output-safety regressions. Independent reviews found and corrected ACKVEC semantic validation and reporter count/output-guard issues before the relevant runs. No production queue default is changed.

48 KiB remains the balanced candidate for a live FreeRDP↔Windows follow-up. Matched baseline comparisons give 64 KiB about 4–7% more goodput, with about 14–27 ms more loaded p95 RTT. Impaired loaded evidence is limited to two strict pairs, and loss-related service failures remain. Peer-window tracking is a correctness/telemetry fix; these unchanged 31-packet laboratory windows do not test increased flight capacity from a Windows peer advertising 15.

## Reproduce and inspect

Build and freeze a native library with `build.py`, then run:

```sh
python3 -B tools/rdp-bench/ack_timing_sweep.py \
  --library /absolute/path/to/frozen/libfreerdp3.dylib \
  --build-metadata /absolute/path/to/rdp-bench-build.json \
  --output build/peer-ack-new
python3 -B tools/rdp-bench/ack_timing_report.py \
  --root build/peer-ack-new --output build/peer-ack-new/report
```

The campaign returns nonzero when service or timing checks fail and still writes completed campaigns and reports. Separate `--phase` invocations resume only pending phases, preserving completed trials and frozen provenance. `--phase control` is an optional unloaded diagnostic and was not run here.

[Machine-readable JSON](../results/peer-ack-20261011.json), [CSV](../results/peer-ack-20261011.csv), [full audited report](../results/peer-ack-20261011.md). Raw sealed trials, calibration, frozen source/library/WinPR, metadata and replay are retained under `build/udp-peer-ack-20261011/`.
