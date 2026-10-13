#!/usr/bin/env python3
"""Combine retained live RDP phases without modifying capture or calibration evidence.

Inputs are freerdp.live_benchmark v1 envelopes. Failed preflight status records
can be imported separately; they contribute no invented measurements. Latest
calibration is displayed explicitly while earlier embedded evidence is retained.
"""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
from collections import Counter, defaultdict
from pathlib import Path

from dashboard import comparisons, finite, normalize_trial, read, stats


def identity(path, purpose):
    path = Path(path)
    return {"file": path.name, "directory": path.parent.name, "purpose": purpose,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def source_reports(paths):
    out = []
    for path in paths:
        source = read(path)
        if not isinstance(source, dict) or source.get("schema") != "freerdp.live_benchmark" or source.get("version") != 1 or not isinstance(source.get("trials"), list):
            raise ValueError(f"{path}: expected freerdp.live_benchmark version 1")
        out.append((Path(path), source))
    return out


def bridge_end_evidence(row):
    """Retain explicit launcher end evidence without guessing from TLS warnings."""
    if row.get("sut_outcome") != "INCOMPLETE":
        return None
    artifacts = row.get("artifacts") or {}
    root, bridge = artifacts.get("root"), artifacts.get("bridge")
    if not root or not bridge:
        return {"available": False, "reason": "No bridge artifact reference in supplied status"}
    path = Path(bridge)
    if not path.is_absolute():
        path = Path(root) / path
    try:
        payload = path.read_bytes()
        records = [json.loads(line) for line in payload.decode("utf-8").splitlines() if line.strip()]
    except (OSError, UnicodeError, ValueError) as error:
        return {"available": False, "reason": f"Bridge evidence unavailable: {type(error).__name__}"}
    ended = [record.get("event", record) for record in records
             if isinstance(record, dict) and isinstance(record.get("event", record), dict)
             and record.get("event", record).get("type") == "ended"]
    if not ended:
        return {"available": False, "sha256": hashlib.sha256(payload).hexdigest(), "reason": "No terminal bridge ended event"}
    event = ended[-1]
    classification = "owned_cancellation_after_connection" if event.get("hadConnected") is True and event.get("outcome") == "cancelled" else "other_terminal_evidence"
    if event.get("hadConnected") is False and "Selected display is unavailable or ambiguous" in (event.get("detail") or ""):
        classification = "selected_display_unavailable"
    return {"available": True, "file": path.name, "sha256": hashlib.sha256(payload).hexdigest(),
            "classification": classification, "event": {key: event.get(key) for key in
                                                          ("type", "outcome", "hadConnected", "detail", "errorCode", "errorInfo")},
            "scope": "Explicit terminal launcher evidence; does not replace controller or observer failure reasons"}


def coverage_view(trials, path):
    """Audit an explicit condition selection without copying trials or changing phases."""
    payload = read(path)
    ids = payload.get("selected_trial_ids") if isinstance(payload, dict) else None
    if not isinstance(ids, list) or not ids or not all(isinstance(ident, str) for ident in ids) or len(ids) != len(set(ids)):
        raise ValueError("coverage requires unique explicit selected_trial_ids")
    by_id = {row["trial_id"]: row for row in trials}
    if any(ident not in by_id for ident in ids):
        raise ValueError("coverage refers to a trial absent from the imported reports")
    rows = [by_id[ident] for ident in ids]
    conditions = {(row["profile_id"], row["case_id"], row.get("seed")) for row in rows}
    if len(conditions) != len(rows) or any(row["variant"] != "candidate" or row.get("seed") is None for row in rows):
        raise ValueError("coverage must select unique candidate profile/case/seed conditions")
    for row in rows:
        counts = row["probes"]
        expected = counts["expected"]
        if not row.get("measurement_evidence_available") or expected is None or expected <= 0 or counts["injected"] != expected or counts["observed"] != expected:
            raise ValueError(f"coverage {row['trial_id']}: complete observed input cohort is required")
        if any(counts[name] != 0 for name in ("missed", "unobserved", "timeouts")):
            raise ValueError(f"coverage {row['trial_id']}: misses, unobserved probes or timeouts")
    counts = {"selected_trials": len(rows), "expected": sum(row["probes"]["expected"] for row in rows),
              "injected": sum(row["probes"]["injected"] for row in rows), "observed": sum(row["probes"]["observed"] for row in rows),
              "missed_injections": 0, "timeouts": 0,
              "complete": sum(row["sut_outcome"] == "COMPLETE" for row in rows),
              "incomplete": sum(row["sut_outcome"] == "INCOMPLETE" for row in rows),
              "strict_eligible": sum(row["strict_eligible"] for row in rows)}
    for key in ("expected", "observed", "missed_injections", "timeouts", "complete", "incomplete"):
        if key in payload and payload[key] != counts[key]:
            raise ValueError(f"coverage {key} claim differs from selected trial evidence")
    groups = []
    for profile in sorted({row["profile_id"] for row in rows}):
        group = [row for row in rows if row["profile_id"] == profile]
        metrics = {name: stats([row["metrics"].get(name) for row in group])
                   for name in ("input_visible_ms.p95", "input_visible_ms.p99", "input_scheduled_ms.p99",
                                "progress.observed_hz", "verified_goodput_mbps")}
        groups.append({"profile_id": profile, "N": len(group), "seeds": sorted(row["seed"] for row in group),
                       "selected_trial_ids": [row["trial_id"] for row in group], "metrics": metrics,
                       "source_phases": sorted({row["phase"] for row in group}),
                       "complete": sum(row["sut_outcome"] == "COMPLETE" for row in group),
                       "incomplete": sum(row["sut_outcome"] == "INCOMPLETE" for row in group)})
    for supplied in payload.get("groups") or []:
        group = next((group for group in groups if group["profile_id"] == supplied.get("profile")), None)
        if group is None or group["N"] != supplied.get("N") or group["seeds"] != supplied.get("seeds"):
            raise ValueError("coverage group membership differs from selected trial evidence")
        for name, metric in (("median_trial_p95_ms", "input_visible_ms.p95"), ("median_trial_p99_ms", "input_visible_ms.p99")):
            if name in supplied and (not finite(supplied[name]) or not math.isclose(supplied[name], group["metrics"][metric]["median"], rel_tol=1e-10, abs_tol=1e-9)):
                raise ValueError(f"coverage {name} differs from whole-trial median")
    return {"id": "candidate-condition-coverage", "label": "Candidate baseline · 10 conditions" if len(rows) == 10 else f"Candidate condition coverage · {len(rows)} trials",
            "scope": payload.get("scope", "Explicit candidate condition coverage across retained phases; provisional"),
            "source": identity(path, "explicit selected candidate conditions"), "selected_trial_ids": ids,
            "source_phases": sorted({row["phase"] for row in rows}), "counts": counts, "groups": groups,
            "notes": ["Coverage selects retained trial IDs; it does not create new trials or rewrite their original phase, source fingerprints or eligibility.",
                      "Helper versions differ across the selected phases. The combined condition view is provisional and provides no causal reference comparison.",
                      "Profile summaries are medians/ranges of whole-trial p95/p99 values; response samples are never pooled.",
                      "Incomplete final status acknowledgement remains a recorded failure even when all input responses were observed."]}


def combine(paths, preflights=None, calibration=None, coverage=None):
    reports = source_reports(paths)
    if not reports:
        raise ValueError("at least one live report is required")
    trials, ids, notes, sources, provenance, calibration_history = [], set(), [], [], [], []
    profiles, declarations, phases = {}, [], []
    for path, source in reports:
        source_identity = identity(path, "recorded live phase")
        sources.append(source_identity)
        provenance.append({"source": source_identity, "campaign_id": source.get("campaign_id"),
                           "provenance": copy.deepcopy(source.get("provenance")),
                           "environment_preflight": copy.deepcopy(source.get("environment_preflight"))})
        if source.get("calibration"):
            calibration_history.append({"source": source_identity, "evidence": copy.deepcopy(source["calibration"])})
        for row in source["trials"]:
            ident = row.get("trial_id")
            if not isinstance(ident, str) or ident in ids:
                raise ValueError(f"{path}: missing or duplicate trial_id {ident}")
            ids.add(ident)
            saved = copy.deepcopy(row)
            if not saved.get("phase"):
                saved["phase"] = source.get("campaign_id") or path.parent.name
            saved["source_report"] = source_identity
            if "planned trial has no completed summary" in (saved.get("reasons") or []):
                folder = path.parent / ident
                present = {name: (folder / name).is_file() for name in ("manifest.json", "timeline.jsonl", "bridge.jsonl", "summary.json")}
                saved["completion_evidence"] = {"summary_completed": False, "artifact_presence": present,
                                                "status": "interrupted_attempt_without_summary" if present["bridge.jsonl"] or present["timeline.jsonl"] else "no_recorded_start",
                                                "scope": "Planned row without completed summary; no measurements inferred"}
            bridge = bridge_end_evidence(saved)
            if bridge:
                saved["derived_bridge_evidence"] = bridge
                if bridge.get("classification") == "selected_display_unavailable":
                    saved["reasons"] = list(dict.fromkeys((saved.get("reasons") or []) + ["Bridge ended before connection: selected display unavailable or ambiguous"]))
            trials.append(saved)
        notes.extend(source.get("notes") or [])
        phases.extend(copy.deepcopy(source.get("phases") or []))
        for profile in source.get("network_profiles") or []:
            ident = profile.get("id")
            if ident in profiles and profiles[ident] != profile:
                raise ValueError(f"profile {ident} changed between live reports; use distinct IDs")
            profiles[ident] = copy.deepcopy(profile)
        declarations.extend(copy.deepcopy(source.get("comparisons") or []))
    imported_preflights = []
    if preflights:
        status = read(preflights)
        if not isinstance(status, dict):
            raise ValueError("preflight input must contain phase status objects")
        source_identity = identity(preflights, "preserved failed preflight statuses; no measurements imported")
        sources.append(source_identity)
        for phase, group in status.items():
            if not isinstance(group, dict) or not isinstance(group.get("outcomes"), list):
                raise ValueError(f"preflight {phase}: outcomes array is required")
            if group.get("trials") != len(group["outcomes"]):
                raise ValueError(f"preflight {phase}: trial count does not match outcomes")
            phases.append({"phase": "preflight-" + phase, "expected_trials": group["trials"],
                           "evidence_scope": "preserved setup failures; no measured-phase performance"})
            for outcome in group["outcomes"]:
                ident = outcome.get("trial_id")
                if not isinstance(ident, str) or ident in ids:
                    raise ValueError(f"preflight input repeats or omits trial_id {ident}")
                ids.add(ident)
                # The status sidecar lacks measured trial identity and metrics.
                # Preserve that absence rather than infer values from trial names.
                row = copy.deepcopy(outcome)
                row.update(phase="preflight-" + phase, original_phase=phase,
                           case_id="failed-setup", profile_id="unknown", variant="unknown",
                           seed=None, comparison_key=None, integrity_verified=False,
                           strict_eligible=False, all_observed_eligible=False,
                           metrics={"verified_goodput_mbps": None}, probes={}, source_report=source_identity,
                           evidence_scope="Preserved failed setup status only; no measured-phase values imported")
                trials.append(row)
                imported_preflights.append(ident)
    normalized = [normalize_trial(row, live=True) for row in trials]
    # Use explicit recorded variant identities, not trial-name guesses.
    declared_variants = set()
    for _, source in reports:
        variants = source.get("provenance", {}).get("variants", [])
        if isinstance(variants, list):
            declared_variants.update(v.get("id") for v in variants if isinstance(v, dict))
        elif isinstance(variants, dict):
            declared_variants.update(variants)
    if not declarations and {"reference", "candidate"} <= declared_variants:
        names = sorted({name for row in normalized for name in row["metrics"]})
        configurations = {(row["phase"], row["case_id"], row.get("comparison_key"))
                          for row in normalized if row.get("comparison_key")}
        # A candidate-only phase never inherits a comparison from older phases.
        paired_configurations = {key for key in configurations if {"reference", "candidate"} <=
                                 {row["variant"] for row in normalized if (row["phase"], row["case_id"], row.get("comparison_key")) == key}}
        declarations = [dict(phase=phase, case_id=case, comparison_key=config,
                             baseline_variant="reference", candidate_variant="candidate", metric_names=names)
                        for phase, case, config in sorted(paired_configurations)]
        notes.append("Reference/candidate comparisons use recorded provenance variant IDs and matching phase/case/configuration/seed; all-observed values remain diagnostic.")
    elif not declarations:
        for _, source in reports:
            if source.get("comparison_variants"):
                variants = source["comparison_variants"]
                names = sorted({name for row in normalized for name in row["metrics"]})
                declarations.extend(dict(phase=phase, case_id=case, comparison_key=config,
                                         baseline_variant=variants["baseline"], candidate_variant=variants["candidate"], metric_names=names)
                                    for phase, case, config in sorted({(row["phase"], row["case_id"], row.get("comparison_key"))
                                                                    for row in normalized if row.get("comparison_key")}))
    selected_calibration = copy.deepcopy(reports[-1][1].get("calibration") or {"approved": False})
    if calibration:
        latest = read(calibration)
        if not isinstance(latest, dict) or not isinstance(latest.get("samples"), list):
            raise ValueError("latest calibration requires a samples array")
        passed = sum(row.get("pass") is True for row in latest["samples"])
        if latest.get("sample_count") != len(latest["samples"]) or latest.get("strict_passed") != passed:
            raise ValueError("latest calibration counts disagree with retained sample records")
        source_identity = identity(calibration, "latest actual VM UDP calibration")
        sources.append(source_identity)
        selected_calibration = {"approved": latest.get("accepted_calibration_receipt") is True and latest.get("strict_pass") is True,
                                "source": source_identity, "scope": latest.get("scope"), "strict_passed": passed,
                                "sample_count": len(latest["samples"]), "failed_sample_count": len(latest["samples"]) - passed,
                                "failed_records": [copy.deepcopy(row) for row in latest["samples"] if row.get("pass") is not True],
                                "latest_evidence": copy.deepcopy(latest), "history": calibration_history,
                                "notes": ["Latest calibration overrides the display, not the preserved older source evidence.",
                                          "CRC UDP echo payload rates are calibration only; they are not RDP payload goodput."]}
    else:
        selected_calibration["history"] = calibration_history
    counts = {"trials": len(normalized), "outcomes": dict(Counter(row["sut_outcome"] for row in normalized)),
              "validity": dict(Counter(row["validity"] for row in normalized)),
              "strict_eligible": sum(row["strict_eligible"] for row in normalized),
              "preflight_statuses": len(imported_preflights)}
    counts["bridge_end_classifications"] = dict(Counter(row.get("derived_bridge_evidence", {}).get("classification", "unknown")
                                                       for row in trials if row.get("sut_outcome") == "INCOMPLETE" and row.get("derived_bridge_evidence")))
    counts["planned_without_summary"] = sum(row.get("completion_evidence", {}).get("summary_completed") is False for row in trials)
    notes.extend(["Failed preflight statuses remain distinct phases without numeric measurement evidence.",
                  "Visible input-response percentiles describe observed probes; failures, misses, unobserved/censored IDs and timeouts stay separate.",
                  "UDP-only shaping does not certify a full-RDP 10 Mbit/s profile. TCP and graphics routing require their recorded evidence.",
                  "Network traffic and SDL texture upload bytes are not verified RDP application goodput."])
    preferred_phase = next((row.get("phase") for row in reversed(trials) if row.get("source_report") == provenance[-1]["source"]), None)
    selected_coverage = coverage_view(normalized, coverage) if coverage else None
    return {"schema": "freerdp.live_benchmark", "version": 1, "scope": "Live Windows RDP; UDP-only network profiles, TCP unshaped; measured graphics routing may remain unproved",
            "campaign_id": "combined-live-vm", "provenance": {"sources": sources, "phase_provenance": provenance},
            "preferred_phase": preferred_phase,
            "coverage": selected_coverage,
            "network_profiles": list(profiles.values()), "calibration": selected_calibration,
            "counts": counts, "phases": phases, "trials": trials,
            "comparisons": comparisons(normalized, declarations),
            "notes": list(dict.fromkeys(notes))}


def output_guard(output, inputs):
    output = Path(output).resolve()
    for source in inputs:
        source = Path(source).resolve()
        if output == source.parent or output in source.parents:
            raise ValueError(f"output must be separate from input evidence directory: {source}")
    return output


def report_markdown(report):
    lines = ["# Live Windows RDP campaign", "", report["scope"], "",
             "Whole-trial percentiles remain separate. Group values below are medians of per-trial statistics, not pooled latency percentiles.", "",
             "| Phase | Profile / case | Variant | Trials | Complete / incomplete / fail | Strict eligible | Observed / injected | Visible p95 / p99 ms | Verified RDP goodput Mbit/s |",
             "|---|---|---|---:|---|---:|---|---|---|"]
    groups = defaultdict(list)
    for trial in report["trials"]:
        row = normalize_trial(trial, live=True)
        groups[(row["phase"], row["profile_id"], row["case_id"], row["variant"])].append(row)
    def fmt(value):
        return f"{value:.3f}" if finite(value) else "unavailable"
    for (phase, profile, case, variant), rows in sorted(groups.items()):
        outcomes = Counter(row["sut_outcome"] for row in rows)
        observed = [row["probes"]["observed"] for row in rows if row["probes"]["observed"] is not None]
        injected = [row["probes"]["injected"] for row in rows if row["probes"]["injected"] is not None]
        probes = f"{sum(observed)} / {sum(injected)}" if len(observed) == len(rows) == len(injected) else "unknown"
        p95, p99, goodput = [stats([row["metrics"].get(name) for row in rows])["median"]
                             for name in ("input_visible_ms.p95", "input_visible_ms.p99", "verified_goodput_mbps")]
        lines.append(f"| {phase} | {profile} / {case} | {variant} | {len(rows)} | {outcomes['COMPLETE']} / {outcomes['INCOMPLETE']} / {outcomes['FAIL']} | {sum(row['strict_eligible'] for row in rows)} | {probes} | {fmt(p95)} / {fmt(p99)} | {fmt(goodput)} |")
    calibration = report["calibration"]
    coverage = report.get("coverage")
    if coverage:
        lines.extend(["", "## Selected candidate condition coverage", "", coverage["scope"], "",
                      f"Selected {coverage['counts']['selected_trials']} distinct conditions: {coverage['counts']['observed']}/{coverage['counts']['expected']} responses, {coverage['counts']['missed_injections']} misses, {coverage['counts']['timeouts']} timeouts; {coverage['counts']['complete']} COMPLETE and {coverage['counts']['incomplete']} INCOMPLETE. Source phases and helper versions remain distinct.", "",
                      "| Profile | Selected trials | Median trial p95 ms | Median trial p99 ms | Verified RDP goodput |",
                      "|---|---:|---:|---:|---|"])
        for group in coverage["groups"]:
            lines.append(f"| {group['profile_id']} | {group['N']} | {fmt(group['metrics']['input_visible_ms.p95']['median'])} | {fmt(group['metrics']['input_visible_ms.p99']['median'])} | {fmt(group['metrics']['verified_goodput_mbps']['median'])} |")
        lines.extend(["", *["- " + note for note in coverage["notes"]], ""])
    lines.extend(["", f"Latest calibration: {calibration.get('strict_passed', 'unknown')}/{calibration.get('sample_count', 'unknown')} strict samples; accepted receipt: {calibration.get('approved') is True}.", "",
                  "## Retained failures and partial evidence", ""])
    classifications = report["counts"].get("bridge_end_classifications") or {}
    if classifications:
        lines.extend(["Explicit incomplete-trial bridge end evidence: " + ", ".join(f"{key}={value}" for key, value in sorted(classifications.items())) + ". Normal certificate warnings are not treated as a failure cause.", ""])
    for row in report["trials"]:
        if row.get("sut_outcome") != "COMPLETE" or row.get("reasons") or row.get("strict_eligible") is False:
            lines.append(f"- {row['trial_id']} ({row.get('phase')}, {row.get('sut_outcome')}, {row.get('validity')}): " + "; ".join(row.get("reasons") or ["Outside strict comparison subset"]))
    lines.extend(["", "## Measurement limits", "", *["- " + note for note in report["notes"]], ""])
    return "\n".join(lines)


def export(report, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "live-results.json").write_text(json.dumps(report, ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n", encoding="utf-8")
    (output / "report.md").write_text(report_markdown(report), encoding="utf-8")
    fields = ("trial_id", "phase", "case_id", "profile_id", "variant", "seed", "commit", "binary_sha256",
              "comparison_key", "validity", "sut_outcome", "strict_eligible", "measurement_evidence_available",
              "metric", "value", "reasons")
    with (output / "trials.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for trial in report["trials"]:
            row = normalize_trial(trial, live=True)
            metrics = {**row["metrics"], **{"probes." + key: value for key, value in row["probes"].items()}}
            for name, value in metrics.items():
                encoded = {key: row.get(key) for key in fields}
                encoded.update(metric=name, value=value, reasons="; ".join(row["reasons"]))
                writer.writerow(encoded)
    return output / "live-results.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, action="append", required=True, help="repeat for smoke and main phase reports")
    parser.add_argument("--preflights", type=Path, help="retained failed-live-preflights.json status sidecar")
    parser.add_argument("--calibration", type=Path, help="latest actual VM UDP calibration summary")
    parser.add_argument("--coverage", type=Path, help="explicit selected candidate condition IDs; raw phases remain unchanged")
    parser.add_argument("--output", type=Path, required=True, help="separate derived report directory")
    args = parser.parse_args()
    try:
        output = output_guard(args.output, args.input + [p for p in (args.preflights, args.calibration, args.coverage) if p])
        report = combine(args.input, args.preflights, args.calibration, args.coverage)
        path = export(report, output)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f"live report: {error}\n")
    print(json.dumps({"report": str(path), "counts": report["counts"]}))


if __name__ == "__main__":
    main()
