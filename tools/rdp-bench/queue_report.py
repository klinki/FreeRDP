#!/usr/bin/env python3
"""Audit and summarize a fixed-window, fixed-peer native queue-cap sweep.

Read inputs only. Outputs are separate from sealed campaigns. Failed and invalid
trials remain in all-observed diagnostics; conservative comparisons require
matching case/configuration/seed and artifact-intact timing-valid COMPLETE runs.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/rdp-bench"))
from baseline_report import aggregate_campaign, write_report

CORE_METRICS = (
    "c2s.verified_goodput_mbps", "s2c.verified_goodput_mbps",
    "client.probe_rtt_ms.p95", "client.probe_rtt_ms.p99",
    "client.probe_scheduled_ms.p95", "client.probe_scheduled_ms.p99",
    "client.send_call_ms.p95", "client.send_call_ms.p99", "client.send_call_ms.max",
    "client.queue_residence_ms.p95", "client.queue_residence_ms.p99", "client.queue_residence_ms.max",
    "client.queue_admission_ms.p95", "client.queue_admission_ms.p99", "client.queue_admission_ms.max",
    "client.queue_blocked_admission_ms.p95", "client.queue_blocked_admission_ms.p99", "client.queue_blocked_admission_ms.max",
)
SUBSETS = ("all_observed_diagnostic", "artifact_intact_timing_valid_complete", "validated_lab_complete_only")
NOTES = [
    "Native UDP2 raw steady-state transport experiment; calibration remains provisional unless an accepted receipt explicitly proves otherwise.",
    "Queue capacity is the treatment. Native binary, 31-packet production window, peer binary/environment, profile, workload, offered load, measurement duration, harness, and seed must be held fixed within a paired comparison.",
    "Summaries are medians/minima/maxima of whole-trial statistics. Per-trial p95/p99 values are never pooled into a fictitious combined percentile.",
    "Goodput is payload goodput verified after drain by complete byte/hash/order integrity checks. Failed integrity suppresses verified goodput.",
    "Observed-probe latency does not include missing probes; expected/injected/observed/missed/timeouts are reported separately, retaining every failure.",
    "send_call_ms measures whole native API-call duration, including admission/backpressure and service work. Native accept-to-first-send residence is a distinct metric; neither is silently substituted for the other.",
    "Queue residence is byte-weighted for bytes accepted during measurement, with first transmission observed through drain; admission waiting is call-weighted for calls started during measurement. Histograms use 125 µs bins. p95/p99 scalars are bin upper bounds; recorded maxima are exact. Lower/upper quantile bounds remain in JSON/CSV.",
    "The residence endpoint is the timestamp immediately before the first successful nonblocking socket send call, counted only after success. It does not measure kernel completion or physical wire departure.",
    "Probe injection timestamps precede native queue admission, so reported RTT includes admission blocking; scheduled RTT additionally includes owner-loop scheduling delay.",
    "A histogram quantile upper bound can exceed the exact maximum when all values lie below the end of that bin. This is a valid bound, not an inverted measured quantile.",
    "Timing-valid COMPLETE comparisons can remain provisional. INVALID_HARNESS and SUT-failed trials remain diagnostic; INVALID_ARTIFACT metrics are withheld from paired calculations.",
    "Native retransmission/drop/peak telemetry is whole-trial unless the recorded observer scope explicitly states otherwise. Network injected loss counts are separate from native negative ACKVEC observations.",
    "No TLS, RDP setup, Windows, rendering, or end-to-end desktop latency result is implied.",
]


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def stats(values, total=None):
    vals = [v for v in values if finite(v)]
    return {"n": len(vals), "missing": None if total is None else total - len(vals),
            "median": statistics.median(vals) if vals else None,
            "min": min(vals) if vals else None, "max": max(vals) if vals else None}


def read(path):
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def flatten_numbers(obj, prefix=""):
    result = {}
    if isinstance(obj, dict):
        for key, value in obj.items():
            name = f"{prefix}.{key}" if prefix else str(key)
            if finite(value):
                result[name] = value
            elif isinstance(value, dict):
                result.update(flatten_numbers(value, name))
    return result


def eligible(row, subset):
    if subset == "artifact_intact_timing_valid_complete":
        return row["artifact_intact_timing_valid_complete"]
    if subset == "validated_lab_complete_only":
        return row["validated_lab_complete"]
    return row["validity"] != "INVALID_ARTIFACT"


def enrich(row):
    # The audit performed by aggregate_campaign covers every sealed source file.
    result = dict(row)
    folder = Path(row["artifact_directory"]) if row.get("artifact_directory") else None
    manifest = read(folder / "manifest.json") if folder else {}
    result["manifest"] = {k: manifest.get(k) for k in (
        "source_commit", "binary_sha256", "build_metadata", "profile", "configuration",
        "effective_variant_environment", "effective_peer_environment", "scope")}
    result["invariants"] = manifest.get("invariants", {})
    result["metrics"] = dict(row.get("metrics", {}))
    result["endpoints"] = {}
    result["probe_accounting"] = {}
    for index, side in enumerate(("client", "server")):
        endpoint = read(folder / f"endpoint-{index}.result.json") if folder else {}
        result["endpoints"][side] = {key: endpoint.get(key) for key in (
            "completed", "error", "native_stats", "send_call_ms", "send_admission_ms",
            "queue_residence_ms", "send_queue_residence_ms", "observer_scope",
            "incomplete_record_bytes", "cpu_total_s", "measurement_s")}
        queue = endpoint.get("queue_stats")
        if isinstance(queue, dict):
            result["endpoints"][side]["queue_stats"] = {key: queue.get(key) for key in (
                "version", "scope", "valid", "reasons", "clock_alignment", "fields", "summaries")}
            result["endpoints"][side]["queue_histogram_artifact"] = str(folder / f"endpoint-{index}.result.json")
        # Preserve telemetry structure and flattened numeric values, without
        # guessing native histogram semantics or converting unknowns to zero.
        for key in ("native_stats", "send_admission_ms", "queue_residence_ms", "send_queue_residence_ms"):
            result["metrics"].update(flatten_numbers(endpoint.get(key), f"{side}.{key}"))
        result["probe_accounting"][side] = {
            key: row.get("metrics", {}).get(f"{side}.{key}") for key in (
                "probes_expected", "probes_injected", "probes_observed",
                "probes_not_injected", "probe_timeouts")}
    shaper = read(folder / "shaper.json") if folder else {}
    result["shaper"] = shaper
    for direction in ("c2s", "s2c"):
        values = shaper.get(direction)
        if isinstance(values, dict):
            # Counter keys omitted by Counter() mean observed zero. Only do
            # this when a whole-direction artifact exists; missing is unknown.
            for key in ("random_drops", "queue_drops", "blackout_drops", "socket_send_errors"):
                result["metrics"][f"shaper.{direction}.{key}"] = values.get(key, 0)
            result["metrics"].update(flatten_numbers(values, f"shaper.{direction}"))
    env = manifest.get("effective_variant_environment") or manifest.get("variant_environment") or {}
    cap = env.get("RDP_BENCH_SEND_QUEUE_BYTES")
    if cap is not None:
        try:
            result["queue_cap_bytes"] = int(cap)
        except (TypeError, ValueError):
            result["queue_cap_bytes"] = None
    else:
        result["queue_cap_bytes"] = None
    return result


def metric_names(rows):
    names = set(CORE_METRICS)
    for row in rows:
        names.update(name for name in row["metrics"] if any(token in name for token in (
            "queue", "admission", "send_call", "retransmission", "loss_reports", "drop", "lateness", "window", "retry", "probes_", "probe_timeouts")))
    return sorted(names)


def summarize(rows, names):
    result = {"trial_count": len(rows), "trial_ids": [r["trial_id"] for r in rows],
              "metrics": {name: stats((r["metrics"].get(name) for r in rows), len(rows)) for name in names},
              "counts": {"integrity_pass": sum(r["integrity_verified"] is True for r in rows),
                         "integrity_fail": sum(r["integrity_verified"] is False for r in rows),
                         "sut_fail": sum(r["sut_outcome"] == "FAIL" for r in rows),
                         "harness_invalid": sum(r["original_validity"] == "INVALID_HARNESS" for r in rows),
                         "artifact_invalid": sum(r["validity"] == "INVALID_ARTIFACT" for r in rows)},
              "counter_totals": {}}
    for name in names:
        if any(token in name for token in ("retransmissions", ".random_drops", ".queue_drops", ".blackout_drops", ".socket_send_errors", "probes_", "probe_timeouts", "capacity_blocked_calls", ".queue_admission_calls", "clock_error_count", "segment_overflow_count", "counter_overflow_count")):
            values = [r["metrics"].get(name) for r in rows]
            known = [v for v in values if finite(v)]
            result["counter_totals"][name] = {"sum": sum(known) if known else None,
                                               "known_trials": len(known), "unknown_trials": len(rows) - len(known)}
    return result


def pair(rows, baseline, candidate, names):
    keyed = defaultdict(dict)
    for row in rows:
        if row["variant"] not in (baseline, candidate):
            continue
        key = (row["case_id"], row["comparison_key"], row["seed"])
        if row["variant"] in keyed[key]:
            raise ValueError(f"duplicate variant/configuration/seed: {key} {row['variant']}")
        keyed[key][row["variant"]] = row
    grouped = defaultdict(list)
    unpaired = []
    for key, variants in keyed.items():
        if baseline not in variants or candidate not in variants:
            unpaired.extend({"trial_id": r["trial_id"], "variant": r["variant"], "seed": key[2],
                             "case_id": key[0], "comparison_key": key[1],
                             "reason": "missing identical case/comparison_key/seed counterpart"} for r in variants.values())
        else:
            grouped[key[:2]].append((key[2], variants[baseline], variants[candidate]))
    output = []
    for (case, comparison_key), pairs in sorted(grouped.items()):
        for subset in SUBSETS:
            for name in names:
                records, excluded = [], []
                for seed, a, b in sorted(pairs):
                    av, bv = a["metrics"].get(name), b["metrics"].get(name)
                    if not eligible(a, subset) or not eligible(b, subset) or not finite(av) or not finite(bv):
                        excluded.append({"seed": seed, "baseline_trial": a["trial_id"], "candidate_trial": b["trial_id"],
                                         "baseline_validity": a["validity"], "candidate_validity": b["validity"],
                                         "baseline_outcome": a["sut_outcome"], "candidate_outcome": b["sut_outcome"],
                                         "reason": "subset ineligibility or metric absent/nonfinite"})
                        continue
                    records.append({"seed": seed, "baseline_trial": a["trial_id"], "candidate_trial": b["trial_id"],
                                    "baseline": av, "candidate": bv, "delta": bv - av,
                                    "ratio": bv / av if av else None})
                output.append({"baseline_variant": baseline, "candidate_variant": candidate,
                               "case_id": case, "comparison_key": comparison_key, "subset": subset, "metric": name,
                               "pairs": records, "excluded_pairs": excluded,
                               "baseline": stats(r["baseline"] for r in records),
                               "candidate": stats(r["candidate"] for r in records),
                               "paired_delta": stats(r["delta"] for r in records),
                               "paired_ratio": stats(r["ratio"] for r in records)})
    return output, unpaired


def fmt(value):
    return "—" if value is None else f"{value:.3f}"


def fmt_stats(value):
    return f"{fmt(value['median'])} [{fmt(value['min'])}–{fmt(value['max'])}]"


def invariants_audit(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[(row["case_id"], row["comparison_key"])].append(row)
    output = []
    for (case, key), group in sorted(groups.items()):
        binaries = sorted({r.get("binary_sha256") for r in group if r.get("binary_sha256")})
        peers = sorted({r["invariants"].get("peer_sha256") for r in group if r["invariants"].get("peer_sha256")})
        configs = {json.dumps(r["invariants"], sort_keys=True) for r in group}
        configurations = [{"trial_id": r["trial_id"], "variant": r["variant"],
                           "configured_dut_cap_bytes": r["queue_cap_bytes"],
                           "actual_dut_cap_bytes": r["metrics"].get("client.queue_queue_limit_bytes"),
                           "actual_peer_cap_bytes": r["metrics"].get("server.queue_queue_limit_bytes"),
                           "dut_window_packets": r["metrics"].get("client.queue_effective_window_packets"),
                           "peer_window_packets": r["metrics"].get("server.queue_effective_window_packets"),
                           "dut_observer_enabled": r["metrics"].get("client.queue_observer_enabled"),
                           "peer_observer_enabled": r["metrics"].get("server.queue_observer_enabled"),
                           "cap_matches_config": r["queue_cap_bytes"] is not None
                               and r["queue_cap_bytes"] == r["metrics"].get("client.queue_queue_limit_bytes"),
                           "packet_window_held_at_31": r["metrics"].get("client.queue_effective_window_packets") == 31
                               and r["metrics"].get("server.queue_effective_window_packets") == 31}
                          for r in group]
        output.append({"case_id": case, "comparison_key": key, "variants": sorted({r["variant"] for r in group}),
                       "native_binary_sha256s": binaries, "fixed_peer_sha256s": peers,
                       "identical_invariants": len(configs) == 1,
                       "same_native_binary": len(binaries) == 1,
                       "same_fixed_peer_binary": len(peers) == 1, "actual_configurations": configurations})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Directory containing the sealed sweep campaigns")
    parser.add_argument("--campaign", default="campaign")
    parser.add_argument("--baseline", default="q32")
    parser.add_argument("--candidates", nargs="+", default=["q48", "q64", "q128"])
    parser.add_argument("--output-prefix", default="analysis")
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    campaign = root / args.campaign
    report = aggregate_campaign(campaign)
    write_report(report, root / f"{args.campaign}-report")
    rows = [enrich(r) for r in report["trials"]]
    names = metric_names(rows)
    grouped = defaultdict(list)
    for row in rows:
        grouped[(row["case_id"], row["variant"], row["comparison_key"])].append(row)
    groups = []
    for (case, variant, key), members in sorted(grouped.items()):
        groups.append({"case_id": case, "variant": variant, "comparison_key": key,
                       "profile_id": members[0].get("profile_id"), "workload": members[0].get("workload"),
                       "queue_cap_bytes": sorted({r["queue_cap_bytes"] for r in members if r["queue_cap_bytes"] is not None}),
                       "subsets": {subset: summarize([r for r in members if eligible(r, subset)] if subset != "all_observed_diagnostic" else members, names) for subset in SUBSETS}})
    comparisons, unpaired = [], []
    for candidate in args.candidates:
        p, u = pair(rows, args.baseline, candidate, names)
        comparisons.extend(p)
        unpaired.extend(u)
    failures = [{"trial_id": r["trial_id"], "variant": r["variant"], "case_id": r["case_id"], "seed": r["seed"],
                 "kind": kind, "reason": reason}
                for r in rows for kind in ("harness_reasons", "artifact_reasons", "sut_failures", "integrity_failures")
                for reason in r.get(kind, [])]
    output = {"version": 1, "campaign": str(campaign), "scope": "provisional_local_fixed_window_queue_cap_sweep",
              "baseline_variant": args.baseline, "candidate_variants": args.candidates,
              "notes": NOTES, "counts": report["counts"], "groups": groups, "comparisons": comparisons,
              "unpaired": unpaired, "invariants_audit": invariants_audit(rows), "failures": failures, "trials": rows}
    prefix = root / args.output_prefix
    prefix.with_suffix(".json").write_text(json.dumps(output, indent=2) + "\n")
    with prefix.with_name(prefix.name + "-trials.csv").open("w", newline="") as stream:
        fields = ["trial_id", "case_id", "variant", "profile_id", "workload", "seed", "queue_cap_bytes",
                  "comparison_key", "validity", "sut_outcome", "integrity_verified", *names]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            data = {key: row.get(key) for key in fields[:11]}
            data.update({name: row["metrics"].get(name) for name in names})
            writer.writerow(data)
    with prefix.with_name(prefix.name + "-groups.csv").open("w", newline="") as stream:
        fields = ["case_id", "variant", "comparison_key", "subset", "trial_count", "metric", "n", "missing", "median", "min", "max"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for group in groups:
            for subset, values in group["subsets"].items():
                for metric, summary in values["metrics"].items():
                    writer.writerow({**{key: group[key] for key in fields[:3]}, "subset": subset,
                                     "trial_count": values["trial_count"], "metric": metric, **summary})
    lines = ["# Fixed-window UDP2 queue-cap sweep", "", *[f"- {n}" for n in NOTES], ""]
    counts = report["counts"]
    lines += [f"Trials: {counts['observed_trials']}/{counts['expected_trials']} observed; "
              f"{counts['artifact_intact_timing_valid_complete_trials']} timing-valid COMPLETE; "
              f"{counts['validated_lab_complete_trials']} accepted-calibration COMPLETE; "
              f"{counts['sut_failed_trials']} SUT failures; {counts['invalid_harness_trials']} harness-invalid; "
              f"{counts['invalid_artifact_trials']} artifact-invalid; {counts['integrity_failed_trials']} integrity failures.", ""]
    display = list(CORE_METRICS)
    display.extend(name for name in names if "queue" in name and name.endswith((".p95", ".p99", ".max")) and name not in display)
    for subset in SUBSETS:
        lines += [f"## {subset}", "", "Median [min–max] across whole trials; goodput Mbit/s and latency ms.", "",
                  "| Case | Cap | N | " + " | ".join(display) + " |",
                  "|---|---|---:|" + "---:|" * len(display)]
        for group in groups:
            values = group["subsets"][subset]
            lines.append(f"| {group['case_id']} | {group['variant']} | {values['trial_count']} | " +
                         " | ".join(fmt_stats(values["metrics"][name]) for name in display) + " |")
        lines.append("")
    lines += ["## Strict paired timing-valid COMPLETE differences", "",
              f"| Case | Candidate / {args.baseline} | Metric | Paired seeds | {args.baseline} | Candidate | Candidate − {args.baseline} | Candidate / {args.baseline} |",
              "|---|---|---|---:|---:|---:|---:|---:|"]
    for comp in comparisons:
        if comp["subset"] != "artifact_intact_timing_valid_complete" or comp["metric"] not in display or not comp["pairs"]:
            continue
        lines.append(f"| {comp['case_id']} | {comp['candidate_variant']} | {comp['metric']} | {len(comp['pairs'])} | " +
                     " | ".join(fmt_stats(comp[key]) for key in ("baseline", "candidate", "paired_delta", "paired_ratio")) + " |")
    lines += ["", "## Failures and invalidity", ""]
    if failures:
        lines.extend(f"- {f['trial_id']}: {f['kind']}: {f['reason']}" for f in failures)
    else:
        lines.append("No SUT, harness, artifact, or integrity failures recorded.")
    lines += ["", "## Counter totals and queue telemetry", "",
              "| Case | Cap | Missed / timeout probes | Retransmissions DUT / peer | Random / queue drops C→S | Random / queue drops S→C | Blocked admissions DUT | Owned / unsent / in-flight bytes peak | In-flight packet peak |",
              "|---|---|---|---|---|---|---:|---|---:|"]
    def total(values, name):
        data = values["counter_totals"].get(name, {})
        result = data.get("sum")
        return "unknown" if result is None else str(result) + (f" (+{data['unknown_trials']} unknown trials)" if data.get("unknown_trials") else "")
    def peak(values, name):
        data = values["metrics"].get(name, {})
        result = data.get("max")
        return "unknown" if result is None else str(result)
    for group in groups:
        values = group["subsets"]["all_observed_diagnostic"]
        probes = " / ".join(total(values, f"client.{name}") for name in ("probes_not_injected", "probe_timeouts"))
        retries = " / ".join(total(values, f"{side}.native_stats.retransmissions") for side in ("client", "server"))
        drops = [" / ".join(total(values, f"shaper.{direction}.{name}") for name in ("random_drops", "queue_drops")) for direction in ("c2s", "s2c")]
        queue_peaks = " / ".join(peak(values, f"client.queue_peak_{name}_bytes") for name in ("owned", "unsent", "inflight"))
        lines.append(f"| {group['case_id']} | {group['variant']} | {probes} | {retries} | {drops[0]} | {drops[1]} | "
                     f"{total(values, 'client.queue_capacity_blocked_calls')} | {queue_peaks} | {peak(values, 'client.queue_peak_inflight_packets')} |")
    lines += ["",
              "The JSON retains native telemetry, complete shaper summaries, all endpoint outcomes, all probe counts, "
              "pair exclusions, and every trial. Long-form groups CSV includes native queue/admission/window/retry/drop "
              "metrics with missing-value coverage. Missing artifacts remain unknown, not zero.", ""]
    prefix.with_suffix(".md").write_text("\n".join(lines))
    print(json.dumps({"analysis": str(prefix.with_suffix('.md')), "counts": counts,
                      "failure_reasons": len(failures), "unpaired_records": len(unpaired)}))


if __name__ == "__main__":
    main()
