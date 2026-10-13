# Queue-cap measurements on this Mac

Measured on `codex/udp-queue-sweep`, based on sender commit `5134a26d2` plus the
benchmark-only observer and outgoing mixed workload. All variants use frozen
native binary SHA-256
`82d087dd76ec6db66e21c4865b482c945e624236de5856c3ead322d106d38f0c`,
the same fixed 128 KiB reference peer, and the same 31-packet window. Each main
condition uses five seeds (11, 29, 47, 71, 101), five seconds of warmup, 30 seconds
of measurement and ten seconds of drain. Payload records are 16 KiB with 20 Hz
probes in the outgoing mixed workload. Both profiles cap each direction at
10 Mbit/s and add 30 ms RTT; impairment adds +/-1 ms jitter and 0.05% random loss.

48 KiB is the strongest candidate for a follow-up with live TLS/Windows: pure
upload roughly doubles, and clean loaded median p99 falls from 67 to 48 ms.
64 KiB adds about 7% clean throughput over 48 KiB with about 27 ms more paired
loaded p99. 128 KiB adds little throughput over 64 KiB and substantially more
queue residence. Impaired tails and scheduled-probe misses persist at every cap;
these results support a throughput/latency tradeoff, rather than a general
loss-tail improvement or a validated production default. Normal builds use
32 KiB; experimental overrides and observations require `WITH_RDP_BENCH`.

`TestRdpeUdp`, 19 sender scenarios, four Mac socket/event scenarios, eight queue
observer test groups and all 50 Python tests pass. Normal builds also pass the
three applicable native targets and export neither benchmark nor observer hooks.
The portable queue and observer reporters reproduce the audited results below.
Calibration remains provisional. The raw campaign does not execute TLS,
Windows interoperability or multitransport maintenance callbacks.

Raw artifacts and JSON/CSV reports are retained in
`build/udp-queue-sweep-20261011/`, including `combined-summary.json`,
`analysis.json`, `packet-ledger.csv` and `probe-failures.csv`. Reproduce a fresh
sweep using a frozen library and matching build metadata with `queue_sweep.py`.
Audit and summarize it using:

```sh
python3 -B tools/rdp-bench/queue_report.py --root build/your-sweep --campaign campaign
python3 -B tools/rdp-bench/queue_observer_report.py --root build/your-sweep
```

- Native UDP2 raw steady-state transport experiment; calibration remains provisional unless an accepted receipt explicitly proves otherwise.
- Queue capacity is the treatment. Native binary, 31-packet production window, peer binary/environment, profile, workload, offered load, measurement duration, harness, and seed must be held fixed within a paired comparison.
- Summaries are medians/minima/maxima of whole-trial statistics. Per-trial p95/p99 values are never pooled into a fictitious combined percentile.
- Goodput is payload goodput verified after drain by complete byte/hash/order integrity checks. Failed integrity suppresses verified goodput.
- Observed-probe latency does not include missing probes; expected/injected/observed/missed/timeouts are reported separately, retaining every failure.
- send_call_ms measures whole native API-call duration, including admission/backpressure and service work. Native accept-to-first-send residence is a distinct metric; neither is silently substituted for the other.
- Queue residence is byte-weighted for bytes accepted during measurement, with first transmission observed through drain; admission waiting is call-weighted for calls started during measurement. Histograms use 125 µs bins. p95/p99 scalars are bin upper bounds; recorded maxima are exact. Lower/upper quantile bounds remain in JSON/CSV.
- The residence endpoint is the timestamp immediately before the first successful nonblocking socket send call, counted only after success. It does not measure kernel completion or physical wire departure.
- Probe injection timestamps precede native queue admission, so reported RTT includes admission blocking; scheduled RTT additionally includes owner-loop scheduling delay.
- A histogram quantile upper bound can exceed the exact maximum when all values lie below the end of that bin. This is a valid bound, not an inverted measured quantile.
- Timing-valid COMPLETE comparisons can remain provisional. INVALID_HARNESS and SUT-failed trials remain diagnostic; INVALID_ARTIFACT metrics are withheld from paired calculations.
- Native retransmission/drop/peak telemetry is whole-trial unless the recorded observer scope explicitly states otherwise. Network injected loss counts are separate from native negative ACKVEC observations.
- No TLS, RDP setup, Windows, rendering, or end-to-end desktop latency result is implied.
- The 80-trial 5/30/10-second main campaign is the cap sweep. The eight smoke, 24 observer, and 24 unloaded-control trials remain separate short diagnostics; unlike conditions and durations are not pooled.
- All four phase input artifacts were seal-audited before this combination. Existing datagram counters and the fixed peer observer remain on throughout; observer on/off estimates only incremental enabled queue-observer overhead within WITH_RDP_BENCH.

## Audited campaign accounting

| Phase | Trials | Integrity verified | Timing-valid COMPLETE | SUT service failures | Relay/timing invalid | Artifact failures |
|---|---:|---:|---:|---:|---:|---:|
| smoke | 8 | 8 | 8 | 0 | 0 | 0 |
| observer-study | 24 | 24 | 22 | 2 | 0 | 0 |
| controls | 24 | 24 | 24 | 0 | 0 | 0 |
| campaign | 80 | 80 | 68 | 11 | 4 | 0 |

All 136 trials verified application payload integrity. Across all phases, 40512 injected probes were observed and 128 scheduled probes were not injected; 0 probe timeouts. Missing injections remain service failures.

## All observed main trials, diagnostic

Median [min–max] of whole-trial statistics. Each main condition planned five seeds.

| Profile | Cap | Pure upload N / Mbit/s | Loaded N / Mbit/s | Loaded RTT p95 | Loaded RTT p99 | Scheduled p99 |
|---|---|---|---|---:|---:|---:|
| Clean 10 Mbit/s | 32 KiB | 5 / 3.971 [3.969–3.974] | 5 / 3.969 [3.811–3.971] | 66.776 [66.726–73.538] | 67.063 [66.901–161.415] | 98.899 [98.746–319.757] |
| Clean 10 Mbit/s | 48 KiB | 5 / 8.234 [8.233–8.236] | 5 / 8.244 [8.244–8.251] | 47.978 [47.960–48.009] | 48.275 [48.252–48.695] | 63.399 [63.267–63.451] |
| Clean 10 Mbit/s | 64 KiB | 5 / 8.842 [8.838–8.860] | 5 / 8.839 [7.940–8.849] | 74.540 [74.157–121.348] | 75.372 [75.098–495.101] | 86.932 [85.677–642.569] |
| Clean 10 Mbit/s | 128 KiB | 5 / 8.846 [8.839–8.852] | 5 / 8.844 [8.814–8.856] | 133.283 [132.963–133.744] | 134.678 [134.275–143.133] | 147.724 [146.374–155.642] |
| Impaired 10 Mbit/s | 32 KiB | 5 / 3.958 [3.869–3.985] | 5 / 3.985 [3.869–4.010] | 67.034 [65.829–178.314] | 217.117 [214.068–239.049] | 233.412 [225.875–263.723] |
| Impaired 10 Mbit/s | 48 KiB | 5 / 7.748 [7.539–7.861] | 5 / 7.792 [7.492–7.914] | 64.209 [63.798–64.866] | 236.079 [232.447–251.609] | 252.379 [239.143–322.467] |
| Impaired 10 Mbit/s | 64 KiB | 5 / 8.061 [7.820–8.218] | 5 / 8.099 [7.795–8.189] | 78.748 [78.600–231.034] | 251.093 [248.485–264.262] | 261.127 [253.981–274.199] |
| Impaired 10 Mbit/s | 128 KiB | 5 / 8.068 [7.812–8.212] | 5 / 8.028 [6.748–8.147] | 294.880 [286.818–530.490] | 325.091 [319.718–1152.300] | 339.755 [326.647–1478.071] |

### Acceptance-to-first-successful-send-call residence, byte weighted

Quantile scalars are 125 µs bin upper bounds. Max is the median [range] of per-trial exact maxima; all values ms.

| Profile | Cap | Pure upload N | Pure p95 | Pure p99 | Pure max | Loaded N | Loaded p95 | Loaded p99 | Loaded max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Clean 10 Mbit/s | 32 KiB | 5 | 0.250 [0.250–0.250] | 0.250 [0.250–0.250] | 0.569 [0.271–4.957] | 5 | 0.125 [0.125–0.625] | 0.250 [0.250–1.625] | 0.377 [0.304–39.719] |
| Clean 10 Mbit/s | 48 KiB | 5 | 10.875 [10.875–10.875] | 11.750 [11.750–11.750] | 14.749 [13.731–17.427] | 5 | 11.000 [11.000–11.000] | 11.875 [11.875–11.875] | 15.861 [13.352–21.764] |
| Clean 10 Mbit/s | 64 KiB | 5 | 27.000 [26.875–27.000] | 28.000 [27.875–28.000] | 32.919 [31.492–50.309] | 5 | 26.875 [26.875–27.750] | 27.875 [27.875–53.375] | 33.205 [30.269–364.953] |
| Clean 10 Mbit/s | 128 KiB | 5 | 86.125 [86.000–86.125] | 87.375 [87.250–87.375] | 93.402 [91.257–95.278] | 5 | 86.000 [86.000–86.125] | 87.375 [87.000–88.000] | 106.763 [88.796–166.469] |
| Impaired 10 Mbit/s | 32 KiB | 5 | 0.250 [0.125–0.250] | 95.750 [0.250–106.375] | 143.609 [141.866–146.435] | 5 | 0.125 [0.125–0.250] | 100.750 [0.250–111.500] | 144.444 [142.316–173.275] |
| Impaired 10 Mbit/s | 48 KiB | 5 | 13.000 [12.875–13.500] | 159.125 [154.500–169.500] | 189.836 [189.045–348.668] | 5 | 13.375 [13.000–13.375] | 159.375 [145.375–168.000] | 188.052 [184.921–345.571] |
| Impaired 10 Mbit/s | 64 KiB | 5 | 28.375 [28.125–28.875] | 187.500 [183.375–190.500] | 206.190 [202.295–357.396] | 5 | 28.375 [28.250–29.000] | 187.250 [179.125–192.000] | 205.455 [202.007–363.981] |
| Impaired 10 Mbit/s | 128 KiB | 5 | 90.625 [89.500–247.000] | 257.375 [256.375–258.375] | 267.783 [267.183–426.180] | 5 | 103.500 [89.625–246.750] | 258.125 [256.875–543.750] | 425.422 [266.356–1707.975] |

### Native admission wait, call weighted

Quantile scalars are 125 µs bin upper bounds. Max is the median [range] of per-trial exact maxima; all values ms.

| Profile | Cap | Pure upload N | Pure p95 | Pure p99 | Pure max | Loaded N | Loaded p95 | Loaded p99 | Loaded max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Clean 10 Mbit/s | 32 KiB | 5 | 33.250 [33.250–33.250] | 33.375 [33.375–33.625] | 34.177 [34.073–41.165] | 5 | 33.125 [33.000–33.125] | 33.375 [33.250–45.000] | 34.220 [34.063–402.367] |
| Clean 10 Mbit/s | 48 KiB | 5 | 16.125 [16.125–16.125] | 16.500 [16.500–16.625] | 16.887 [16.603–20.150] | 5 | 16.125 [16.125–16.125] | 16.500 [16.250–16.500] | 18.374 [16.558–32.556] |
| Clean 10 Mbit/s | 64 KiB | 5 | 15.375 [15.375–16.125] | 15.750 [15.625–16.375] | 19.052 [17.610–30.327] | 5 | 15.375 [15.375–16.125] | 16.250 [16.000–25.000] | 18.649 [17.107–294.806] |
| Clean 10 Mbit/s | 128 KiB | 5 | 15.500 [15.375–15.500] | 16.125 [15.625–16.250] | 21.616 [17.914–22.409] | 5 | 15.375 [15.375–16.000] | 16.250 [15.500–16.250] | 28.709 [16.387–42.056] |
| Impaired 10 Mbit/s | 32 KiB | 5 | 33.625 [33.625–33.875] | 35.000 [34.375–138.875] | 151.251 [149.247–152.192] | 5 | 33.250 [32.625–33.375] | 34.000 [33.500–34.625] | 173.454 [150.350–175.933] |
| Impaired 10 Mbit/s | 48 KiB | 5 | 18.625 [18.500–18.625] | 20.125 [19.750–21.875] | 190.931 [188.511–191.733] | 5 | 18.250 [18.125–18.500] | 19.750 [19.625–20.250] | 190.333 [187.636–191.593] |
| Impaired 10 Mbit/s | 64 KiB | 5 | 17.750 [17.625–17.875] | 19.500 [18.875–29.250] | 191.917 [189.910–193.136] | 5 | 17.375 [17.250–17.375] | 18.750 [18.625–19.125] | 192.215 [189.791–193.644] |
| Impaired 10 Mbit/s | 128 KiB | 5 | 17.750 [17.625–17.875] | 19.375 [18.875–23.875] | 190.028 [189.671–191.035] | 5 | 17.375 [17.250–17.625] | 18.875 [18.500–36.250] | 192.078 [191.216–656.515] |
## Timing-valid COMPLETE main trials, provisional

Median [min–max] of whole-trial statistics. Each main condition planned five seeds.

| Profile | Cap | Pure upload N / Mbit/s | Loaded N / Mbit/s | Loaded RTT p95 | Loaded RTT p99 | Scheduled p99 |
|---|---|---|---|---:|---:|---:|
| Clean 10 Mbit/s | 32 KiB | 5 / 3.971 [3.969–3.974] | 4 / 3.970 [3.969–3.971] | 66.775 [66.726–66.819] | 67.055 [66.901–67.067] | 98.857 [98.746–99.162] |
| Clean 10 Mbit/s | 48 KiB | 5 / 8.234 [8.233–8.236] | 5 / 8.244 [8.244–8.251] | 47.978 [47.960–48.009] | 48.275 [48.252–48.695] | 63.399 [63.267–63.451] |
| Clean 10 Mbit/s | 64 KiB | 5 / 8.842 [8.838–8.860] | 4 / 8.844 [8.835–8.849] | 74.459 [74.157–74.598] | 75.298 [75.098–75.544] | 86.356 [85.677–87.781] |
| Clean 10 Mbit/s | 128 KiB | 5 / 8.846 [8.839–8.852] | 5 / 8.844 [8.814–8.856] | 133.283 [132.963–133.744] | 134.678 [134.275–143.133] | 147.724 [146.374–155.642] |
| Impaired 10 Mbit/s | 32 KiB | 5 / 3.958 [3.869–3.985] | 3 / 3.985 [3.869–4.004] | 67.238 [67.010–178.314] | 216.465 [214.068–238.065] | 233.412 [225.875–261.090] |
| Impaired 10 Mbit/s | 48 KiB | 5 / 7.748 [7.539–7.861] | 2 / 7.823 [7.732–7.914] | 64.332 [63.798–64.866] | 242.028 [232.447–251.609] | 251.408 [239.143–263.673] |
| Impaired 10 Mbit/s | 64 KiB | 5 / 8.061 [7.820–8.218] | 4 / 8.112 [7.795–8.189] | 78.683 [78.600–231.034] | 249.934 [248.485–263.339] | 259.363 [253.981–274.199] |
| Impaired 10 Mbit/s | 128 KiB | 5 / 8.068 [7.812–8.212] | 1 / 8.028 [8.028–8.028] | 294.880 [294.880–294.880] | 319.718 [319.718–319.718] | 326.647 [326.647–326.647] |

### Acceptance-to-first-successful-send-call residence, byte weighted

Quantile scalars are 125 µs bin upper bounds. Max is the median [range] of per-trial exact maxima; all values ms.

| Profile | Cap | Pure upload N | Pure p95 | Pure p99 | Pure max | Loaded N | Loaded p95 | Loaded p99 | Loaded max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Clean 10 Mbit/s | 32 KiB | 5 | 0.250 [0.250–0.250] | 0.250 [0.250–0.250] | 0.569 [0.271–4.957] | 4 | 0.125 [0.125–0.125] | 0.250 [0.250–0.250] | 0.376 [0.304–1.209] |
| Clean 10 Mbit/s | 48 KiB | 5 | 10.875 [10.875–10.875] | 11.750 [11.750–11.750] | 14.749 [13.731–17.427] | 5 | 11.000 [11.000–11.000] | 11.875 [11.875–11.875] | 15.861 [13.352–21.764] |
| Clean 10 Mbit/s | 64 KiB | 5 | 27.000 [26.875–27.000] | 28.000 [27.875–28.000] | 32.919 [31.492–50.309] | 4 | 26.875 [26.875–27.000] | 27.875 [27.875–28.000] | 32.723 [30.269–33.669] |
| Clean 10 Mbit/s | 128 KiB | 5 | 86.125 [86.000–86.125] | 87.375 [87.250–87.375] | 93.402 [91.257–95.278] | 5 | 86.000 [86.000–86.125] | 87.375 [87.000–88.000] | 106.763 [88.796–166.469] |
| Impaired 10 Mbit/s | 32 KiB | 5 | 0.250 [0.125–0.250] | 95.750 [0.250–106.375] | 143.609 [141.866–146.435] | 3 | 0.125 [0.125–0.250] | 0.250 [0.250–111.500] | 144.444 [142.316–148.538] |
| Impaired 10 Mbit/s | 48 KiB | 5 | 13.000 [12.875–13.500] | 159.125 [154.500–169.500] | 189.836 [189.045–348.668] | 2 | 13.188 [13.000–13.375] | 153.000 [145.375–160.625] | 187.933 [187.815–188.052] |
| Impaired 10 Mbit/s | 64 KiB | 5 | 28.375 [28.125–28.875] | 187.500 [183.375–190.500] | 206.190 [202.295–357.396] | 4 | 28.438 [28.250–29.000] | 185.562 [179.125–192.000] | 205.123 [202.007–358.298] |
| Impaired 10 Mbit/s | 128 KiB | 5 | 90.625 [89.500–247.000] | 257.375 [256.375–258.375] | 267.783 [267.183–426.180] | 1 | 103.500 [103.500–103.500] | 258.625 [258.625–258.625] | 276.583 [276.583–276.583] |

### Native admission wait, call weighted

Quantile scalars are 125 µs bin upper bounds. Max is the median [range] of per-trial exact maxima; all values ms.

| Profile | Cap | Pure upload N | Pure p95 | Pure p99 | Pure max | Loaded N | Loaded p95 | Loaded p99 | Loaded max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Clean 10 Mbit/s | 32 KiB | 5 | 33.250 [33.250–33.250] | 33.375 [33.375–33.625] | 34.177 [34.073–41.165] | 4 | 33.125 [33.000–33.125] | 33.312 [33.250–33.375] | 34.204 [34.063–37.151] |
| Clean 10 Mbit/s | 48 KiB | 5 | 16.125 [16.125–16.125] | 16.500 [16.500–16.625] | 16.887 [16.603–20.150] | 5 | 16.125 [16.125–16.125] | 16.500 [16.250–16.500] | 18.374 [16.558–32.556] |
| Clean 10 Mbit/s | 64 KiB | 5 | 15.375 [15.375–16.125] | 15.750 [15.625–16.375] | 19.052 [17.610–30.327] | 4 | 15.375 [15.375–15.875] | 16.188 [16.000–16.250] | 18.294 [17.107–21.264] |
| Clean 10 Mbit/s | 128 KiB | 5 | 15.500 [15.375–15.500] | 16.125 [15.625–16.250] | 21.616 [17.914–22.409] | 5 | 15.375 [15.375–16.000] | 16.250 [15.500–16.250] | 28.709 [16.387–42.056] |
| Impaired 10 Mbit/s | 32 KiB | 5 | 33.625 [33.625–33.875] | 35.000 [34.375–138.875] | 151.251 [149.247–152.192] | 3 | 33.250 [33.250–33.375] | 34.000 [33.750–34.625] | 170.827 [150.350–175.024] |
| Impaired 10 Mbit/s | 48 KiB | 5 | 18.625 [18.500–18.625] | 20.125 [19.750–21.875] | 190.931 [188.511–191.733] | 2 | 18.188 [18.125–18.250] | 19.688 [19.625–19.750] | 189.109 [187.886–190.333] |
| Impaired 10 Mbit/s | 64 KiB | 5 | 17.750 [17.625–17.875] | 19.500 [18.875–29.250] | 191.917 [189.910–193.136] | 4 | 17.375 [17.250–17.375] | 18.750 [18.625–19.125] | 191.253 [189.791–193.644] |
| Impaired 10 Mbit/s | 128 KiB | 5 | 17.750 [17.625–17.875] | 19.375 [18.875–23.875] | 190.028 [189.671–191.035] | 1 | 17.500 [17.500–17.500] | 18.875 [18.875–18.875] | 197.712 [197.712–197.712] |

## Strict paired main intersections

Pairs share case/comparison_key/seed and require both artifact-intact, timing-valid COMPLETE outcomes. Failed/invalid trials remain in diagnostic results above.

| Case | Baseline | Candidate | Eligible pairs | Seeds |
|---|---|---|---:|---|
| clean-upload | q32 | q48 | 5 | 11, 29, 47, 71, 101 |
| clean-upload-mixed | q32 | q48 | 4 | 11, 29, 47, 71 |
| impaired-upload | q32 | q48 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload-mixed | q32 | q48 | 2 | 11, 47 |
| clean-upload | q32 | q64 | 5 | 11, 29, 47, 71, 101 |
| clean-upload-mixed | q32 | q64 | 3 | 11, 29, 47 |
| impaired-upload | q32 | q64 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload-mixed | q32 | q64 | 3 | 11, 47, 71 |
| clean-upload | q32 | q128 | 5 | 11, 29, 47, 71, 101 |
| clean-upload-mixed | q32 | q128 | 4 | 11, 29, 47, 71 |
| impaired-upload | q32 | q128 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload-mixed | q32 | q128 | 1 | 47 |
| clean-upload | q48 | q64 | 5 | 11, 29, 47, 71, 101 |
| clean-upload-mixed | q48 | q64 | 4 | 11, 29, 47, 101 |
| impaired-upload | q48 | q64 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload-mixed | q48 | q64 | 2 | 11, 47 |
| clean-upload | q48 | q128 | 5 | 11, 29, 47, 71, 101 |
| clean-upload-mixed | q48 | q128 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload | q48 | q128 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload-mixed | q48 | q128 | 1 | 47 |
| clean-upload | q64 | q128 | 5 | 11, 29, 47, 71, 101 |
| clean-upload-mixed | q64 | q128 | 4 | 11, 29, 47, 101 |
| impaired-upload | q64 | q128 | 5 | 11, 29, 47, 71, 101 |
| impaired-upload-mixed | q64 | q128 | 1 | 47 |

## Probe service failures, all phases

| Phase | Trial | Missed IDs | Missed count | During measurement | Timeouts |
|---|---|---|---:|---:|---:|
| campaign | clean-upload-mixed-q32-s101-85fb7394 | 325–328, 333–339, 341–342, 345–346 | 15 | 15 | 0 |
| campaign | clean-upload-mixed-q64-s71-57c6b20d | 381, 392–396, 399–400, 402–403, 405–412, 418, 420–425 | 25 | 25 | 0 |
| campaign | impaired-upload-mixed-q128-s101-14a0ca8c | 286–287 | 2 | 2 | 0 |
| campaign | impaired-upload-mixed-q128-s11-13f0d453 | 123–127, 129–150, 152, 154–160, 162, 170–172, 174–175, 192–193, 195–202, 206–217, 225–229, 238, 242–245, 247 | 74 | 74 | 0 |
| campaign | impaired-upload-mixed-q128-s29-13473127 | 455 | 1 | 1 | 0 |
| campaign | impaired-upload-mixed-q128-s71-7243c540 | 73 | 1 | 0 | 0 |
| campaign | impaired-upload-mixed-q32-s101-8d7b2ee9 | 514 | 1 | 1 | 0 |
| campaign | impaired-upload-mixed-q48-s101-3fdb4a41 | 288 | 1 | 1 | 0 |
| campaign | impaired-upload-mixed-q48-s29-0f2d7ba7 | 458 | 1 | 1 | 0 |
| campaign | impaired-upload-mixed-q48-s71-56f978ba | 74 | 1 | 0 | 0 |
| campaign | impaired-upload-mixed-q64-s29-131e009c | 455–456 | 2 | 2 | 0 |
| observer-study | impaired-upload-mixed-q32-off-s11-c7023a9c | 238–239 | 2 | 2 | 0 |
| observer-study | impaired-upload-mixed-q32-on-s11-6454c602 | 238–239 | 2 | 2 | 0 |

## Packet ledger and actual losses

272 direction rows reconciled; 0 ledger issues. Native sent counts match relay ingress, and relay ingress minus actual drops plus duplication matches delivered counts. Delivered counts match native receive counts. Raw byte equality is checked within the relay only for zero-drop/nonduplicate directions; endpoint byte/hash checks independently verify application payload.

| Phase | Actual random drops | Queue drops | Native retransmissions | Native negative ACKVEC reports |
|---|---:|---:|---:|---:|
| smoke | 8 | 0 | 4 | 7800 |
| observer-study | 84 | 0 | 47 | 59183 |
| controls | 12 | 0 | 8 | 20 |
| campaign | 1027 | 9 | 823 | 636836 |

Negative ACKVEC reports are repeated observations about outstanding sequence attempts, not packet loss counts. Retransmissions during invalid relay timing remain diagnostic.

## Harness invalidity and all failure reasons

- observer-study / impaired-upload-mixed-q32-off-s11-c7023a9c: sut_failures: probe deadlines unserved/unobserved
- observer-study / impaired-upload-mixed-q32-on-s11-6454c602: sut_failures: probe deadlines unserved/unobserved
- campaign / clean-upload-mixed-q32-s101-85fb7394: harness_reasons: c2s relay p99 lateness 8.109ms exceeds 1.500ms
- campaign / clean-upload-mixed-q32-s101-85fb7394: harness_reasons: s2c relay p99 lateness 4.495ms exceeds 1.500ms
- campaign / clean-upload-mixed-q32-s101-85fb7394: sut_failures: probe deadlines unserved/unobserved
- campaign / clean-upload-mixed-q64-s71-57c6b20d: harness_reasons: c2s relay p99 lateness 25.660ms exceeds 1.500ms
- campaign / clean-upload-mixed-q64-s71-57c6b20d: harness_reasons: s2c relay p99 lateness 19.514ms exceeds 1.500ms
- campaign / clean-upload-mixed-q64-s71-57c6b20d: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q128-s101-14a0ca8c: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q128-s11-13f0d453: harness_reasons: c2s relay p99 lateness 38.894ms exceeds 1.500ms
- campaign / impaired-upload-mixed-q128-s11-13f0d453: harness_reasons: s2c relay p99 lateness 27.499ms exceeds 1.500ms
- campaign / impaired-upload-mixed-q128-s11-13f0d453: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q128-s29-13473127: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q128-s71-7243c540: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q32-s101-8d7b2ee9: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q32-s29-c0a98070: harness_reasons: s2c relay p99 lateness 1.523ms exceeds 1.500ms
- campaign / impaired-upload-mixed-q48-s101-3fdb4a41: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q48-s29-0f2d7ba7: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q48-s71-56f978ba: sut_failures: probe deadlines unserved/unobserved
- campaign / impaired-upload-mixed-q64-s29-131e009c: sut_failures: probe deadlines unserved/unobserved

## Short diagnostics remain separate

Unloaded controls: clean median p99 RTT across caps 32/48/64/128 KiB was 33.126/32.822/32.765/32.668 ms. Impaired medians were 34.158/34.368/34.321/34.253 ms; seed 29 loss tails were 131.065/126.760/138.223/131.726 ms, with one DUT and one peer retry at each cap. All 24 controls completed without missed probes.

Observer on/off: all 24 verified integrity and relay timing; two q32 impaired seed-11 runs each missed IDs 238–239. Paired median goodput effects on/off were 0.000% clean and −0.228% impaired at q32 (all-observed diagnostic), +0.296% clean and −0.022% impaired at q128. The q128 clean whole-trial DUT CPU paired median increased 6.61% (+0.0333 s). Three repeats do not establish a tight overhead bound. Read the separate observer report for COMPLETE-only subsets.

Source JSON/CSV preserves each phase, every trial, quantile bounds, failed probe IDs, per-direction losses, exclusions, and observer configuration. No calibration acceptance or production default change is implied.
