# Original versus full PR flight matrix

Descriptive historical comparison of complete retained matrices; transport implementation/geometry differ; no isolated-cause or statistical-significance claim

Original report: [/Users/david/projects/FreeRDP/tools/rdp-bench/results/flight-matrix-20261013.json](/Users/david/projects/FreeRDP/tools/rdp-bench/results/flight-matrix-20261013.json) · [raw trials](/Users/david/projects/FreeRDP/build/flight-loss-20261013/campaign)

PR report: [/Users/david/projects/FreeRDP/build/pr13608-reference-20261013/campaign/dashboard/dashboard.json](/Users/david/projects/FreeRDP/build/pr13608-reference-20261013/campaign/dashboard/dashboard.json) · [raw trials](/Users/david/projects/FreeRDP/build/pr13608-reference-20261013/campaign)

Native geometry, payload ceiling, source/library hashes and separate versus unified health semantics are preserved in JSON. Fixed external controls match; changing implementation and wire geometry prevents attribution to a single fix.

| Loss | DATA cap | Original / PR full-integrity rate Mbit/s (N) | Original / PR survivor p95 ms (N) | Original / PR survivor p99 ms (N) | Matched full-rate seeds N |
|---:|---:|---:|---:|---:|---:|
| 0% | 16 | 3.605 (N=5) / 4.202 (N=4) | 323.433 (N=5) / 292.304 (N=5) | 340.869 (N=5) / 413.214 (N=5) | 4 |
| 0% | 32 | 7.365 (N=5) / 8.447 (N=5) | 163.925 (N=5) / 144.608 (N=5) | 172.829 (N=5) / 268.910 (N=5) | 5 |
| 0% | 48 | 9.634 (N=5) / 9.482 (N=5) | 124.952 (N=5) / 125.112 (N=5) | 128.879 (N=5) / 225.131 (N=5) | 5 |
| 0% | 64 | 6.386 (N=5) / 9.270 (N=4) | 358.802 (N=5) / 951.218 (N=4) | 507.678 (N=5) / 1870.657 (N=4) | 4 |
| 25% | 16 | 2.424 (N=5) / 0.647 (N=2) | 1212.990 (N=5) / 8388.610 (N=2) | 1538.853 (N=5) / 8978.060 (N=2) | 2 |
| 25% | 32 | 5.161 (N=4) / 1.168 (N=2) | 920.226 (N=5) / 18582.763 (N=3) | 1395.808 (N=5) / 19211.935 (N=3) | 1 |
| 25% | 48 | 7.138 (N=5) / 1.144 (N=3) | 914.263 (N=5) / 19888.311 (N=5) | 1063.150 (N=5) / 20588.744 (N=5) | 3 |
| 25% | 64 | 7.221 (N=5) / 1.583 (N=2) | 966.562 (N=5) / 10154.109 (N=4) | 1628.392 (N=5) / 10674.680 (N=4) | 2 |
| 50% | 16 | unknown (N=0) / unknown (N=0) | 6278.312 (N=5) / unknown (N=0) | 6808.252 (N=5) / unknown (N=0) | 0 |
| 50% | 32 | unknown (N=0) / unknown (N=0) | 2422.756 (N=3) / unknown (N=0) | 2523.021 (N=3) / unknown (N=0) | 0 |
| 50% | 48 | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | 0 |
| 50% | 64 | unknown (N=0) / unknown (N=0) | 2502.741 (N=1) / unknown (N=0) | 2745.806 (N=1) / unknown (N=0) | 0 |
| 70% | 16 | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | 0 |
| 70% | 32 | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | 0 |
| 70% | 48 | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | 0 |
| 70% | 64 | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | unknown (N=0) / unknown (N=0) | 0 |

Rates above require full stream integrity but are diagnostic if timing/service qualification fails. Survivor tails include failed-trial responses and do not represent unobserved probes. Each side has its own contributing N; same-seed metric intersections and qualified-only summaries are explicit in JSON. No percentile pooling or statistical significance claim is made.

| Loss / cap | Original / PR qualified | Full integrity | Timing valid | Native failures | Driver failures | Deadlines | Incomplete streams | Missed / unobserved / timeout (original → PR) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0% / 16 | 0 / 0 | 5 / 4 | 0 / 0 | 0 / 0 | unknown / 0 | 0 / 1 | 0 / 1 | 1/0/0 → 27/0/0 |
| 0% / 32 | 0 / 1 | 5 / 5 | 0 / 1 | 0 / 0 | unknown / 0 | 0 / 0 | 0 / 0 | 0/0/0 → 158/0/0 |
| 0% / 48 | 0 / 1 | 5 / 5 | 0 / 1 | 0 / 0 | unknown / 0 | 0 / 0 | 0 / 0 | 0/0/0 → 238/0/0 |
| 0% / 64 | 0 / 0 | 5 / 4 | 0 / 0 | 0 / unknown | unknown / unknown | 0 / unknown | 0 / unknown | 8/0/0 → unknown/unknown/unknown |
| 25% / 16 | 0 / 0 | 5 / 2 | 0 / 3 | 0 / 3 | unknown / 0 | 0 / 0 | 0 / 3 | 223/8/8 → 2792/315/0 |
| 25% / 32 | 1 / 0 | 4 / 2 | 1 / 1 | 0 / 3 | unknown / 0 | 0 / 0 | 1 / 3 | 0/0/0 → 2155/589/0 |
| 25% / 48 | 0 / 0 | 5 / 3 | 0 / 0 | 0 / 0 | unknown / 0 | 0 / 0 | 0 / 2 | 0/0/0 → 1443/176/176 |
| 25% / 64 | 0 / 0 | 5 / 2 | 0 / 0 | 0 / 3 | unknown / 0 | 0 / 2 | 0 / 3 | 0/0/0 → 1782/458/294 |
| 50% / 16 | 0 / 0 | 0 / 0 | 1 / 0 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 2855/106/0 → 3401/94/21 |
| 50% / 32 | 0 / 0 | 0 / 0 | 3 / 0 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 2527/169/0 → 3288/201/0 |
| 50% / 48 | 0 / 0 | 0 / 0 | 3 / 0 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 3158/143/0 → 3181/317/0 |
| 50% / 64 | 0 / 0 | 0 / 0 | 3 / 1 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 2982/94/0 → 3077/422/0 |
| 70% / 16 | 0 / 0 | 0 / 0 | 0 / 1 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 3466/30/0 → 3484/14/0 |
| 70% / 32 | 0 / 0 | 0 / 0 | 1 / 0 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 3450/45/0 → 3460/39/0 |
| 70% / 48 | 0 / 0 | 0 / 0 | 1 / 0 | 5 / 5 | unknown / 0 | 0 / 1 | 5 / 5 | 3436/61/0 → 3430/69/20 |
| 70% / 64 | 0 / 0 | 0 / 0 | 0 / 0 | 5 / 5 | unknown / 0 | 0 / 0 | 5 / 5 | 3422/74/0 → 3428/72/0 |

The CSV/JSON also retain measurement-response counts, expected/injected/observed probes, artifact/harness invalidity, partial-record evidence and unknown-state counts. All80 per-seed pairs include both raw trial references and failure reasons. No failed50/70% observation is promoted to qualified goodput.

Evidence note: pr 0/64/71 — incomplete startup evidence. Initialization failure before measurements; not described as file tampering. Raw summary: [/Users/david/projects/FreeRDP/build/pr13608-reference-20261013/campaign/main/loss0-upload-mixed-flight64-s71-e0b5f02c/summary.json](/Users/david/projects/FreeRDP/build/pr13608-reference-20261013/campaign/main/loss0-upload-mixed-flight64-s71-e0b5f02c/summary.json). Export validity and null metric exclusion are preserved.
