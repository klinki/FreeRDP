# Native UDP2 loss and inflight packet-count matrix

`flight_sweep.py` prepares and runs the transport experiment used by the loss dashboard. The treatment is the **maximum active reliable DATA packet count**, rather than byte queue size or logical record size. The private benchmark-only switch `RDP_BENCH_MAX_INFLIGHT_PACKETS` accepts exactly 16, 32, 48 or 64. Leaving it unset preserves the existing benchmark adapter defaults and production behavior.

| Parameter | Values |
|---|---|
| Client active DATA limit | 16, 32, 48, 64 packets |
| Independent datagram loss | 0%, 25%, 50%, 70%, in both directions |
| Seeds | 11, 29, 47, 71, 101 |
| Workload | `upload_mixed`: client bulk upload with request/echo probes |
| Warmup / measurement / drain | 5 / 30 / 12 seconds |
| Byte queue | 131072 bytes on both endpoints |
| Protocol windows | Client and fixed peer opt into LogWindow 7; the peer DATA limit is always 64 |
| Bulk record / probe payload | 16384 / 64 bytes, with a 36-byte application record header |
| Probe schedule | 20 Hz through warmup and measurement; tails use measurement-scheduled responses |
| Network | 10 Mbit/s per direction, 15 ms one-way delay, ±1 ms jitter, 30 ms queue, 28-byte IPv4/UDP overhead |
| Native wire limit | 1232 bytes; effective DATA payload ceiling is obtained from each endpoint's native telemetry |
| Duplication / explicit reorder / blackout | None |
| Tracing | Disabled; native counters and bounded queue observation enabled |

The main matrix contains 80 trials. The client variant order is shuffled deterministically within each case/seed block. Each directional loss/jitter decision is tied to a datagram offer index and seed; changing the DATA limit changes the traffic and may change which logical packet occupies that index. Wall-clock scheduling is observed rather than assumed identical across runs. Five seeds support descriptive medians and ranges, without a confidence interval or statistical significance claim.

Both endpoints use the same frozen native library. The suite exercises the production reliable UDP2 send/receive implementation through the optional private adapter, without TLS negotiation, an RDP desktop, Windows, or visual latency. This isolates transport behavior. Independent raw UDP CRC/sequence checks exercise the impairment path and timing headroom; they are calibration checks, not substitutes for native transport stream integrity. No accepted calibration receipt is manufactured by the matrix runner.

## Running and re-auditing

Build a library with `WITH_RDP_BENCH=ON` and save its build metadata with `commit` and `binary_sha256`. Prepare the output directory before the independent network checks:

```sh
python3 tools/rdp-bench/flight_sweep.py \
  --library /absolute/path/libfreerdp3.dylib \
  --build-metadata /absolute/path/build-metadata.json \
  --output /absolute/path/flight-campaign --prepare-only
```

This writes `flight-plan.json`, the exact suite/profile/variant configurations, and an empty dashboard without invented measurements. Supply independent network receipts as `network-checks.json` with `status`, `samples`, and `summary`. Use the same arguments without `--prepare-only` to run serially. The runner checks the library, harness, reporter, and effective environment against the plan before every trial. Reporting occurs between trials. A pre-existing `main` directory is preserved and refused rather than overwritten or resumed implicitly.

After the campaign, regenerate the report from sealed artifacts:

```sh
python3 tools/rdp-bench/flight_report.py \
  --root /absolute/path/flight-campaign \
  --output /absolute/path/flight-campaign/dashboard
```

Outputs are `dashboard.json`, `trials.csv`, and `report.md`. All attempts remain in the campaign, including native failures, benchmark drain deadlines, harness-invalid observations and interrupted/unsealed attempts. Each launch has a write-before-launch receipt; failed trials are not retried to select successful samples. `condition_coverage.unrun` distinguishes never-run conditions. An interrupted controller leaves `meta.status=running` with `meta.interrupted=true`; an unfinished attempt discovered after a killed controller is reported explicitly.

## Reading the results

The three metric subsets are deliberate:

- `qualified`: COMPLETE trials with full CRC/hash/order integrity, endpoint health, flight configuration, reconciled probe accounting, and valid relay timing. Every scheduled probe must be injected and observed without timeouts or send failure.
- `integrity_valid`: the same payload and timing evidence, also allowing a probe-only FAIL. Its verified payload rate can be useful diagnostically, while the failure remains counted.
- `diagnostic`: intact observations including failed survivor tails and timing-invalid observations. A full-integrity rate with invalid harness timing is marked diagnostic. A CRC-valid received-record prefix is exported separately and never presented as complete verified goodput.

Goodput counts bulk application payload observed during the configured 30-second window. Full verification compares sent/received counts and SHA256 in both directions after drain, with per-record CRC, content and ordering checks. It excludes record headers, UDP2/IP/UDP overhead and retransmitted bytes. A partial-prefix payload rate uses the configured measurement denominator and excludes incomplete records; `measurement_observed_s` and the terminal receipt explain early exits.

RTT p95/p99 measure observed responses to probes scheduled during measurement. `scheduled_p99_ms` also includes delay before injection. These are survivor tails, accompanied by expected, injected, observed, missed, unobserved and timeout counters. Missing evidence stays `null`; a missing native failure flag is not zero-failure evidence. Rates and tails are summarized as medians across trials, with min/max and contributing `n`.

`actual_loss` is random drops divided by offers eligible after queue/blackout rejects, separately per direction and over the whole trial, including ACKs and retransmissions. Queue drops, socket errors, and pending relay copies are separate fields. A 48/64-packet burst can exceed the 30 ms shaping queue even at 0% random loss; those queue drops do not raise the reported random-loss fraction. The zero-loss control retains the same rate, delay and jitter as all other cells.

The separate 16-field `flight_config` ABI reports requested and effective limits, protocol/storage capacities, current count/span and lifetime peaks. It leaves the existing 40-field queue ABI unchanged. Lifetime peaks include warmup and drain; a measurement peak is usable only when its validity flag is set. Active sequence span can exceed the live packet count because of holes. A genuine smaller peer advertisement may leave already-sent packets above the new effective limit while blocking new sends; the report preserves that legitimate state. A configured limit need not be reached by a particular trial. `minimum_data_chunks_per_record` uses the actual reported DATA payload ceiling, making the relation between 16KiB records and wire geometry explicit.

Relay timing is provisionally usable when both directional dispatch p99 values are within `max(0.5 ms, 10% of one-way delay)`, which is 1.5 ms here, with no socket error. The underlying trial validity and all qualification notes remain visible. A local calibration receipt is needed for stronger admission; these results do not imply independently certified laboratory measurements.

## Validation before the campaign

Seven development native targets passed in 4.77 seconds; six production targets
passed in 4.50 seconds with no private benchmark exports. All 302 Python tests
passed against the development native library, including exact flight counts,
full 64-packet operation, loss-aware receiver-rate bounds and report audits.
The dashboard preview was checked on desktop and mobile, with matrix selection
and trial filtering. These checks precede all timed network samples.
