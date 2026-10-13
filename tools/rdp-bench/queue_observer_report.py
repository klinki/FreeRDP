#!/usr/bin/env python3
"""Audited paired on/off observer study; disabled observations remain unknown."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from queue_report import (SUBSETS, aggregate_campaign, enrich, fmt_stats, invariants_audit,
                     pair, stats, write_report)

METRICS = (
    "c2s.verified_goodput_mbps", "client.probe_rtt_ms.p95", "client.probe_rtt_ms.p99",
    "client.probe_scheduled_ms.p99", "client.cpu_total_s", "server.cpu_total_s",
    "client.send_call_ms.p95", "client.send_call_ms.p99", "client.send_call_ms.max",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Directory containing the sealed sweep campaigns")
    parser.add_argument("--campaign", default="observer-study")
    parser.add_argument("--output-prefix", default="observer-analysis")
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    report = aggregate_campaign(root / args.campaign)
    write_report(report, root / f"{args.campaign}-report")
    rows = [enrich(row) for row in report["trials"]]
    comparisons, unpaired, cap_checks = [], [], []
    for cap in (32, 128):
        baseline, candidate = f"q{cap}-off", f"q{cap}-on"
        p, u = pair(rows, baseline, candidate, METRICS)
        comparisons.extend(p)
        unpaired.extend(u)
        members = [row for row in rows if row["variant"] in (baseline, candidate)]
        cap_checks.append({"capacity_kib": cap,
                           "rows": [{"trial_id": row["trial_id"], "variant": row["variant"],
                                     "queue_cap_bytes": row["queue_cap_bytes"],
                                     "actual_queue_limit_bytes": row["metrics"].get("client.queue_queue_limit_bytes"),
                                     "actual_window_packets": row["metrics"].get("client.queue_effective_window_packets"),
                                     "observer_enabled": row["metrics"].get("client.queue_observer_enabled"),
                                     "configuration_correct": row["queue_cap_bytes"] == cap * 1024
                                         and row["metrics"].get("client.queue_queue_limit_bytes") == cap * 1024
                                         and row["metrics"].get("client.queue_effective_window_packets") == 31
                                         and row["metrics"].get("client.queue_observer_enabled") == int(row["variant"].endswith("-on"))}
                                    for row in members]})
    failures = [{"trial_id": row["trial_id"], "kind": kind, "reason": reason}
                for row in rows for kind in ("harness_reasons", "artifact_reasons", "sut_failures", "integrity_failures")
                for reason in row.get(kind, [])]
    notes = [
        "Observer-on is compared with observer-off at the same queue cap, case, comparison_key, and seed, with the same native binary and fixed peer.",
        "Only the native queue observer changes. Normal datagram observers and Python journals remain enabled. This study measures queue observer cost, not total instrumentation overhead.",
        "This is incremental enabled queue-observer overhead within WITH_RDP_BENCH. Disabled runs retain hook sites and call-start admission lock pairs, while existing datagram counters and the fixed peer observer remain on; production without the shim is not measured by this toggle.",
        "Disabled native queue measurements remain missing; configuration/window/cap telemetry remains readable. Missing queue values are never zero-valued observations.",
        "Latency statistics are medians and ranges of whole-trial percentiles; no sample pooling.",
        "Probe timestamps precede native admission: RTT includes admission blocking, while scheduled RTT additionally includes owner-loop scheduling delay.",
        "All failed/invalid runs are retained. Conservative pair statistics require both trials artifact-intact, timing-valid, COMPLETE, and integrity-verified.",
        "Three repeats per condition are exploratory. This study cannot establish a tight instrumentation-overhead bound or certification claim. Calibration remains provisional.",
    ]
    result = {"version": 1, "notes": notes, "counts": report["counts"], "comparisons": comparisons,
              "unpaired": unpaired, "configuration_checks": cap_checks,
              "invariants_audit": invariants_audit(rows), "failures": failures, "trials": rows}
    prefix = root / args.output_prefix
    prefix.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
    lines = ["# Native queue observer on/off study", "", *[f"- {note}" for note in notes], "",
             "Counts: " + json.dumps(report["counts"], sort_keys=True), ""]
    for subset in SUBSETS:
        lines += [f"## {subset}", "", "On − off deltas; positive latency deltas mean worse latency.", "",
                  "| Case | Queue cap | Metric | Pairs | Observer off | Observer on | Paired on − off | Paired on / off |",
                  "|---|---|---|---:|---:|---:|---:|---:|"]
        for comp in comparisons:
            if comp["subset"] != subset or not comp["pairs"]:
                continue
            lines.append(f"| {comp['case_id']} | {comp['baseline_variant'].replace('-off', '')} | {comp['metric']} | {len(comp['pairs'])} | " +
                         " | ".join(fmt_stats(comp[key]) for key in ("baseline", "candidate", "paired_delta", "paired_ratio")) + " |")
        lines.append("")
    lines += ["## Failures and invalidity", ""]
    lines.extend(f"- {f['trial_id']}: {f['kind']}: {f['reason']}" for f in failures)
    if not failures:
        lines.append("No SUT, harness, artifact, or integrity failures recorded.")
    lines += [""]
    prefix.with_suffix(".md").write_text("\n".join(lines))
    with prefix.with_name(prefix.name + "-paired.csv").open("w", newline="") as stream:
        fields = ["case_id", "comparison_key", "subset", "baseline_variant", "candidate_variant", "metric", "seed",
                  "baseline_trial", "candidate_trial", "baseline", "candidate", "delta", "ratio"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for comp in comparisons:
            for record in comp["pairs"]:
                writer.writerow({**{key: comp[key] for key in fields[:6]}, **record})
    print(json.dumps({"analysis": str(prefix.with_suffix('.md')), "counts": report["counts"],
                      "comparisons": len(comparisons), "failure_reasons": len(failures), "unpaired": len(unpaired)}))


if __name__ == "__main__":
    main()
