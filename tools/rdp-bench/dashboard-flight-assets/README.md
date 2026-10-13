# Flight/loss dashboard

Buildless static assets for audited `flight_report.py` datasets. The implementation
selector defaults to **PR #13608** and switches evidence independently of the
metric/cohort controls.

Publish these three static files and the following audited data files together:

| Implementation | JSON dataset | Trial CSV |
|---|---|---|
| Local recovery branch | `data.json` | `trials.csv` |
| PR #13608 | `data-pr13608.json` | `trials-pr13608.csv` |

Machine comparison downloads use `compare.json` and `compare.csv`; publish the
validated comparison exporter output with the two selected datasets. The dashboard
does not generate or invent comparison values. Final source files are
`build/pr13608-reference-20261013/comparison/pr13608-flight-comparison-20261014.json`
and its `.csv`, copied to those public names.

A missing or invalid selected dataset clears previous groups, trials, network
checks, details and provenance; the UI shows unavailable evidence instead of
falling back to another implementation. Stale fetches cannot overwrite a later
selection. Neither implementation selection nor load failure resets the metric
cohort, loss condition, packet-count condition, or legend controls.

Source commit and native geometry come from selected evidence. Actual DATA payload
ceilings are derived from intact recorded trial rows, globally and per matrix
cell; they remain unavailable when no trial supplies them. Driver-fault counts
remain unknown unless the report supplies them. The intrinsic LogWindow, storage,
peer-advertisement and recovery-policy differences remain explicit.

Diagnostic observations remain the default metric cohort. Only full-integrity
streams supply goodput; partial CRC-prefix rates remain separate. Trial tails
show observed measurement-response N and missed/unobserved/timeouts. Invalid,
incomplete, native-unknown and benchmark-driver failures remain visible.

Serve over HTTP so the browser can fetch dataset files, for example:

```sh
python3 -m http.server 8868 --bind 127.0.0.1 --directory /absolute/dashboard/path
```

The JS/CSS query fingerprints must be refreshed whenever their bytes change.

Final selected JSON sources are the completed PR dashboard at
`build/pr13608-reference-20261013/campaign/dashboard/dashboard.json` and local
baseline at `build/flight-loss-20261013/campaign/dashboard/dashboard.json`.
Keep frozen dashboard files intact. UI derivative JSON may attach confirmed
operational evidence as either:

```json
{"operational_failure":{"stage":"startup","reason":"endpoint initialization timed out","source":"sealed summary reference","sha256":"source digest"}}
```

on a trial, or in `meta.operational_failures` as an array whose entries include
`trial_id` plus the same fields. An endpoint receipt proves initialization passed;
missing receipts without a supplied annotation leave startup status unknown.
The annotation does not turn unknown native/deadline flags into false or true.

Header accounting exposes timing-valid trials, qualified complete trials,
timing/health-qualified full streams, and observed/missed/unobserved probes with
counter coverage. Counts from incomplete/missing telemetry stay unknown. Drain
cancellation and native transport failure are independent, potentially overlapping
categories. Singleton survivor tails retain their actual observed-response N.
