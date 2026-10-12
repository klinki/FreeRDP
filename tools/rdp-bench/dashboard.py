#!/usr/bin/env python3
"""Export an offline dashboard from audited transport reports and optional live RDP results.

The dashboard is a view of supplied evidence, not another benchmark or calibration
authority. Whole-trial percentiles stay whole-trial values. Missing measurements
stay missing, and transport/application/render scopes remain distinct.
"""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
STRICT = "artifact_intact_timing_valid_complete"
ALL = "all_observed_diagnostic"
SUBSETS = (STRICT, ALL)
PROBE_KEYS = ("expected", "injected", "observed", "missed", "unobserved", "timeouts")
METRIC_INFO = {
    "c2s.verified_goodput_mbps": ("Verified upload goodput", "Mbit/s", "Verified application payload after integrity drain"),
    "s2c.verified_goodput_mbps": ("Verified download goodput", "Mbit/s", "Verified application payload after integrity drain"),
    "client.probe_rtt_ms.p95": ("Probe RTT p95", "ms", "Injection before native admission to observed response"),
    "client.probe_rtt_ms.p99": ("Probe RTT p99", "ms", "Injection before native admission to observed response"),
    "client.probe_scheduled_ms.p99": ("Scheduled probe p99", "ms", "Intended injection deadline to observed response"),
    "client.queue_blocked_admission_ms.max": ("Longest blocked admission", "ms", "Native admission blocking, maximum of a whole trial"),
    "client.queue_residence_ms.p99": ("Queue residence p99", "ms", "Acceptance to first successful socket send; histogram upper bound"),
    "verified_goodput_mbps": ("Verified live payload goodput", "Mbit/s", "Requires independent payload integrity evidence"),
    "input_visible_ms.p95": ("Visible input response p95", "ms", "Input injection to first verified remote response in the client image"),
    "input_visible_ms.p99": ("Visible input response p99", "ms", "Input injection to first verified remote response in the client image"),
    "input_scheduled_ms.p99": ("Scheduled visible response p99", "ms", "Intended input deadline to verified visible response"),
    "injection_lateness_ms.p99": ("Input injection lateness p99", "ms", "Injection time minus intended deadline"),
    "progress.observed_hz": ("Observed progress", "Hz", "Verified visible workload progression, not SDL redraw count"),
    "progress.target_delivery_fraction": ("Target progress delivered", "fraction", "Verified progress relative to the workload target"),
    "progress.longest_gap_ms": ("Longest progress gap", "ms", "Longest observed interval without visible progression"),
    "progress.freeze_fraction_over_250ms": ("Time in progress gaps >250 ms", "fraction", "Measured visible progress gaps"),
    "traffic.udp_rx_Bps_mean": ("UDP receive traffic", "B/s", "Recorded traffic counters; not verified application goodput"),
    "traffic.tcp_rx_Bps_mean": ("TCP receive traffic", "B/s", "Recorded traffic counters; not verified application goodput"),
    "transport_rtt_ms.p99": ("Reported transport RTT p99", "ms", "Fresh deduplicated RDP RTT readings; diagnostic, not input latency"),
    "render.redraw_ms.p99": ("SDL redraw p99", "ms", "Client render wall duration; not end-to-end input latency"),
    "render.present_interval_ms.p99": ("SDL present interval p99", "ms", "Completed-present intervals; may include idle periods"),
}


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def read(path):
    def invalid(value):
        raise ValueError(f"non-finite JSON number: {value}")
    return json.loads(Path(path).read_text(encoding="utf-8"), parse_constant=invalid)


def flatten_metrics(values, prefix=""):
    if not isinstance(values, dict):
        raise ValueError("metrics must be an object")
    out = {}
    for key, value in values.items():
        name = prefix + str(key)
        if isinstance(value, dict):
            out.update(flatten_metrics(value, name + "."))
        elif value is None or finite(value):
            out[name] = value
        else:
            raise ValueError(f"metric {name}: expected a finite number or null")
    return out


def probe_counts(row):
    """Normalize counts without treating absence as zero or deriving timeouts."""
    source = row.get("probes")
    if isinstance(source, dict) and isinstance(source.get("client"), dict):
        source = source["client"]
    elif not isinstance(source, dict):
        source = {}
    metrics = row.get("metrics", {})
    aliases = {
        "expected": ("expected", "requested", "probes_expected"),
        "injected": ("injected", "probes_injected"),
        "observed": ("observed", "probes_observed"),
        "missed": ("missed", "missed_injections", "probes_not_injected"),
        "unobserved": ("unobserved",),
        "timeouts": ("timeouts", "probe_timeouts"),
    }
    out = {}
    for key, names in aliases.items():
        value = next((source[n] for n in names if n in source), None)
        if value is None:
            value = next((metrics["client." + n] for n in names if "client." + n in metrics), None)
        if value is not None and (not isinstance(value, int) or isinstance(value, bool) or value < 0):
            raise ValueError(f"{row.get('trial_id')}: invalid probe count {key}")
        out[key] = value
    return out


def normalize_trial(source, live=False):
    row = copy.deepcopy(source)
    if not isinstance(row, dict) or not isinstance(row.get("trial_id"), str) or not row["trial_id"]:
        raise ValueError("every trial needs a nonempty trial_id")
    row.setdefault("phase", "live" if live else "unknown")
    row.setdefault("case_id", row.get("workload", "unknown"))
    row.setdefault("profile_id", "unknown")
    row.setdefault("variant", "unknown")
    row.setdefault("validity", "UNKNOWN")
    row.setdefault("sut_outcome", "UNKNOWN")
    row["metrics"] = flatten_metrics(row.get("metrics", {}))
    if live:
        row["probe_evidence"] = copy.deepcopy(row.get("probes") or {})
    row["probes"] = probe_counts(row)
    reasons = []
    for field in ("reasons", "harness_reasons", "artifact_reasons", "sut_failures", "integrity_failures", "trace_audit_reasons"):
        values = row.get(field) or []
        if not isinstance(values, list):
            raise ValueError(f"{row['trial_id']}: {field} must be an array")
        reasons.extend(str(value) for value in values)
    row["reasons"] = list(dict.fromkeys(reasons))
    artifact_ok = row["validity"] != "INVALID_ARTIFACT" and not row.get("artifact_reasons")
    integrity_ok = live or row.get("integrity_verified") is True
    trace = row.get("loss_trace") or {}
    if not isinstance(trace, dict):
        raise ValueError(f"{row['trial_id']}: loss_trace must be an object")
    trace_ok = not trace.get("enabled") or trace.get("valid") is True
    probe_ok = all(row["probes"][name] in (None, 0) for name in ("missed", "unobserved", "timeouts"))
    for a, b in (("expected", "injected"), ("injected", "observed")):
        if row["probes"][a] is not None and row["probes"][b] is not None and row["probes"][a] != row["probes"][b]:
            probe_ok = False
    if not probe_ok:
        row["reasons"] = list(dict.fromkeys(row["reasons"] + ["Recorded probe service is incomplete"]))
    timing_ok = row["validity"] in ("VALIDATED_LAB", "PROVISIONAL_UNCALIBRATED", "LIVE_OBSERVATIONAL", "PROVISIONAL_LIVE")
    source_strict = row.get("strict_eligible", row.get("artifact_intact_timing_valid_complete", True))
    row["strict_eligible"] = bool(source_strict and artifact_ok and integrity_ok and timing_ok and trace_ok and probe_ok and row["sut_outcome"] == "COMPLETE")
    row["all_observed_eligible"] = bool(artifact_ok and integrity_ok and row.get("all_observed_eligible", True))
    for key in row["metrics"]:
        if not artifact_ok or ("verified_goodput" in key and row.get("integrity_verified") is not True):
            row["metrics"][key] = None
    if live:
        observed_values = any(finite(value) and (not key.endswith(".count") or value > 0)
                              for key, value in row["metrics"].items())
        row["measurement_evidence_available"] = observed_values
        row["strict_eligible"] = row["strict_eligible"] and observed_values
        row["all_observed_eligible"] = row["all_observed_eligible"] and observed_values
        if not observed_values:
            row["reasons"] = list(dict.fromkeys(row["reasons"] + ["No recorded numeric measurement evidence"]))
    row["commit"] = row.get("commit") or row.get("source_commit") or row.get("client_commit")
    row["binary_sha256"] = row.get("binary_sha256") or row.get("client_binary_sha256")
    row["live"] = live
    return row


def stats(values):
    values = [value for value in values if finite(value)]
    return {"n": len(values), "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None}


def comparisons(trials, declarations):
    """Recompute paired whole-trial summaries; declarations select comparisons only."""
    selected = {}
    for item in declarations:
        key = tuple(item.get(field) for field in ("phase", "case_id", "comparison_key", "baseline_variant", "candidate_variant"))
        if not all(value is not None for value in key):
            raise ValueError("comparison needs phase, case_id, comparison_key and both variants")
        metrics = item.get("metrics")
        names = list(metrics) if isinstance(metrics, dict) else item.get("metric_names", [item.get("metric")])
        selected.setdefault(key, set()).update(name for name in names if isinstance(name, str))
    out = []
    for key, names in sorted(selected.items()):
        phase, case_id, config, baseline, candidate = key
        if baseline == candidate:
            raise ValueError("comparison variants must differ")
        by_seed = defaultdict(dict)
        for row in trials:
            if (row["phase"], row["case_id"], row.get("comparison_key")) != key[:3] or row["variant"] not in (baseline, candidate):
                continue
            seed = row.get("seed")
            if seed is None:
                continue
            if row["variant"] in by_seed[seed]:
                raise ValueError(f"duplicate paired seed: {phase}/{case_id}/{seed}/{row['variant']}")
            by_seed[seed][row["variant"]] = row
        for subset in SUBSETS:
            eligible_key = "strict_eligible" if subset == STRICT else "all_observed_eligible"
            pairs, excluded = [], []
            for seed, sides in sorted(by_seed.items(), key=lambda item: str(item[0])):
                a, b = sides.get(baseline), sides.get(candidate)
                if a and b and a[eligible_key] and b[eligible_key]:
                    pairs.append((seed, a, b))
                else:
                    excluded.append({"seed": seed, "baseline_trial": a["trial_id"] if a else None,
                                     "candidate_trial": b["trial_id"] if b else None,
                                     "reasons": ["no matching configuration/seed trial"] if not a or not b else
                                     list(dict.fromkeys(a["reasons"] + b["reasons"] + ["pair outside selected subset"]))})
            metrics = {}
            for name in sorted(names):
                present = [(seed, a["metrics"].get(name), b["metrics"].get(name)) for seed, a, b in pairs]
                known = [(seed, a, b) for seed, a, b in present if finite(a) and finite(b)]
                metrics[name] = {"n": len(known), "missing_seeds": [seed for seed, a, b in present if not finite(a) or not finite(b)],
                                 "baseline": stats([a for _, a, _ in known]), "candidate": stats([b for _, _, b in known]),
                                 "paired_delta": stats([b - a for _, a, b in known])}
            out.append(dict(zip(("phase", "case_id", "comparison_key", "baseline_variant", "candidate_variant"), key),
                            subset=subset, paired_n=len(pairs), metrics=metrics, excluded_pairs=excluded,
                            pairs=[{"seed": seed, "baseline_trial": a["trial_id"], "candidate_trial": b["trial_id"]} for seed, a, b in pairs]))
    return out


def campaign(path, kind):
    source = read(path)
    if not isinstance(source, dict) or source.get("version") != 1 or not isinstance(source.get("trials"), list):
        raise ValueError(f"{path}: expected version 1 report with trials array")
    live = kind == "live"
    if live and source.get("schema") != "freerdp.live_benchmark":
        raise ValueError("live input requires schema freerdp.live_benchmark, version 1")
    rows = [normalize_trial(row, live=live) for row in source["trials"]]
    ids = [row["trial_id"] for row in rows]
    if len(ids) != len(set(ids)):
        raise ValueError(f"{path}: duplicate trial_id")
    names = {"loss": "Loss recovery", "ack": "Queue & ACK timing", "live": "Windows VM"}
    declarations = source.get("comparisons", [])
    if live and not declarations and source.get("comparison_variants"):
        variants = source["comparison_variants"]
        declarations = [dict(phase=phase, case_id=case, comparison_key=config,
                             baseline_variant=variants["baseline"], candidate_variant=variants["candidate"],
                             metric_names=sorted({name for row in rows for name in row["metrics"]}))
                        for phase, case, config in sorted({(r["phase"], r["case_id"], r.get("comparison_key")) for r in rows if r.get("comparison_key")})]
    metadata = {key: copy.deepcopy(source.get(key)) for key in ("scope", "notes", "provenance", "calibration", "network_profiles", "metric_definitions", "phases", "preferred_phase", "coverage")}
    if kind == "ack" and not metadata["calibration"]:
        metadata["calibration"] = {"approved": False, "available": False, "status": "No accepted calibration receipt in this report", "notes": ["All raw UDP2 campaigns remain provisional."]}
    return {"id": kind, "label": names[kind], "kind": kind, "campaign_id": source.get("campaign_id", Path(path).stem),
            "source": {"file": Path(path).name, "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()},
            **metadata, "trials": rows, "comparisons": comparisons(rows, declarations),
            "auxiliary": {"failed_trace_smoke_v2": source.get("failed_trace_smoke_v2")} if kind == "loss" else {}}


def build_data(loss, ack, live=None):
    campaigns = [campaign(loss, "loss"), campaign(ack, "ack")]
    if live is not None:
        campaigns.append(campaign(live, "live"))
    else:
        campaigns.append({"id": "live", "label": "Windows VM", "kind": "live", "scope": "No live result supplied",
                          "trials": [], "comparisons": [], "notes": ["This dashboard contains no fabricated Windows measurements. Supply --live after a recorded VM campaign."]})
    return {"schema": "freerdp.benchmark_dashboard", "version": 1, "campaigns": campaigns,
            "default_campaign": "live" if campaigns[-1].get("coverage") else "loss",
            "metric_info": {name: {"label": label, "unit": unit, "scope": scope} for name, (label, unit, scope) in METRIC_INFO.items()},
            "contract": ["Each plotted point is a whole trial. Summary values are medians/ranges of whole-trial values, never pooled p95/p99.",
                         "Strict pairs require matching phase, case, configuration and seed with complete service and valid timing/artifacts.",
                         "All-observed diagnostics retain timing/service failures. Invalid-artifact metrics and unverified goodput are withheld.",
                         "Raw native UDP2, live Windows input response, reported transport RTT and SDL render wall time have separate scopes."]}


def output_guard(output, inputs):
    output = Path(output).resolve()
    targets = [output / name for name in ("index.html", "data.json", "trials.csv")]
    for source in [*inputs, HERE / "dashboard-assets/index.html"]:
        source = Path(source).resolve()
        # A dashboard gets its own directory rather than overwriting source/report trees.
        if output == source.parent or source == output or output in source.parents or any(target == source for target in targets):
            raise ValueError(f"output must be separate from input directory: {source}")
    return output


def export(data, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(data, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    (output / "data.json").write_text(encoded + "\n", encoding="utf-8")
    # Embedded JSON makes file:// use work without fetch or a server.
    template = (HERE / "dashboard-assets" / "index.html").read_text(encoding="utf-8")
    payload = encoded.replace("<", "\\u003c").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    (output / "index.html").write_text(template.replace("__BENCHMARK_DATA__", payload), encoding="utf-8")
    with (output / "trials.csv").open("w", newline="", encoding="utf-8") as stream:
        fields = ["campaign", "trial_id", "phase", "profile_id", "case_id", "variant", "seed", "queue_cap_bytes", "comparison_key", "commit", "binary_sha256", "validity", "sut_outcome", "integrity_verified", "strict_eligible", "metric", "value", "reasons"]
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for item in data["campaigns"]:
            for row in item["trials"]:
                metrics = {**row["metrics"], **{"probes." + key: value for key, value in row["probes"].items()}}
                for name, value in metrics.items():
                    values = {key: row.get(key) for key in fields if key in row}
                    values.update(campaign=item["id"], metric=name, value=value, reasons="; ".join(row["reasons"]))
                    writer.writerow(values)
    return output / "index.html"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loss", type=Path, default=HERE / "results/loss-stalls-20261012.json")
    parser.add_argument("--ack", type=Path, default=HERE / "results/peer-ack-20261011.json")
    parser.add_argument("--live", type=Path, help="freerdp.live_benchmark v1 JSON; omitted measurements remain unavailable")
    parser.add_argument("--output", type=Path, required=True, help="separate dashboard directory")
    args = parser.parse_args()
    inputs = [args.loss, args.ack] + ([args.live] if args.live else [])
    try:
        output = output_guard(args.output, inputs)
        data = build_data(args.loss, args.ack, args.live)
        path = export(data, output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"dashboard: {error}\n")
    print(json.dumps({"index": str(path), "campaigns": len(data["campaigns"]), "trials": sum(len(item["trials"]) for item in data["campaigns"])}))


if __name__ == "__main__":
    main()
