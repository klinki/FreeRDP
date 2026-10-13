# Recovery fix: two 10 Mbit/s profiles

Historical descriptive comparison; no accepted calibration receipt.

Full COMPLETE-qualified cohorts below may have different populations. Use the following same-seed intersection table when comparing values.

| Profile / queue | COMPLETE-qualified N old/current | Goodput Mbit/s old→current | RTT p95 ms old→current | RTT p99 ms old→current |
|---|---:|---:|---:|---:|
| ten_mbit_clean / 48KiB | 5/5 | 8.208→8.206 | 48.280→49.119 | 49.928→52.805 |
| ten_mbit_clean / 64KiB | 5/4 | 8.798→8.793 | 75.014→75.695 | 76.439→80.908 |
| ten_mbit / 48KiB | 5/5 | 8.213→8.134 | 64.030→64.543 | 87.310→90.804 |
| ten_mbit / 64KiB | 5/5 | 8.497→8.500 | 80.034→80.331 | 104.588→116.733 |

| Same-seed COMPLETE-qualified intersection | N / seeds | Goodput Mbit/s old→current | RTT p95 ms old→current | RTT p99 ms old→current |
|---|---|---:|---:|---:|
| ten_mbit_clean / 48 KiB | 5 / 11,29,47,71,101 | 8.208→8.206 (N=5) | 48.280→49.119 (N=5) | 49.928→52.805 (N=5) |
| ten_mbit_clean / 64 KiB | 4 / 29,47,71,101 | 8.799→8.793 (N=4) | 74.932→75.695 (N=4) | 76.332→80.908 (N=4) |
| ten_mbit / 48 KiB | 5 / 11,29,47,71,101 | 8.213→8.134 (N=5) | 64.030→64.543 (N=5) | 87.310→90.804 (N=5) |
| ten_mbit / 64 KiB | 5 / 11,29,47,71,101 | 8.497→8.500 (N=5) | 80.034→80.331 (N=5) | 104.588→116.733 (N=5) |

Probe-only service failures below retain full payload integrity and valid timing; these are diagnostics, not COMPLETE results.

| Current profile / queue | Probe-only FAIL N | Integrity-verified goodput Mbit/s | Survivor RTT p95 / p99 ms | Missed / timeouts |
|---|---:|---:|---:|---:|
| ten_mbit_clean / 48KiB | 0 | unavailable | unavailable / unavailable | None / None |
| ten_mbit_clean / 64KiB | 0 | unavailable | unavailable / unavailable | None / None |
| ten_mbit / 48KiB | 0 | unavailable | unavailable / unavailable | None / None |
| ten_mbit / 64KiB | 0 | unavailable | unavailable / unavailable | None / None |

| Period / subset | Trials | Expected / injected / observed / missed / timeouts |
|---|---:|---|
| historical / complete_qualified | 20 | 14000 / 14000 / 14000 / 0 / 0 |
| historical / probe_only_failure_diagnostic | 0 | unknown / unknown / unknown / unknown / unknown |
| historical / all_intact_observed_diagnostic | 20 | 14000 / 14000 / 14000 / 0 / 0 |
| current / complete_qualified | 19 | 13300 / 13300 / 13300 / 0 / 0 |
| current / probe_only_failure_diagnostic | 0 | unknown / unknown / unknown / unknown / unknown |
| current / all_intact_observed_diagnostic | 20 | 14000 / 13913 / 13913 / 87 / 0 |

| Retained main failure / invalid trial | Outcome / validity | Missed / timeouts | Reasons |
|---|---|---:|---|
| current / clean-upload-mixed-fixed-q64-s11-4983da2b | FAIL / INVALID_HARNESS | 87 / 0 | probe deadlines unserved/unobserved; c2s relay p99 lateness 59.869ms exceeds 1.500ms; s2c relay p99 lateness 47.435ms exceeds 1.500ms |

Outcome counts: {"historical": {"COMPLETE": 20}, "current": {"COMPLETE": 19, "FAIL": 1}}

Configuration issues: 0

Separate smoke: {"COMPLETE": 4}; harness invalid 1; artifact invalid 0.

- Historical descriptive comparison across dates, changed native sources/adapter/WinPR/harness; not an isolated causal estimate of the last recovery fix.
- The fixed peer binary/environment, workload, profiles, queue overrides, seeds and5/30/10-second timings match the recorded historical conditions.
- Main statistics require intact artifacts, complete integrity-verified streams, COMPLETE SUT outcome, healthy endpoints and per-trial relay timing qualification.
- Probe-only FAIL trials with intact full payloads and valid timing are shown separately as survivor-response diagnostics; they never become COMPLETE.
- All failed, incomplete and invalid trials remain in the trial list; invalid-artifact metrics are withheld.
- Timing-invalid payload rates are integrity-only diagnostics and are excluded from every qualified throughput subset.
- Missing native failure flags or probe accounting remain unknown and withhold qualification. Legacy zero send-failure/unobserved counts may be inferred only from the hash-verified original endpoint implementation plus explicit complete integrity, all probes observed, zero timeouts/errors and healthy native flags; every inference is recorded.
- Group values are medians of whole-trial goodput and per-trial p95/p99; probe samples are never pooled.
- Observed RTT includes native admission/backpressure; scheduled RTT also includes injection scheduling delay. Missing probes/timeouts stay separate.
- No accepted calibration/headroom receipt exists; qualification is provisional local timing evidence, not certified10Mbit/s lab performance.
- Both actual lab peers advertise LogWindow5/effective31 packets; peer LogWindow15 correctness is covered elsewhere, not a larger flight window in this campaign.
- 48/64KiB are benchmark overrides. Production queue remains32KiB; upload_mixed does not stress the large Windows receive backlog.
- Native retransmission counters and network drops are whole-trial observations; neither is a measured-phase packet-loss rate unless explicitly scoped.
