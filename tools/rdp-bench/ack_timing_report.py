#!/usr/bin/env python3
"""Audit a 48/64 KiB ACK-feedback campaign and export portable summaries.

This reads sealed trials only, writes outside every campaign, and never pools
trial percentiles. ACK-only relay deadline offsets are not receiver batching.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path

from baseline_report import aggregate_campaign, trial_counts
from queue_report import (CORE_METRICS, SUBSETS, eligible, enrich, finite, fmt_stats,
                          pair, read, summarize)
from rdpbench.common import timing_summary
from rdpbench.feedback import KINDS, SELECTED_KINDS

PHASE_COUNTS = {"smoke": 8, "baseline": 40, "ack-timing": 48, "control": 12}
DIRECTIONS = ("c2s", "s2c")
PROBE_METRICS = tuple(f"{side}.{name}" for side in ("client", "server")
                      for name in ("probes_expected", "probes_injected", "probes_observed",
                                   "probes_not_injected", "probe_timeouts"))
EXTRA_METRICS = tuple(f"{side}.{name}" for side in ("client", "server") for name in (
    "native_stats.retransmissions", "queue_peak_owned_bytes", "queue_peak_inflight_packets",
    "queue_effective_window_packets", "queue_peer_log_window", "queue_capacity_blocked_calls"))
ACK_METRICS = tuple(f"{direction}.ack_feedback.{name}" for direction in DIRECTIONS
                    for name in ("delivered_datagrams", "requested_delay_ms",
                                 "added_delivery_ms.p95", "added_delivery_ms.p99",
                                 "scheduler_overshoot_ms.p95", "scheduler_overshoot_ms.p99"))
METRICS = tuple(dict.fromkeys((*CORE_METRICS, *PROBE_METRICS, *EXTRA_METRICS, *ACK_METRICS)))
DISPLAY_METRICS = ("c2s.verified_goodput_mbps", "client.probe_rtt_ms.p95",
                   "client.probe_rtt_ms.p99", "client.queue_residence_ms.p95",
                   "client.queue_residence_ms.p99")
NOTES = [
    "Raw native UDP2 transport on a local Python relay; no TLS, Windows peer, rendering or desktop latency is implied.",
    "Calibration can remain provisional. Timing-valid COMPLETE trials and all-observed diagnostics are separate, with all failures retained.",
    "Goodput requires complete byte/hash/order integrity verification after drain. Invalid artifacts contribute no numeric samples.",
    "Each summary is the median/minimum/maximum of whole-trial statistics; per-trial p95/p99 values are never pooled into a combined percentile.",
    "Observed-probe percentiles exclude missing probes; expected/injected/observed/missed/timeouts are reported separately.",
    "ACK feedback is an added relay deadline offset on parsed ACK/ACKVEC-only datagrams. DATA with piggybacked ACKs is unchanged. This is not receiver ACK batching.",
    "Every recorded ACK dispatch is checked against the requested offset, delivered count and reported timing summaries. Nonzero treatments must actually dispatch a selected ACK in the treated direction.",
    "ACK classification precedes network drops and duplication. Selected ACK drops are not individually identifiable; scheduled ACK counts are checked against truthful bounds from all-direction drop/duplicate counts.",
    "48/64 KiB comparisons require identical phase, case, comparison key and seed. Failed or invalid pairs remain explicitly excluded from strict comparisons.",
    "Queue-residence histogram p95/p99 values are bin upper bounds; queue admission and native API-call duration are distinct measurements.",
    "The lab peer retains LogWindow 5 and both effective packet windows remain 31; LogWindow 15 correctness is covered by native regression fixtures.",
]


def integer(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def same_number(actual, expected):
    return finite(actual) and math.isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-9)


def ack_ledger_audit(folder, treatment, shaper):
    """Recompute every ACK timing sample; never trust ledger totals alone."""
    if treatment is None:
        return {"required": False, "valid": True, "directions": {}, "reasons": []}
    reasons, records = [], {direction: [] for direction in DIRECTIONS}
    if not isinstance(treatment, dict) or set(treatment) != {"c2s_delay_ms", "s2c_delay_ms"}:
        return {"required": True, "valid": False, "directions": {},
                "reasons": ["invalid ACK treatment configuration"]}
    path = folder / "ack-feedback.jsonl"
    try:
        with path.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                try:
                    event = json.loads(line)
                except ValueError:
                    reasons.append(f"ACK ledger line {line_number}: invalid JSON")
                    continue
                if not isinstance(event, dict) or event.get("direction") not in DIRECTIONS:
                    reasons.append(f"ACK ledger line {line_number}: invalid direction or record")
                    continue
                records[event["direction"]].append(event)
    except (OSError, UnicodeError) as error:
        reasons.append(f"ACK ledger unavailable: {type(error).__name__}")
    output = {}
    for direction in DIRECTIONS:
        local_reasons, added, overshoot = [], [], []
        delay = treatment.get(direction + "_delay_ms")
        if not finite(delay) or not 0 <= delay <= 1000:
            reasons.append(f"{direction}: invalid requested ACK delay")
            continue
        delay_ns = int(delay * 1e6)
        direction_summary = shaper.get(direction)
        if not isinstance(direction_summary, dict):
            reasons.append(f"{direction}: malformed or missing direction summary")
            continue
        feedback = direction_summary.get("ack_feedback")
        if not isinstance(feedback, dict):
            reasons.append(f"{direction}: ACK feedback summary missing")
            continue
        # Classification occurs once per arrival, before offer() applies losses
        # or duplication. Scheduling counts every selected surviving copy.
        classification = feedback.get("classification")
        valid_classification = isinstance(classification, dict) and set(classification) == set(KINDS)
        if not valid_classification or not all(integer(value) for value in classification.values()):
            local_reasons.append("invalid ACK classification counters")
            valid_classification = False
        count_keys = ("classified_datagrams", "selected_datagrams", "scheduled_datagrams", "delivered_datagrams")
        valid_counts = all(integer(feedback.get(key)) for key in count_keys)
        if not valid_counts:
            local_reasons.append("invalid ACK classified/selected/scheduled/delivered counters")
        network_keys = ("received_datagrams", "delivered_datagrams", "blackout_drops", "queue_drops",
                        "random_drops", "duplicates", "socket_send_errors")
        network_counts = {key: direction_summary.get(key, 0) for key in network_keys}
        valid_network = all(integer(value) for value in network_counts.values())
        if not valid_network:
            local_reasons.append("invalid network arrival/delivery/drop/duplicate/error counters")
        pending = shaper.get("pending_on_close")
        if not integer(pending):
            local_reasons.append("invalid or missing relay pending count")
        if valid_classification and valid_counts:
            if sum(classification.values()) != feedback["classified_datagrams"]:
                local_reasons.append("classification sum differs from classified_datagrams")
            if sum(classification[kind] for kind in SELECTED_KINDS) != feedback["selected_datagrams"]:
                local_reasons.append("ACK/ACKVEC classification sum differs from selected_datagrams")
            if classification["unclassified"]:
                local_reasons.append("ACK classification contains unclassified native datagrams")
        if valid_counts and valid_network:
            if feedback["classified_datagrams"] != network_counts["received_datagrams"]:
                local_reasons.append("classified_datagrams differs from received_datagrams")
            selected, scheduled, delivered = (feedback[key] for key in count_keys[1:])
            drops = sum(network_counts[key] for key in ("blackout_drops", "queue_drops", "random_drops"))
            duplicates = network_counts["duplicates"]
            if drops > network_counts["received_datagrams"]:
                local_reasons.append("network drops exceed received_datagrams")
            if duplicates > network_counts["received_datagrams"] - drops:
                local_reasons.append("network duplicates exceed surviving received_datagrams")
            lower, upper = max(0, selected - drops), selected + duplicates
            if not lower <= scheduled <= upper:
                local_reasons.append("scheduled_datagrams outside selected/drop/duplicate bounds")
            all_scheduled = network_counts["received_datagrams"] - drops + duplicates
            if scheduled > all_scheduled or network_counts["delivered_datagrams"] > all_scheduled:
                local_reasons.append("ACK or network deliveries exceed total scheduled datagrams")
            if delivered > scheduled or delivered > network_counts["delivered_datagrams"]:
                local_reasons.append("ACK delivered_datagrams exceeds scheduled or network deliveries")
            if integer(pending) and scheduled - delivered > pending + network_counts["socket_send_errors"]:
                local_reasons.append("undelivered scheduled ACKs exceed relay pending/socket-error bounds")
        for event_number, event in enumerate(records[direction], 1):
            valid_times = all(integer(event.get(key)) for key in (
                "network_due_ns", "feedback_due_ns", "actual_dispatch_ns"))
            if event.get("kind") not in ("ack_only", "ackvec_only") or not integer(event.get("udp_bytes")) or event.get("udp_bytes", 0) == 0:
                local_reasons.append(f"event {event_number}: invalid selected packet kind/size")
            if not valid_times:
                local_reasons.append(f"event {event_number}: invalid timestamps")
                continue
            network, due, actual = (event[key] for key in (
                "network_due_ns", "feedback_due_ns", "actual_dispatch_ns"))
            if due - network != delay_ns:
                local_reasons.append(f"event {event_number}: deadline offset differs from requested delay")
            if actual < due:
                local_reasons.append(f"event {event_number}: dispatched before feedback deadline")
            added.append(max(0, actual - network) / 1e6)
            overshoot.append(max(0, actual - network - delay_ns) / 1e6)
        count = len(records[direction])
        if valid_classification and valid_network:
            delivered_kinds = Counter(event.get("kind") for event in records[direction])
            for kind in SELECTED_KINDS:
                if delivered_kinds[kind] > classification[kind] + network_counts["duplicates"]:
                    local_reasons.append(f"delivered {kind} records exceed classified/duplicate bounds")
        if not same_number(feedback.get("requested_delay_ms"), delay):
            local_reasons.append("summary requested delay differs from configuration")
        for key in ("delivered_datagrams", "ledger_events"):
            if not integer(feedback.get(key)) or feedback[key] != count:
                local_reasons.append(f"summary {key} differs from ACK ledger count")
        if feedback.get("ledger_overflow_datagrams") != 0:
            local_reasons.append("ACK ledger overflow is nonzero or unavailable")
        if delay > 0 and count == 0:
            local_reasons.append("nonzero treatment dispatched no selected ACKs")
        computed = {}
        for name, values in (("added_delivery_ms", added), ("scheduler_overshoot_ms", overshoot)):
            expected, saved = timing_summary(values), feedback.get(name, {})
            computed[name] = expected
            if not isinstance(saved, dict):
                local_reasons.append(f"summary {name} missing")
                continue
            for key, value in expected.items():
                if value is None:
                    matches = saved.get(key) is None
                elif key == "count":
                    matches = integer(saved.get(key)) and saved[key] == value
                else:
                    matches = same_number(saved.get(key), value)
                if not matches:
                    local_reasons.append(f"summary {name}.{key} differs from ACK ledger")
            for key in ("sample_count", "delivered_count"):
                if not integer(saved.get(key)) or saved[key] != count:
                    local_reasons.append(f"summary {name}.{key} differs from ACK ledger count")
            if saved.get("sampled") is not False:
                local_reasons.append(f"summary {name} is sampled or sampling status unavailable")
        output[direction] = {"requested_delay_ms": delay, "ledger_events": count,
                             "valid": not local_reasons, "computed_timings": computed}
        reasons.extend(f"{direction}: {reason}" for reason in local_reasons)
    return {"required": True, "valid": not reasons, "directions": output,
            "reasons": list(dict.fromkeys(reasons))}


def compact_trial(row, phase, root):
    """Discard bulky endpoint/histogram structures but retain failure evidence."""
    result = enrich(row)
    folder = Path(result["artifact_directory"])
    treatment = result["invariants"].get("ack_feedback")
    audit = ack_ledger_audit(folder, treatment, result["shaper"])
    artifact_ok = result["validity"] != "INVALID_ARTIFACT"
    if not audit["valid"]:
        result["artifact_intact_timing_valid_complete"] = False
        result["validated_lab_complete"] = False
    keys = ("trial_id", "case_id", "variant", "comparison_key", "seed", "profile_id", "workload",
            "validity", "original_validity", "sut_outcome", "integrity_verified", "queue_cap_bytes",
            "artifact_intact_timing_valid_complete", "validated_lab_complete", "harness_reasons",
            "artifact_reasons", "sut_failures", "integrity_failures", "calibration_notes", "binary_sha256")
    compact = {key: result.get(key) for key in keys}
    compact.update(phase=phase, artifact_directory=folder.relative_to(root).as_posix(),
                   artifact_present=(folder / "summary.json").is_file(),
                   ack_feedback=treatment, ack_ledger_audit=audit,
                   source_commit=result["manifest"].get("source_commit"),
                   fixed_peer_sha256=result["invariants"].get("peer_sha256"),
                   metrics={name: result["metrics"].get(name) if artifact_ok else None for name in METRICS})
    return compact


def expected_slots(root, phase, plan):
    config = plan.get("configurations", {}).get(phase) or read(root / f"{phase}-suite.json")
    if not config:
        return None
    caps = plan.get("caps_kib", [48, 64])
    return {(case["id"], f"q{cap}", seed) for case in config.get("cases", [])
            for cap in caps for seed in config.get("seeds", [])}


def build_report(root, phases=None):
    root = Path(root).expanduser().resolve()
    plan = read(root / "ack-sweep-plan.json")
    phases = phases or ["smoke", "baseline", "ack-timing"] + (["control"] if (root / "control").exists() else [])
    rows, phase_results, groups, comparisons, unpaired, failures = [], [], [], [], [], []
    for phase in phases:
        path = root / phase
        expected = expected_slots(root, phase, plan)
        expected_count = len(expected) if expected is not None else PHASE_COUNTS.get(phase)
        phase_rows, phase_errors = [], []
        if not (path / "campaign.json").is_file():
            phase_errors.append("completed campaign.json missing")
        else:
            report = aggregate_campaign(path)
            phase_rows = [compact_trial(row, phase, root) for row in report["trials"]]
            if len(phase_rows) != expected_count:
                phase_errors.append(f"collected {len(phase_rows)} trials; expected {expected_count}")
        actual_slots = [(row["case_id"], row["variant"], row["seed"]) for row in phase_rows]
        duplicate_slots = [list(slot) for slot, count in Counter(actual_slots).items() if count > 1]
        if duplicate_slots:
            phase_errors.append("duplicate case/variant/seed trial slots")
        missing_slots = sorted(expected - set(actual_slots)) if expected is not None else []
        unexpected_slots = sorted(set(actual_slots) - expected) if expected is not None else []
        if missing_slots:
            phase_errors.append("expected case/variant/seed slots missing")
        if unexpected_slots:
            phase_errors.append("unexpected case/variant/seed slots recorded")
        counts = trial_counts(phase_rows, expected_count)
        counts.update(present_trial_artifacts=sum(row["artifact_present"] for row in phase_rows),
                      ack_ledger_invalid_trials=sum(not row["ack_ledger_audit"]["valid"] for row in phase_rows))
        phase_results.append({"phase": phase, "counts": counts, "errors": phase_errors,
                              "missing_expected_slots": [list(slot) for slot in missing_slots],
                              "unexpected_slots": [list(slot) for slot in unexpected_slots],
                              "duplicate_slots": duplicate_slots,
                              "missing_trial_artifacts": [row["trial_id"] for row in phase_rows if not row["artifact_present"]]})
        failures.extend({"phase": phase, "trial_id": None, "kind": "campaign", "reason": reason} for reason in phase_errors)
        grouped = defaultdict(list)
        for row in phase_rows:
            grouped[(row["case_id"], row["variant"], row["comparison_key"])].append(row)
            for kind in ("harness_reasons", "artifact_reasons", "sut_failures", "integrity_failures"):
                failures.extend({"phase": phase, "trial_id": row["trial_id"], "kind": kind, "reason": reason}
                                for reason in row.get(kind, []) or [])
            failures.extend({"phase": phase, "trial_id": row["trial_id"], "kind": "ack_ledger_audit", "reason": reason}
                            for reason in row["ack_ledger_audit"]["reasons"])
        for (case, variant, key), members in sorted(grouped.items()):
            groups.append({"phase": phase, "case_id": case, "variant": variant, "comparison_key": key,
                           "profile_id": members[0]["profile_id"], "workload": members[0]["workload"],
                           "queue_cap_bytes": members[0]["queue_cap_bytes"], "ack_feedback": members[0]["ack_feedback"],
                           "subsets": {subset: summarize(members if subset == "all_observed_diagnostic" else
                                                         [row for row in members if eligible(row, subset)], METRICS)
                                       for subset in SUBSETS}})
        p, u = pair(phase_rows, "q48", "q64", DISPLAY_METRICS)
        comparisons.extend(dict(item, phase=phase) for item in p)
        unpaired.extend(dict(item, phase=phase) for item in u)
        rows.extend(phase_rows)
    return {"version": 1, "scope": "provisional_local_udp2_ack_feedback_queue_sweep", "notes": NOTES,
            "provenance": {"binary_sha256": plan.get("binary_sha256"), "driver_sha256": plan.get("driver_sha256"),
                           "profile_source_sha256": plan.get("profile_source_sha256"), "source_hashes": plan.get("source_hashes"),
                           "caps_kib": plan.get("caps_kib", [48, 64]), "fixed_peer_environment": plan.get("fixed_peer_environment")},
            "phases": phase_results, "groups": groups, "comparisons": comparisons,
            "unpaired": unpaired, "failures": failures, "trials": rows}


def markdown(report):
    lines = ["# UDP2 peer-window and ACK-feedback campaign", "", *[f"- {note}" for note in report["notes"]], "",
             "| Phase | Present / expected | Timing-valid COMPLETE | SUT failures | Artifact invalid | ACK audit invalid |",
             "|---|---:|---:|---:|---:|---:|"]
    for phase in report["phases"]:
        count = phase["counts"]
        lines.append(f"| {phase['phase']} | {count['present_trial_artifacts']} / {count['expected_trials']} | "
                     f"{count['artifact_intact_timing_valid_complete_trials']} | {count['sut_failed_trials']} | "
                     f"{count['invalid_artifact_trials']} | {count['ack_ledger_invalid_trials']} |")
    for subset in ("artifact_intact_timing_valid_complete", "all_observed_diagnostic"):
        lines += ["", f"## {subset}", "", "Median [min–max] of whole-trial statistics; goodput Mbit/s, latency ms.", "",
                  "| Phase | Case | Cap | N | " + " | ".join(DISPLAY_METRICS) + " | Missed / timeout probes |",
                  "|---|---|---|---:|" + "---:|" * len(DISPLAY_METRICS) + "---:|"]
        for group in report["groups"]:
            summary = group["subsets"][subset]
            probe_totals = [summary["counter_totals"].get("client." + name, {}).get("sum")
                            for name in ("probes_not_injected", "probe_timeouts")]
            lines.append(f"| {group['phase']} | {group['case_id']} | {group['variant']} | {summary['trial_count']} | " +
                         " | ".join(fmt_stats(summary["metrics"][name]) for name in DISPLAY_METRICS) + " | " +
                         " / ".join("unknown" if value is None else str(value) for value in probe_totals) + " |")
    lines += ["", "## Strict paired 64 KiB − 48 KiB differences", "",
              "| Phase | Case | Metric | Pairs | Paired delta [min–max] |", "|---|---|---|---:|---:|"]
    for item in report["comparisons"]:
        if item["subset"] == "artifact_intact_timing_valid_complete":
            lines.append(f"| {item['phase']} | {item['case_id']} | {item['metric']} | "
                         f"{len(item['pairs'])} | {fmt_stats(item['paired_delta'])} |")
    lines += ["", "## Failures and invalidity", ""]
    lines.extend(f"- {item['phase']}/{item['trial_id'] or 'campaign'}: {item['kind']}: {item['reason']}"
                 for item in report["failures"])
    if not report["failures"]:
        lines.append("No campaign, artifact, SUT, integrity, harness or ACK-ledger failures recorded.")
    return "\n".join(lines) + "\n"


def write_report(report, output, root):
    output = Path(output).expanduser().resolve()
    root = Path(root).expanduser().resolve()
    for phase in PHASE_COUNTS:
        campaign = root / phase
        if output.is_relative_to(campaign):
            raise ValueError("report output must be outside every sealed campaign")
    targets = [output.with_suffix(suffix).resolve() for suffix in (".json", ".md", ".csv")]
    protected_files = {root / name for name in (
        "ack-sweep-plan.json", "ack-sweep-status.json", "variants.json", "peer-env.json",
        *(f"{phase}-suite.json" for phase in PHASE_COUNTS))}
    protected_trees = {root / name for name in (
        *PHASE_COUNTS, "source", "source-snapshot", "frozen-source", "binaries", "relay-calibration", "calibration")}
    tool_root = Path(__file__).resolve().parent
    protected_files.update(path.resolve() for path in tool_root.glob("*") if path.is_file())
    protected_trees.update(tool_root / name for name in ("rdpbench", "tests", "configs", "docs", "linux"))

    tree_fields = {"source_snapshot", "source_snapshot_directory", "campaign_source_snapshot", "snapshot_directory",
                   "frozen_source_directory", "build_directory", "binary_directory", "calibration_directory"}

    def declared_paths(value, field=None):
        # CMake options contain ordinary source/install/include directories,
        # including the whole checkout. They are not sealed artifact trees.
        if field == "build_options":
            return
        if isinstance(value, dict):
            for name, child in value.items():
                declared_paths(child, name)
        elif isinstance(value, list):
            for child in value:
                declared_paths(child, field)
        elif isinstance(value, str) and Path(value).is_absolute():
            path = Path(value).resolve()
            if path.is_dir() and field in tree_fields:
                protected_trees.add(path)
            elif path.is_file():
                protected_files.add(path)
                if path.suffix in (".dylib", ".so", ".dll", ".a"):
                    protected_trees.add(path.parent)

    declared_paths(read(root / "ack-sweep-plan.json"))
    declared_paths(read(root / "variants.json"))
    for phase in PHASE_COUNTS:
        for manifest in (root / phase).glob("*/manifest.json"):
            declared_paths(read(manifest).get("calibration"))
    protected_files = {path.resolve() for path in protected_files}
    for target in targets:
        if target in protected_files or any(target.is_relative_to(tree.resolve()) for tree in protected_trees):
            raise ValueError("report output would overwrite or enter campaign provenance, source, binary or calibration inputs")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.with_suffix(".json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    output.with_suffix(".md").write_text(markdown(report), encoding="utf-8")
    fields = ("phase", "case_id", "variant", "comparison_key", "subset", "trial_count", "metric", "n", "missing", "median", "min", "max")
    with output.with_suffix(".csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for group in report["groups"]:
            for subset, values in group["subsets"].items():
                for name, summary in values["metrics"].items():
                    writer.writerow({**{key: group[key] for key in fields[:4]}, "subset": subset,
                                     "trial_count": values["trial_count"], "metric": name, **summary})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="Output filename prefix, outside sealed campaigns")
    parser.add_argument("--phases", nargs="+", choices=tuple(PHASE_COUNTS), help="Defaults to all three required phases and control if present")
    args = parser.parse_args(argv)
    report = build_report(args.root, args.phases)
    write_report(report, args.output, args.root)
    print(json.dumps({"output": str(args.output), "phase_counts": {item["phase"]: item["counts"] for item in report["phases"]},
                      "failure_reasons": len(report["failures"])}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
