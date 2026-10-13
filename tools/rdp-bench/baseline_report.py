#!/usr/bin/env python3
"""Summarize an audited baseline campaign without pooling trial percentiles."""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path

from rdpbench.common import read_json, write_json
from rdpbench.report import collect

GOODPUT = ("c2s.verified_goodput_mbps", "s2c.verified_goodput_mbps")
LATENCY = tuple(f"{side}.{kind}.{q}" for side in ("client", "server")
                for kind in ("probe_rtt_ms", "probe_scheduled_ms") for q in ("p95", "p99"))
COUNT_NAMES = {"expected": "probes_expected", "injected": "probes_injected",
               "observed": "probes_observed", "missed": "probes_not_injected",
               "timeouts": "probe_timeouts", "rtt_samples": "probe_rtt_ms.count",
               "scheduled_samples": "probe_scheduled_ms.count"}
COUNTS = tuple(f"{side}.{name}" for side in ("client", "server") for name in COUNT_NAMES.values())
METRICS = GOODPUT + LATENCY + COUNTS
WARNING = ("Provisional local calibration and baseline evidence; not independent certification. "
           "All-observed statistics retain failed and invalid trials and are diagnostic. "
           "Latency values are medians of per-trial percentiles, never pooled percentiles; "
           "they cover observed responses only, so review missed probes and timeouts separately.")


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def metric_summary(trials, name):
    values = [r["metrics"].get(name) for r in trials if finite(r["metrics"].get(name))]
    unit = "Mbit/s" if name in GOODPUT else "ms" if name in LATENCY else "count"
    result = {"unit": unit, "contributing_trials": len(values), "missing_trials": len(trials) - len(values),
              "median": statistics.median(values) if values else None,
              "min": min(values) if values else None, "max": max(values) if values else None}
    if name in COUNTS:
        result["sum"] = sum(values) if values else None
    return result


def subset_summary(trials):
    return {"trial_count": len(trials), "trial_ids": [r["trial_id"] for r in trials],
            "metrics": {name: metric_summary(trials, name) for name in METRICS},
            "probe_counts": {side: {label: metric_summary(trials, f"{side}.{name}")
                                    for label, name in COUNT_NAMES.items()}
                             for side in ("client", "server")}}


def trial_counts(trials, expected=None):
    return {"expected_trials": expected, "observed_trials": len(trials),
            "validated_lab_complete_trials": sum(r["validated_lab_complete"] for r in trials),
            "artifact_intact_timing_valid_complete_trials": sum(r["artifact_intact_timing_valid_complete"] for r in trials),
            "sut_failed_trials": sum(r.get("sut_outcome") == "FAIL" for r in trials),
            "sut_incomplete_or_unknown_trials": sum(r.get("sut_outcome") not in ("COMPLETE", "FAIL") for r in trials),
            "invalid_harness_trials": sum(r["original_validity"] == "INVALID_HARNESS" for r in trials),
            "invalid_artifact_trials": sum(r.get("validity") == "INVALID_ARTIFACT" for r in trials),
            "integrity_failed_trials": sum(r["integrity_verified"] is False for r in trials),
            "integrity_unknown_trials": sum(r["integrity_verified"] is None for r in trials),
            "by_validity": dict(sorted(Counter(r.get("validity", "UNKNOWN") for r in trials).items())),
            "by_outcome": dict(sorted(Counter(r.get("sut_outcome", "UNKNOWN") for r in trials).items()))}


def reason_counts(trials, key):
    return [{"reason": reason, "trial_count": len(ids), "trial_ids": ids}
            for reason, ids in sorted(_reason_ids(trials, key).items())]


def _reason_ids(trials, key):
    result = defaultdict(list)
    for trial in trials:
        for reason in dict.fromkeys(trial.get(key, [])):
            result[str(reason)].append(trial["trial_id"])
    return result


def safe_read(path):
    try:
        value = read_json(path)
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def normalized_trial(row, folder, planned):
    """Retain original harness reasons when collect replaces validity after an audit failure."""
    original = safe_read(folder / "summary.json") if folder else {}
    saved = original or planned
    manifest = safe_read(folder / "manifest.json") if folder else {}
    case = manifest.get("invariants", {}).get("case", {})
    result = dict(row)
    for key in ("case_id", "variant", "comparison_key", "seed", "commit", "binary_sha256", "scope", "utc"):
        if result.get(key) is None:
            result[key] = planned.get(key, manifest.get(key))
    result["case_id"] = result.get("case_id") or case.get("id") or "UNKNOWN"
    result["variant"] = result.get("variant") or "UNKNOWN"
    result["comparison_key"] = result.get("comparison_key") or "UNKNOWN"
    result["profile_id"] = manifest.get("profile_id", planned.get("profile_id"))
    result["workload"] = case.get("workload", planned.get("workload"))
    result["artifact_directory"] = str(folder) if folder else None
    result["diagnostic_summary_source"] = "trial_summary" if original else "campaign_record" if planned else "unavailable"
    result["original_validity"] = saved.get("validity", result.get("validity", "UNKNOWN"))
    result["harness_reasons"] = list(saved.get("reasons", row.get("reasons", []))) if result["original_validity"] == "INVALID_HARNESS" else []
    result["artifact_reasons"] = list(row.get("reasons", [])) if row.get("validity") == "INVALID_ARTIFACT" else []
    if result.get("sut_outcome") is None:
        result["sut_outcome"] = saved.get("sut_outcome", "UNKNOWN")
    result["sut_failures"] = list(saved.get("sut_failures", row.get("sut_failures", [])))
    result["calibration_notes"] = list(saved.get("calibration_notes", row.get("calibration_notes", [])))
    result["integrity_verified"] = original.get("integrity_verified", row.get("integrity_verified"))
    failures = list(original.get("integrity_failures", []))
    if result["integrity_verified"] is False:
        # Type1's summary folds integrity failures into sut_failures; probe service is separate.
        failures.extend(x for x in result["sut_failures"] if x != "probe deadlines unserved/unobserved")
        if folder:
            for index in (0, 1):
                endpoint = safe_read(folder / f"endpoint-{index}.result.json")
                if endpoint and not endpoint.get("completed"):
                    failures.append(f"endpoint {index}: {endpoint.get('error', 'incomplete endpoint')}")
        if not failures:
            failures.append("application integrity verification did not pass")
    result["integrity_failures"] = list(dict.fromkeys(failures))
    result["metrics"] = dict(row.get("metrics", {}))
    if result["integrity_verified"] is not True:
        for name in GOODPUT:
            result["metrics"][name] = None
    result["validated_lab_complete"] = (result.get("validity") == "VALIDATED_LAB"
                                         and result.get("sut_outcome") == "COMPLETE"
                                         and result["integrity_verified"] is True)
    result["artifact_intact_timing_valid_complete"] = (result.get("validity") in ("VALIDATED_LAB", "PROVISIONAL_UNCALIBRATED")
                                                        and result.get("sut_outcome") == "COMPLETE"
                                                        and result["integrity_verified"] is True)
    return result


def aggregate_campaign(root):
    root = Path(root).expanduser().resolve()
    campaign = read_json(root / "campaign.json")
    if not isinstance(campaign, dict) or not isinstance(campaign.get("trials"), list):
        raise ValueError("input must contain a completed campaign.json with a trials list")
    planned = {r["trial_id"]: r for r in campaign["trials"] if isinstance(r, dict) and isinstance(r.get("trial_id"), str)}
    if len(planned) != len(campaign["trials"]):
        raise ValueError("campaign trial IDs must be present and unique")
    folders = {}
    for folder in {p.parent for name in ("manifest.json", "summary.json") for p in root.rglob(name)}:
        if folder.name in folders and folders[folder.name] != folder:
            raise ValueError(f"duplicate trial directory name: {folder.name}")
        folders[folder.name] = folder
    for ident in planned:
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", ident):
            folders.setdefault(ident, root / ident)
        else:
            raise ValueError(f"unsafe planned trial ID: {ident!r}")
    trials = [normalized_trial(row, folders.get(row["trial_id"]), planned.get(row["trial_id"], {}))
              for row in collect(root)]
    bycase = defaultdict(list)
    for trial in trials:
        bycase[(trial["case_id"], trial["variant"], trial["comparison_key"])].append(trial)
    cases = []
    for (case_id, variant, comparison_key), rows in sorted(bycase.items()):
        planned_ids = {r["trial_id"] for r in rows} & planned.keys()
        cases.append({"case_id": case_id, "variant": variant, "comparison_key": comparison_key,
                      "profile_id": next((r["profile_id"] for r in rows if r["profile_id"]), None),
                      "workload": next((r["workload"] for r in rows if r["workload"]), None),
                      "counts": trial_counts(rows, len(planned_ids)),
                      "all_observed": subset_summary(rows),
                      "validated_lab_complete_only": subset_summary([r for r in rows if r["validated_lab_complete"]]),
                      "artifact_intact_timing_valid_complete": subset_summary([r for r in rows if r["artifact_intact_timing_valid_complete"]]),
                      "sut_failures": reason_counts(rows, "sut_failures"),
                      "invalid_harness_reasons": reason_counts(rows, "harness_reasons"),
                      "artifact_failures": reason_counts(rows, "artifact_reasons"),
                      "integrity_failures": reason_counts(rows, "integrity_failures")})
    return {"version": 1, "input": str(root), "scope": "provisional_local_transport_baseline",
            "warning": WARNING,
            "aggregation": "median/min/max of whole-trial statistics; percentile samples are never pooled",
            "subset_definitions": {
                "all_observed": "every collected trial record, including failures, invalid artifacts, and missing-trial placeholders",
                "validated_lab_complete_only": "VALIDATED_LAB, COMPLETE, and integrity_verified=true after artifact audit; local operator acceptance",
                "artifact_intact_timing_valid_complete": "PROVISIONAL_UNCALIBRATED or VALIDATED_LAB, COMPLETE, and integrity_verified=true after artifact audit; calibration can remain provisional"},
            "campaign_declared_pass": campaign.get("pass"), "counts": trial_counts(trials, len(planned)),
            "cases": cases, "trials": trials}


def format_metric(metric):
    if metric["median"] is None:
        return "—"
    return f"{metric['median']:.3f} [{metric['min']:.3f}–{metric['max']:.3f}]"


def markdown_cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def markdown_report(report):
    counts = report["counts"]
    lines = ["# Transport baseline campaign", "", WARNING, "",
             f"Trials: {counts['observed_trials']}/{counts['expected_trials']} planned; "
             f"{counts['artifact_intact_timing_valid_complete_trials']} artifact-intact timing-valid complete; "
             f"{counts['validated_lab_complete_trials']} with local accepted calibration; {counts['sut_failed_trials']} SUT failures; "
             f"{counts['invalid_harness_trials']} invalid harness; {counts['invalid_artifact_trials']} invalid artifacts; "
             f"{counts['integrity_failed_trials']} integrity failures.", "",
             "Numbers below are median [min–max] across repeats. Goodput is verified payload Mbit/s; latency is ms.", ""]
    for subset, title in (("all_observed", "All observed (includes failed/invalid trials)"),
                          ("artifact_intact_timing_valid_complete", "Artifact-intact timing-valid complete (calibration can remain provisional)"),
                          ("validated_lab_complete_only", "Validated lab complete only (local accepted calibration)")):
        lines += [f"## {title}", "", "| Case / profile / workload | Trials | C→S Mbit/s | S→C Mbit/s | RTT p95 / p99 | Scheduled p95 / p99 |", "|---|---:|---:|---:|---|---|"]
        for case in report["cases"]:
            data = case[subset]
            metrics = data["metrics"]
            ident = f"{case['case_id']} / {case.get('profile_id') or '?'} / {case.get('workload') or '?'} ({case['variant']})"
            pair = lambda kind: " / ".join(format_metric(metrics[f"client.{kind}.{q}"]) for q in ("p95", "p99"))
            lines.append(f"| {markdown_cell(ident)} | {data['trial_count']} | {format_metric(metrics[GOODPUT[0]])} | {format_metric(metrics[GOODPUT[1]])} | {pair('probe_rtt_ms')} | {pair('probe_scheduled_ms')} |")
        lines.append("")
    lines += ["## Probe accounting (all observed)", "", "| Case | Expected | Injected | Observed | Missed | Timeouts |", "|---|---:|---:|---:|---:|---:|"]
    for case in report["cases"]:
        totals = []
        for name in ("expected", "injected", "observed", "missed", "timeouts"):
            values = [case["all_observed"]["probe_counts"][side][name] for side in ("client", "server")]
            total = sum(v["sum"] for v in values if v["sum"] is not None)
            unknown = sum(v["missing_trials"] for v in values)
            totals.append(f"{total:g}" + (f" (+{unknown} unknown endpoint trials)" if unknown else ""))
        lines.append(f"| {markdown_cell(case['case_id'])} | " + " | ".join(totals) + " |")
    failures = [(case["case_id"], kind, item) for case in report["cases"]
                for kind in ("sut_failures", "invalid_harness_reasons", "artifact_failures", "integrity_failures")
                for item in case[kind]]
    lines += ["", "## Failures and invalidity", ""]
    if failures:
        for case, kind, item in failures:
            lines.append(f"- {markdown_cell(case)}: {kind} — {markdown_cell(item['reason'])} ({item['trial_count']} trials).")
    else:
        lines.append("No SUT, harness, artifact, or integrity failure reasons were recorded.")
    lines += ["", "Machine-readable JSON retains every trial, its audit status, reasons, and metric coverage. "
              "CSV uses one row per case, subset, and metric; missing values remain empty.", ""]
    return "\n".join(lines)


def write_report(report, output):
    output = Path(output).expanduser().resolve()
    root = Path(report["input"])
    if output == root or output.is_relative_to(root):
        raise ValueError("output must be outside the input campaign to preserve artifact seals")
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "aggregate.json", report)
    columns = ["case_id", "profile_id", "workload", "variant", "comparison_key", "subset", "trial_count",
               "expected_trials", "observed_trials", "sut_failed_trials", "invalid_harness_trials",
               "invalid_artifact_trials", "integrity_failed_trials", "metric", "unit", "contributing_trials",
               "missing_trials", "median", "min", "max", "sum"]
    with (output / "aggregate.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for case in report["cases"]:
            for subset in ("all_observed", "artifact_intact_timing_valid_complete", "validated_lab_complete_only"):
                data = case[subset]
                for metric, stats in data["metrics"].items():
                    row = {k: case.get(k) for k in ("case_id", "profile_id", "workload", "variant", "comparison_key")}
                    row.update({k: case["counts"][k] for k in columns if k in case["counts"]})
                    row.update(subset=subset, trial_count=data["trial_count"], metric=metric, **stats)
                    writer.writerow(row)
    (output / "report.md").write_text(markdown_report(report), encoding="utf-8")
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="completed campaign directory")
    parser.add_argument("--output", required=True, help="report directory outside the campaign")
    args = parser.parse_args(argv)
    try:
        report = aggregate_campaign(args.input)
        output = write_report(report, args.output)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    print(json.dumps({"report": str(output / "report.md"), "aggregate": str(output / "aggregate.json"),
                      "counts": report["counts"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
