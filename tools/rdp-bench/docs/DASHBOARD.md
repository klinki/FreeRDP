# Offline benchmark dashboard

The exporter reads existing audited report JSON and creates a buildless local
dashboard. It does not run a benchmark, approve calibration, upload data or
require network access. The transport and Windows tabs retain their different
measurement scopes.

```sh
python3 -B tools/rdp-bench/dashboard.py --output build/bench-dashboard
# Add a recorded Windows campaign when it exists:
python3 -B tools/rdp-bench/dashboard.py \
  --live /absolute/path/to/live-results.json --output build/bench-dashboard
```

Open `build/bench-dashboard/index.html` directly. Its JSON is embedded, so it
works from `file://` without a server. `data.json` and long-form `trials.csv`
are adjacent machine-readable exports. `--loss` and `--ack` override the
default committed loss-stall and peer-window/ACK reports. Output must be in
a separate directory from the inputs; source reports are never modified.

The dashboard shows whole-trial points, strict matched comparisons and an
all-observed diagnostic subset. Comparisons match phase, case, configuration
key and seed. Their p95/p99 summaries are medians/ranges of per-trial
percentiles, never pooled percentiles. The number of complete pairs can differ
from the number with a particular metric. Missing metrics stay unavailable;
counts with missing trials say how many are known. Invalid-artifact numbers and
unverified payload goodput are withheld. Every failure, calibration limit and
recorded build identity remains visible, including failures outside the current
filters. The dashboard recomputes summaries from the supplied per-trial values
and does not independently re-audit the raw sealed campaign.

## Combining live phases and preserved failures

`live_report.py` derives one live envelope and a Markdown report from completed
phase envelopes, with a long-form `trials.csv`. It leaves source reports,
captures and TLS secrets untouched.
Use a separate output directory and add the completed main input after the
campaign ends:

```sh
python3 -B tools/rdp-bench/live_report.py \
  --input build/live-vm-bench-20261012/smoke-tls-drain/live-results.json \
  --input build/live-vm-bench-20261012/main-tls-drain/live-results.json \
  --preflights build/live-vm-bench-20261012/failed-live-preflights.json \
  --calibration build/live-vm-bench-20261012/relay-vm-calibration-tls-drain/summary.json \
  --output build/live-vm-bench-20261012/report
python3 -B tools/rdp-bench/dashboard.py \
  --live build/live-vm-bench-20261012/report/live-results.json \
  --output build/bench-dashboard-20261012
```

Omit the main input while it is still running; the tool does not pretend missing
trials have completed. Original trial IDs and per-probe observation/timeout/
censoring IDs are retained. Imported failed setup statuses have distinct
`preflight-*` phases and no manufactured metric/probe values. Incomplete setup
is counted separately from complete service with partial method evidence.

Latest calibration counts are verified against its retained sample records.
Earlier embedded calibration remains under history, with each input hash and
scope preserved. Calibration CRC echo goodput never becomes RDP goodput.
`PARTIAL_E2E_EVIDENCE` trials remain outside strict pairs even when all requested
input responses were observed. Their all-observed comparisons are diagnostic.
Setup attempts with no numeric measurement evidence remain visible as failures
but do not count as observed comparison pairs.
Reference/candidate pair declarations can come from recorded provenance variant
IDs; phase, case, configuration key and seed still must match. Changed profiles
under one ID and repeated trial IDs are rejected.

## Optional live input

Live input uses this versioned envelope. This example describes the contract;
it contains no example numeric benchmark result.

```json
{
  "schema": "freerdp.live_benchmark",
  "version": 1,
  "campaign_id": "utm-windows-campaign",
  "scope": "Live Windows RDP input response and visible progression",
  "provenance": {},
  "network_profiles": [],
  "calibration": {"approved": false, "notes": []},
  "notes": [],
  "trials": []
}
```

Each trial needs a unique `trial_id`. Record `variant`, `phase`, `case_id`,
`profile_id`, `workload`, `seed`, `comparison_key`, `validity`, `sut_outcome`,
`client_commit`/`commit` and `client_binary_sha256`/`binary_sha256` as available.
The default phase is `live`. Missing identity, validity and outcome are shown as
unknown. `metrics` is an object of finite numbers or nulls; nested objects are
flattened with dotted keys. `probes` records `requested`, `injected`, `observed`,
`unobserved`, `missed_injections` and `timeouts` as nonnegative integer counts.
Omitted counts stay unknown. A missing count is not a zero.

Use `reasons`, `negotiated`, `artifacts`, per-trial network receipts and any other
trial evidence fields to retain measurement limits. Do not put passwords or TLS
key material into the aggregate; references to private retained captures/secrets
are sufficient. Provenance should identify VM/OS, client binary/dependencies,
workload hash and dimensions, timing, controller and actual profile realization.

The recognized live metric names are:

| Key / prefix | Scope |
|---|---|
| `input_visible_ms.{count,p50,p95,p99,max}` | Input injection to verified visible remote response |
| `input_scheduled_ms.{count,p50,p95,p99,max}` | Intended injection deadline to verified response |
| `injection_lateness_ms.{count,p50,p95,p99,max}` | Driver injection lateness |
| `progress.{observed_hz,target_delivery_fraction,longest_gap_ms,freeze_fraction_over_250ms}` | Verified visible workload progression |
| `traffic.{udp_rx_Bps_mean,tcp_rx_Bps_mean,udp_tx_Bps_mean,tcp_tx_Bps_mean,diagnostic_sample_count}` | Traffic counters, not verified payload goodput |
| `transport_rtt_ms.{count,p50,p95,p99,max}` | Fresh deduplicated RDP RTT readings; diagnostic |
| `render.redraw_ms.{count,p50,p95,p99,max}` | SDL render wall duration, not end-to-end input latency |
| `render.present_interval_ms.{count,p50,p95,p99,max}` | Completed-present intervals, including idle when present |
| `verified_goodput_mbps` | Independent payload verification required |

`verified_goodput_mbps` is withheld unless the trial also records
`integrity_verified: true`. Do not infer it from network traffic, SDL uploaded
texture bytes, logical workload frames or client redraw counts.

Optional `comparison_variants: {"baseline": "name", "candidate": "name"}`
declares matching comparisons. Alternatively provide a `comparisons` array,
each with `phase`, `case_id`, `comparison_key`, `baseline_variant`,
`candidate_variant` and `metric_names`. Without a declaration, the live tab
shows recorded individual trials without inventing a before/after claim.

Live strict eligibility requires `LIVE_OBSERVATIONAL` or `PROVISIONAL_LIVE`
validity, `COMPLETE` outcome and consistent complete probe service when counts
are available. An explicit `strict_eligible: false` always excludes a trial.
These labels describe eligibility within an observational campaign and do not
certify network shaping or validate calibration. Use an unknown/invalid label
or explicit exclusion when measurement quality is insufficient.
