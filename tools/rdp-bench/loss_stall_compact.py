#!/usr/bin/env python3
"""Export a portable, compact loss-stall report from an already audited aggregate.

This exporter does not replace the SHA/native/relay ledger audits performed by
loss_stall_report.py. It reads only that report and optional small status files;
it never reopens endpoint, packet, native-event or sealed trial ledgers.
"""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path

DEFAULT_INPUT = "build/udp-loss-stalls-20261012/report/aggregate.json"
DEFAULT_OUTPUT = "tools/rdp-bench/results/loss-stalls-20261012"
SUBSETS = ("all_observed_diagnostic", "artifact_intact_timing_valid_complete")
PAIR_METRICS = (
    "c2s.verified_goodput_mbps", "client.probe_rtt_ms.p95", "client.probe_rtt_ms.p99",
    "client.probe_scheduled_ms.p95", "client.probe_scheduled_ms.p99",
    "client.send_call_ms.max", "client.queue_blocked_admission_ms.max",
    "client.queue_residence_ms.p99",
)
TRIAL_METRICS = (*PAIR_METRICS, "client.queue_residence_ms.p95", "client.queue_residence_ms.max",
                 "client.queue_admission_ms.max", "client.native_stats.retransmissions",
                 "shaper.c2s.lateness_ms.p99", "shaper.s2c.lateness_ms.p99",
                 "shaper.c2s.random_drops", "shaper.c2s.queue_drops",
                 "shaper.s2c.random_drops", "shaper.s2c.queue_drops")
PROBES = ("probes_expected", "probes_injected", "probes_observed", "probes_not_injected", "probe_timeouts")
UNKNOWN_INTERVALS = (
    "fixed peer ACK generation to relay offer; peer native ABI unavailable",
    "source socket send to relay offer except matched DATA drop",
    "host scheduling outside recorded calls and relay dispatches",
)
RECOVERY_FIELDS = (
    "kind", "data_seq", "channel_seq", "time_ns", "gap_ns", "ready_feedback_ns",
    "relay_drop_ns", "retired_ns", "gap_to_retry_ms", "ready_feedback_to_retry_ms",
    "attempt_to_retry_ms", "native_attempt_age_ms", "native_pump_events_after_feedback",
    "relay_drop_reason", "native_send_to_relay_drop_offer_ms", "retry_to_retire_ms",
)
# Only these fields contain absolute monotonic timestamps in the sparse proof
# schema. Other nanosecond fields, such as lateness_ns and duration_ns, are
# elapsed measurements and must retain their units and original values.
ABSOLUTE_TIMESTAMP_FIELDS = frozenset((
    "time_ns", "gap_ns", "ready_feedback_ns", "relay_drop_ns", "retired_ns",
    "start_ns", "end_ns", "scheduled_ns", "injected_ns", "observed_ns", "deadline_ns",
))
TREE_FIELDS = {
    "source_snapshot", "source_snapshot_directory", "campaign_source_snapshot", "snapshot_directory",
    "frozen_source_directory", "build_directory", "binary_directory", "calibration_directory",
}
VERSION_FIELDS = (
    "commit", "resolved_native_source_commit", "source_parent_commit", "source_patch_sha256",
    "tracked_dirty", "tracked_changes_sha256", "campaign_source_diff_sha256",
)
SOURCE_METADATA_FIELDS = (*VERSION_FIELDS, "campaign_source_snapshot", "binary_sha256",
                          "native_source_sha256", "campaign_source_files")


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def stats(values):
    values = [v for v in values if finite(v)]
    return {"n": len(values), "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None}


def relative_path(value, root):
    if not isinstance(value, str):
        return value
    path = Path(value)
    if not path.is_absolute():
        return value
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return value


def portable(value, root):
    if isinstance(value, dict):
        return {key: portable(child, root) for key, child in value.items()}
    if isinstance(value, list):
        return [portable(child, root) for child in value]
    return relative_path(value, root)


def trace_summary(trace, root):
    """Keep sparse causal chains and slow spans, including relay-confounded proof."""
    if not isinstance(trace, dict):
        trace = {}
    result = {key: trace.get(key) for key in ("enabled", "available", "valid", "reasons")}
    result["native_clock_alignment"] = trace.get("native_clock_alignment")
    result["native_metadata"] = {key: (trace.get("native_metadata") or {}).get(key) for key in (
        "version", "count", "overflow", "clock_error_count", "recovery_mode", "timestamp_clock")}
    peer = trace.get("fixed_peer_native_trace") or {}
    result["fixed_peer_native_trace"] = {key: peer.get(key) for key in ("available", "valid", "reasons")}
    for key in ("call_ledger", "native_ledger", "relay_ledger"):
        if trace.get(key):
            result[key] = relative_path(trace[key], root)
    # Invalid traces are explicitly visible, but unaudited timestamps never form proof.
    if trace.get("valid") is not True:
        result["causal_evidence_available"] = False
        result["unknown_intervals"] = list(UNKNOWN_INTERVALS)
        return result
    attribution = trace.get("attribution") or {}
    result.update({key: attribution.get(key) for key in (
        "stall_threshold_ms", "relay_tolerance_ms", "stall_classes", "miss_classes", "recovery_counts")})
    result["native_event_counts"] = trace.get("native_event_counts", {})
    recoveries = {}

    def retain(recovery):
        if not isinstance(recovery, dict):
            return None
        item = {key: recovery.get(key) for key in RECOVERY_FIELDS}
        identity = (item["kind"], item["time_ns"], item["data_seq"], item["channel_seq"])
        if identity not in recoveries:
            recoveries[identity] = item
        return identity

    # Native retry evidence is meaningful only when feedback and a C→S drop
    # can be linked. Unmatched source/peer intervals stay explicitly unknown.
    for recovery in attribution.get("recoveries", []):
        if recovery.get("ready_feedback_ns") is not None and recovery.get("relay_drop_reason") is not None:
            retain(recovery)
    stalls = []
    for span in attribution.get("stalls", []):
        item = {key: span.get(key) for key in (
            "call_kind", "record_id", "bytes", "start_ns", "end_ns", "duration_ns",
            "classification", "relay_dispatch_count", "relay_max_lateness_ms",
            "relay_lateness_confound", "native_event_counts")}
        proof = span.get("native_admission_timer_evidence", span.get("native_timer_recovery_evidence", []))
        item["proof_keys"] = [retain(r) for r in proof]
        stalls.append(item)
    ordered = sorted(recoveries.items(), key=lambda kv: (kv[1]["time_ns"] or 0, repr(kv[0])))
    ids = {identity: index for index, (identity, _) in enumerate(ordered)}
    result["recovery_proof"] = [dict(recovery, proof_id=ids[identity]) for identity, recovery in ordered]
    for span in stalls:
        span["native_admission_timer_evidence"] = [ids[key] for key in span.pop("proof_keys") if key in ids]
    result["slow_call_spans"] = stalls
    result["misses"] = copy.deepcopy(attribution.get("misses", []))
    result["causal_evidence_available"] = True
    result["unknown_intervals"] = sorted(set(UNKNOWN_INTERVALS).union(
        note for span in attribution.get("stalls", []) for note in span.get("unknown_intervals", [])))
    # Every sparse timestamp has the same epoch; retaining it allows exact
    # reconstruction while smaller relative integers make reports portable.
    def timestamps(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key in ABSOLUTE_TIMESTAMP_FIELDS and isinstance(child, int):
                    yield child
                elif isinstance(child, (dict, list)):
                    yield from timestamps(child)
        elif isinstance(value, list):
            for child in value:
                yield from timestamps(child)
    evidence = {key: result[key] for key in ("recovery_proof", "slow_call_spans", "misses")}
    epoch = min(timestamps(evidence), default=None)
    result["sparse_proof_epoch_ns"] = epoch
    def offset(value):
        if isinstance(value, dict):
            for key in list(value):
                child = value[key]
                if key in ABSOLUTE_TIMESTAMP_FIELDS:
                    value[key[:-3] + "_relative_ns"] = child - epoch if isinstance(child, int) and epoch is not None else None
                    del value[key]
                else:
                    offset(child)
        elif isinstance(value, list):
            for child in value:
                offset(child)
    offset(evidence)
    return result


def compact_trial(row, root):
    keys = ("phase", "trial_id", "case_id", "profile_id", "workload", "variant", "seed",
            "comparison_key", "commit", "binary_sha256", "queue_cap_bytes", "validity", "original_validity",
            "sut_outcome", "integrity_verified", "harness_reasons", "artifact_reasons",
            "sut_failures", "integrity_failures", "trace_audit_reasons", "calibration_notes")
    result = {key: row.get(key) for key in keys}
    result["artifact_directory"] = relative_path(row.get("artifact_directory"), root)
    manifest, invariants, metrics = row.get("manifest") or {}, row.get("invariants") or {}, row.get("metrics") or {}
    result["binary_sha256"] = row.get("binary_sha256") or manifest.get("binary_sha256")
    result["peer_sha256"] = invariants.get("peer_sha256")
    result["actual_configuration"] = {
        "dut_cap_bytes": metrics.get("client.queue_queue_limit_bytes"),
        "peer_cap_bytes": metrics.get("server.queue_queue_limit_bytes"),
        "dut_window_packets": metrics.get("client.queue_effective_window_packets"),
        "peer_window_packets": metrics.get("server.queue_effective_window_packets"),
        "dut_peer_log_window": metrics.get("client.queue_peer_log_window"),
        "peer_peer_log_window": metrics.get("server.queue_peer_log_window"),
    }
    result["version"] = {key: (manifest.get("build_metadata") or {}).get(key) for key in VERSION_FIELDS}
    result["metrics"] = {name: metrics.get(name) if finite(metrics.get(name)) else None for name in TRIAL_METRICS}
    result["probes"] = {side: {name: metrics.get(f"{side}.{name}") for name in PROBES}
                        for side in ("client", "server")}
    result["loss_trace"] = trace_summary(row.get("loss_trace"), root)
    requested = bool(invariants.get("case", {}).get("loss_trace") or result["loss_trace"].get("enabled"))
    strict = (row.get("validity") in ("VALIDATED_LAB", "PROVISIONAL_UNCALIBRATED")
              and row.get("sut_outcome") == "COMPLETE" and row.get("integrity_verified") is True
              and (not requested or result["loss_trace"].get("valid") is True)
              and row.get("artifact_intact_timing_valid_complete") is not False)
    result["strict_eligible"] = strict
    result["all_observed_eligible"] = row.get("validity") != "INVALID_ARTIFACT"
    if not result["all_observed_eligible"]:
        result["metrics"] = {name: None for name in TRIAL_METRICS}
        result["actual_configuration"] = {name: None for name in result["actual_configuration"]}
        # Original accounting remains failure evidence, but is not a numeric sample.
        result["probes"] = {side: {name: None for name in PROBES} for side in ("client", "server")}
        result["loss_trace"] = trace_summary({"enabled": requested, "available": False, "valid": False,
                                               "reasons": ["sealed artifacts changed/missing; trace withheld"]}, root)
    elif row.get("integrity_verified") is not True:
        result["metrics"]["c2s.verified_goodput_mbps"] = None
    return result


def counts(rows):
    result = {"trials": len(rows), "integrity_pass": sum(r["integrity_verified"] is True for r in rows),
              "integrity_fail": sum(r["integrity_verified"] is False for r in rows),
              "service_failures": sum(r["sut_outcome"] == "FAIL" for r in rows),
              "service_unknown": sum(r["sut_outcome"] not in ("FAIL", "COMPLETE") for r in rows),
              "timing_invalid": sum(r["original_validity"] == "INVALID_HARNESS" for r in rows),
              "artifact_invalid": sum(r["validity"] == "INVALID_ARTIFACT" for r in rows),
              "trace_invalid": sum(bool(r["loss_trace"].get("enabled")) and r["loss_trace"].get("valid") is not True for r in rows),
              "strict_eligible": sum(r["strict_eligible"] for r in rows)}
    result["probes"] = {}
    for name in PROBES:
        values = [r["probes"]["client"].get(name) for r in rows]
        known = [v for v in values if finite(v)]
        result["probes"][name] = {"sum": sum(known) if known else None, "known_trials": len(known),
                                 "unknown_trials": len(values) - len(known)}
    return result


def comparisons(rows):
    keyed = defaultdict(dict)
    for row in rows:
        key = (row["phase"], row["case_id"], row["comparison_key"], row["seed"])
        if row["variant"] in keyed[key]:
            raise ValueError(f"duplicate phase/case/configuration/seed/variant: {key} {row['variant']}")
        keyed[key][row["variant"]] = row
    grouped, unpaired = defaultdict(list), []
    for (phase, case, key, seed), variants in keyed.items():
        labels = ("reference", "candidate") if phase == "main" else ("timer", "fast") if phase == "smoke" or str(phase).startswith("diagnostic") else None
        if not labels:
            continue
        for cap in (48, 64):
            a, b = (f"{label}-q{cap}" for label in labels)
            if a not in variants and b not in variants:
                continue
            if a not in variants or b not in variants:
                unpaired.extend({"trial_id": r["trial_id"], "phase": phase, "seed": seed,
                                 "reason": "missing identical case/comparison_key/seed counterpart"}
                                for name, r in variants.items() if name in (a, b))
                continue
            grouped[(phase, case, key, a, b)].append((seed, variants[a], variants[b]))
    output = []
    for (phase, case, key, a, b), pairs in sorted(grouped.items()):
        for subset in SUBSETS:
            eligibility = "strict_eligible" if subset == SUBSETS[1] else "all_observed_eligible"
            accepted, excluded = [], []
            for seed, x, y in sorted(pairs):
                record = {"seed": seed, "baseline_trial": x["trial_id"], "candidate_trial": y["trial_id"]}
                if x[eligibility] and y[eligibility]:
                    accepted.append((record, x, y))
                else:
                    excluded.append(dict(record, baseline_validity=x["validity"], candidate_validity=y["validity"],
                                         baseline_outcome=x["sut_outcome"], candidate_outcome=y["sut_outcome"],
                                         reason="subset ineligibility; trace invalidity is included for strict pairs"))
            item = {"phase": phase, "case_id": case, "comparison_key": key, "baseline_variant": a,
                    "candidate_variant": b, "subset": subset, "paired_n": len(accepted),
                    "pairs": [record for record, _, _ in accepted], "excluded_pairs": excluded, "metrics": {}}
            for name in PAIR_METRICS:
                values = [(record["seed"], x["metrics"].get(name), y["metrics"].get(name)) for record, x, y in accepted]
                numeric = [(seed, x, y) for seed, x, y in values if finite(x) and finite(y)]
                item["metrics"][name] = {"n": len(numeric), "missing_seeds": [seed for seed, x, y in values if not finite(x) or not finite(y)],
                                         "baseline": stats(x for _, x, _ in numeric), "candidate": stats(y for _, _, y in numeric),
                                         "paired_delta": stats(y - x for _, x, y in numeric)}
            output.append(item)
    return output, unpaired


def auxiliary_status(value, root):
    """Preserve failures without copying configuration, samples or build options."""
    if not isinstance(value, dict):
        return {"available": False, "status": "unknown"}
    if "trials" in value and isinstance(value["trials"], list):
        return {"available": True, **{key: value.get(key) for key in ("pass", "complete", "expected_trials", "observed_trials", "returncode")},
                "trials": [{key: row.get(key) for key in ("trial_id", "validity", "sut_outcome", "integrity_verified", "reasons", "sut_failures")}
                           for row in value["trials"]]}
    if isinstance(value.get("phases"), dict):
        return {"available": True, "pass": value.get("pass"),
                "artifact_status": value.get("artifact_status", "status manifest retained; exporter does not audit failed preflight artifacts"),
                "phases": {name: {key: phase.get(key) for key in (
                    "complete", "expected_trials", "observed_trials", "returncode", "failed_trials")}
                           for name, phase in value["phases"].items()}}
    keep = ("version", "pass", "complete", "approved", "approved_by", "scope", "limitations", "criteria", "failed_criteria", "reasons", "artifact_status", "counts",
            "target_headroom_pps", "empirically_passed_packet_rate", "covers_2x_supplied_native_pps",
            "driver_sha256", "engine_files", "profile_source_sha256")
    result = {key: value[key] for key in keep if key in value}
    result["available"] = True
    # Different calibration versions use samples/cases/tests. Retain only
    # failing records and their criteria, keeping raw samples in the source.
    def failures(node, path=""):
        found = []
        if isinstance(node, dict):
            if node.get("pass") is False or node.get("valid") is False:
                record = {"path": path, **{key: node[key] for key in (
                    "id", "case_id", "profile_id", "mode", "direction", "seed", "pass", "valid", "failed_criteria", "reasons", "error", "errors", "evidence") if key in node}}
                criteria = node.get("criteria")
                if isinstance(criteria, list):
                    record["failed_criteria"] = [item for item in criteria if item.get("pass") is False]
                elif isinstance(criteria, dict):
                    record["failed_criteria"] = {name: passed for name, passed in criteria.items() if passed is False}
                found.append(record)
            for key, child in node.items():
                if isinstance(child, (dict, list)) and key not in ("criteria", "failed_criteria", "reasons", "build_options"):
                    found.extend(failures(child, f"{path}.{key}" if path else key))
        elif isinstance(node, list):
            for index, child in enumerate(node):
                found.extend(failures(child, f"{path}[{index}]"))
        return found
    result["failed_records"] = failures(value)
    result["failed_sample_count"] = len(value.get("failed_samples", [])) if isinstance(value.get("failed_samples"), list) else None
    return portable(result, root)


def build_report(audited, root, input_path=None, calibration=None, failed_trace_smoke=None):
    if not isinstance(audited, dict) or not isinstance(audited.get("trials"), list):
        raise ValueError("input must be an audited loss-stall aggregate with a trials list")
    rows = [compact_trial(row, root) for row in audited["trials"]]
    if len({(row["phase"], row["trial_id"]) for row in rows}) != len(rows):
        raise ValueError("trial IDs must be unique within each phase")
    pairs, unpaired = comparisons(rows)
    plan = audited.get("provenance") or {}
    provenance = {key: plan.get(key) for key in (
        "version", "candidate_library", "candidate_sha256", "reference_library", "reference_sha256",
        "fixed_peer_library", "fixed_peer_sha256", "fixed_peer_environment", "source_hashes",
        "driver_sha256", "reporter_sha256", "profiles_sha256", "notes")}
    provenance["input"] = relative_path(str(input_path), root) if input_path else None
    provenance["input_sha256"] = hashlib.sha256(Path(input_path).read_bytes()).hexdigest() if input_path else None
    provenance["paths_relative_to"] = str(root.name)
    provenance["candidate_metadata"] = {key: (plan.get("candidate_metadata") or {}).get(key) for key in SOURCE_METADATA_FIELDS}
    provenance["reference_metadata"] = {key: (plan.get("reference_metadata") or {}).get(key) for key in SOURCE_METADATA_FIELDS}
    phases = []
    for phase in audited.get("phases", []):
        selected = [r for r in rows if r["phase"] == phase.get("phase")]
        phases.append({"phase": phase.get("phase"), "expected_trials": phase.get("expected_trials"),
                       "complete": phase.get("complete"), "counts": counts(selected)})
    return {"version": 1, "scope": audited.get("scope"), "audit_contract": "Input is already SHA/native/relay-ledger audited; this exporter does not recertify source artifacts.",
            "provenance": portable(provenance, root), "counts": counts(rows), "phases": phases,
            "calibration": auxiliary_status(calibration if calibration is not None else audited.get("calibration"), root),
            "failed_trace_smoke_v2": auxiliary_status(failed_trace_smoke if failed_trace_smoke is not None else audited.get("failed_trace_smoke_v2"), root),
            "comparisons": pairs, "unpaired": unpaired, "trials": rows,
            "notes": list(audited.get("notes") or []) + [
                "All observed diagnostics retain failed and timing-invalid runs; hash-invalid metrics are withheld in every subset.",
                "Strict pairs require matching phase, case, comparison key and seed, plus timing-valid COMPLETE integrity-verified trials and audited requested tracing.",
                "Paired N is also reported per metric; an absent metric never becomes zero or changes the matching seeds silently.",
                "Sparse proof timestamps are relative to each trial's recorded sparse_proof_epoch_ns. Duration fields retain their original units.",
                "Relay-confounded calls can still contain observed native timer recovery proof. The proof does not establish exclusive causation.",
            ]}


def markdown(report):
    c = report["counts"]
    lines = ["# Loss-stall compact results", "", str(report.get("scope")), "",
             f"{c['trials']} trials; {c['integrity_pass']} integrity passes; {c['service_failures']} service failures; "
             f"{c['timing_invalid']} timing invalid; {c['artifact_invalid']} artifact invalid; {c['trace_invalid']} trace invalid.", "",
             "Calibration and the failed trace smoke run are separate evidence; neither is hidden by successful main comparisons.", "",
             "| Phase | Case | Baseline → candidate | Subset | Pairs | Goodput Mbit/s | RTT p95/p99 ms | Scheduled p95/p99 ms |",
             "|---|---|---|---|---:|---|---|---|"]
    def value(item, name, side):
        number = item["metrics"][name][side]["median"]
        return "unknown" if number is None else f"{number:.3f}"
    for item in report["comparisons"]:
        good = " → ".join(value(item, PAIR_METRICS[0], s) for s in ("baseline", "candidate"))
        latency = [" → ".join("/".join(value(item, name, side) for name in names) for side in ("baseline", "candidate"))
                   for names in (PAIR_METRICS[1:3], PAIR_METRICS[3:5])]
        lines.append(f"| {item['phase']} | {item['case_id']} | {item['baseline_variant']} → {item['candidate_variant']} | "
                     f"{item['subset']} | {item['paired_n']} | {good} | {latency[0]} | {latency[1]} |")
    lines += ["", "## Failures retained", ""]
    failures = []
    for row in report["trials"]:
        if row["sut_outcome"] != "COMPLETE" or row["original_validity"] == "INVALID_HARNESS" or row["validity"] == "INVALID_ARTIFACT" or (row["loss_trace"].get("enabled") and row["loss_trace"].get("valid") is not True):
            reasons = [r for key in ("harness_reasons", "artifact_reasons", "sut_failures", "integrity_failures", "trace_audit_reasons") for r in (row.get(key) or [])]
            failures.append(f"- {row['phase']}/{row['trial_id']}: {row['validity']} / {row['sut_outcome']}; {'; '.join(dict.fromkeys(reasons))}")
    lines += failures or ["No trial service, timing, artifact or requested-trace failure recorded."]
    for name in ("calibration", "failed_trace_smoke_v2"):
        lines += ["", f"## {name}", "", "```json", json.dumps(report[name], indent=2, allow_nan=False), "```"]
    lines += ["", "## Sparse causal evidence", ""]
    for row in report["trials"]:
        trace = row["loss_trace"]
        if trace.get("enabled"):
            lines.append(f"- {row['phase']}/{row['trial_id']}: trace valid={trace.get('valid')}; "
                         f"stalls={json.dumps(trace.get('stall_classes', {}))}; misses={json.dumps(trace.get('miss_classes', {}))}; "
                         f"{len(trace.get('recovery_proof', []))} matched feedback/drop/retry chains.")
    lines += ["", "## Limits", ""] + ["- " + note for note in report["notes"]]
    return "\n".join(lines) + "\n"


def guard_output(input_path, output, root, audited, extra_inputs=()):
    targets = [output.with_suffix(suffix).resolve() for suffix in (".json", ".csv", ".md")]
    tool_root = Path(__file__).resolve().parent
    files = {input_path.resolve(), *(Path(p).resolve() for p in extra_inputs)}
    trees = {root / name for name in ("source", "source-snapshot", "frozen-source", "binaries", "relay-calibration", "calibration", "failed-trace-smoke-v2")}
    trees.update(root / name for name in (audited.get("provenance", {}).get("configurations") or {}))
    trees.update(tool_root / name for name in ("rdpbench", "tests", "configs", "docs", "linux"))
    files.update(path.resolve() for path in tool_root.glob("*") if path.is_file())
    files.update((root / name).resolve() for name in ("loss-sweep-plan.json", "loss-sweep-status.json", "peer-env.json"))
    def declared(value, field=None):
        if field == "build_options":
            return
        if isinstance(value, dict):
            for key, child in value.items():
                declared(child, key)
        elif isinstance(value, list):
            for child in value:
                declared(child, field)
        elif isinstance(value, str) and Path(value).is_absolute():
            path = Path(value).resolve()
            if field in TREE_FIELDS:
                trees.add(path)
            elif path.is_file():
                files.add(path)
                if path.suffix in (".dylib", ".so", ".dll", ".a"):
                    trees.add(path.parent)
    declared(audited.get("provenance", {}))
    for row in audited.get("trials", []):
        if row.get("artifact_directory"):
            trees.add(Path(row["artifact_directory"]))
    if any(target in files or any(target.is_relative_to(tree.resolve()) for tree in trees) for target in targets):
        raise ValueError("output would overwrite input or enter sealed campaign, source, binary or calibration trees")


def write_report(report, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    # Compact JSON keeps numeric provenance and sparse proof committable without
    # repeating hundreds of metrics/build options for every trial.
    output.with_suffix(".json").write_text(json.dumps(report, separators=(",", ":"), allow_nan=False) + "\n", encoding="utf-8")
    output.with_suffix(".md").write_text(markdown(report), encoding="utf-8")
    fields = ("phase", "trial_id", "case_id", "profile_id", "variant", "seed", "commit", "binary_sha256", "peer_sha256",
              "queue_cap_bytes", "validity", "original_validity", "sut_outcome", "integrity_verified", "strict_eligible",
              "dut_window_packets", "peer_window_packets", *PROBES, *TRIAL_METRICS,
              "trace_valid", "trace_stall_classes", "trace_miss_classes", "native_timer_proof_count", "failures")
    with output.with_suffix(".csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fields, lineterminator="\n")
        writer.writeheader()
        for row in report["trials"]:
            values = {key: row.get(key) for key in fields[:15]}
            values.update({key: row["actual_configuration"].get(key) for key in ("dut_window_packets", "peer_window_packets")})
            values.update(row["probes"]["client"])
            values.update(row["metrics"])
            trace = row["loss_trace"]
            values.update(trace_valid=trace.get("valid"), trace_stall_classes=json.dumps(trace.get("stall_classes", {})),
                          trace_miss_classes=json.dumps(trace.get("miss_classes", {})),
                          native_timer_proof_count=sum(bool(span.get("native_admission_timer_evidence")) for span in trace.get("slow_call_spans", [])),
                          failures=json.dumps({key: row.get(key) for key in ("harness_reasons", "artifact_reasons", "sut_failures", "integrity_failures", "trace_audit_reasons")}))
            writer.writerow(values)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path(DEFAULT_INPUT))
    parser.add_argument("--output", type=Path, default=Path(DEFAULT_OUTPUT), help="Output filename prefix")
    parser.add_argument("--root", type=Path, help="Campaign root; defaults to the input's grandparent")
    parser.add_argument("--calibration", type=Path, help="Optional small calibration summary")
    parser.add_argument("--failed-preflight", "--failed-trace-smoke", dest="failed_trace_smoke", type=Path,
                        help="Optional failed smoke campaign/status JSON")
    args = parser.parse_args(argv)
    input_path, output = args.input.expanduser().resolve(), args.output.expanduser().resolve()
    root = args.root.expanduser().resolve() if args.root else input_path.parent.parent
    try:
        audited = json.loads(input_path.read_text(encoding="utf-8"))
        extras = [p.expanduser().resolve() for p in (args.calibration, args.failed_trace_smoke) if p]
        guard_output(input_path, output, root, audited, extras)
        calibration = json.loads(args.calibration.read_text()) if args.calibration else None
        failed_trace = json.loads(args.failed_trace_smoke.read_text()) if args.failed_trace_smoke else None
        report = build_report(audited, root, input_path, calibration, failed_trace)
        write_report(report, output)
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps({"output": str(output), "counts": report["counts"], "json_bytes": output.with_suffix(".json").stat().st_size}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
