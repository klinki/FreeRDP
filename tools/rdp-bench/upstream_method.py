#!/usr/bin/env python3
"""Record and summarize the OpenThinRDP server-counter loss ramp.

This adapter does not shape traffic, launch a VM or infer successful delivery
from a running client. The owner of those processes records each step's outcome.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from datetime import datetime, timezone
from pathlib import Path

SOURCE_URL = "https://github.com/OpenThinRdp/openthinrdp/tree/main/tests/udp-bench"
SOURCE_FILES = ("README.md", "session.sh", "line.sh", "ramp.sh", "load.ps1", "counters.ps1")
METRICS = {
    "server_udp_bandwidth": "upstream display: kbps; raw cooked counter",
    "server_udp_sent_rate": "raw cooked counter; upstream does not specify a unit",
    "server_udp_rtt_ms": "ms",
    "server_loss_percent": "percent",
    "server_retransmission_percent": "percent",
    "server_output_fps": "frames/s produced by server; not physical client FPS",
    "server_skipped_network_fps": "frames/s skipped by server for network resources",
}


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def finite(value):
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value)


def timestamp(value):
    if not isinstance(value, str):
        raise ValueError("UTC timestamps must be strings with timezone offsets")
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("timestamp has no timezone offset")
    return parsed.astimezone(timezone.utc)


def numeric_stats(values):
    values = [value for value in values if finite(value)]
    return {"n": len(values), "mean": statistics.mean(values) if values else None,
            "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None}


def plan(source_dir, losses=(25, 50, 70), step_seconds=120):
    if not finite(step_seconds) or step_seconds <= 0:
        raise ValueError("step duration must be positive")
    if not losses or any(not finite(loss) or not 0 <= loss <= 100 for loss in losses):
        raise ValueError("loss must be a percentage from 0 through 100")
    source_dir = Path(source_dir)
    files = [{"name": name, "sha256": hashlib.sha256((source_dir / name).read_bytes()).hexdigest()}
             for name in SOURCE_FILES]
    return {"schema_version": 1, "method": "OpenThinRDP tests/udp-bench",
            "source_url": SOURCE_URL, "source_files": files,
            "requested_loss_percent": list(losses), "seconds_per_step": step_seconds,
            "upstream_default_loss_percent": [50, 70, 80],
            "session": {"width": 1280, "height": 800, "bpp": 32,
                        "gfx": "progressive,AVC420:off,AVC444:off", "network": "auto",
                        "connect_timeout_ms": 20000, "client_surface": "Linux Xvfb :99"},
            "workload": {"script": "load.ps1", "bitmap_width": 240, "bitmap_height": 135,
                         "timer_interval_ms": 33, "fullscreen_coloured_noise": True},
            "network": {"direction": "server to client only", "protocol_scope": "all IPv4 from VM",
                        "implementation": "Linux ingress tc/netem via IFB",
                        "rate_cap_mbps": None,
                        "large_ip_packet_min_bytes": 512,
                        "large_packet_extra_delay_ms": 1, "large_packet_reorder_percent": 70,
                        "small_packet_extra_delay_ms": 0, "small_packet_reorder_percent": 0,
                        "netem_limit_packets": 100000,
                        "verification": "100 pings each at payload sizes 56 and 1400 before each timed step"},
            "counter_interval_s": 10, "metrics": METRICS,
            "notes": ["25/50/70 substitutes the user-requested loss levels for upstream's 50/70/80.",
                      "Network setup and ping verification precede the 120-second timed step.",
                      "Stop the ramp at the first client process termination, as upstream does.",
                      "A live process does not establish an active UDP tunnel or visible client frames.",
                      "Server counters and network byte counts do not verify application goodput.",
                      "Mac UDP-only shaping is an adaptation, not the full upstream packet-loss method."]}


def read_counter_rows(path):
    records, errors = [], []
    with Path(path).open(encoding="utf-8-sig") as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise ValueError("row is not an object")
                records.append(row)
            except (ValueError, TypeError) as error:
                errors.append({"line": number, "error": str(error)})
    return records, errors


def sample_metrics(row, explicit_instance=None):
    """Do not pick the first session, fabricate zeros, or accept PDH error values."""
    result, issues = {}, []
    counters = row.get("counters", [])
    if not isinstance(counters, list):
        return {}, ["counters is not an array"]
    for metric in METRICS:
        candidates = [counter for counter in counters if isinstance(counter, dict)
                      and counter.get("metric") == metric
                      and str(counter.get("instance", "")).lower() != "_total"]
        if explicit_instance is not None:
            candidates = [counter for counter in candidates
                          if isinstance(counter.get("instance"), str)
                          and counter["instance"].casefold() == str(explicit_instance).casefold()]
        if len(candidates) != 1:
            if len(candidates) > 1:
                issues.append(metric + ": ambiguous session instances")
            continue
        counter = candidates[0]
        if counter.get("status") != 0 or isinstance(counter.get("status"), bool):
            issues.append(metric + ": nonzero or unknown PDH status")
        elif not finite(counter.get("value")):
            issues.append(metric + ": unavailable/non-finite value")
        else:
            result[metric] = counter["value"]
    return result, issues


def summarize(campaign, counter_rows, parse_errors=()):
    if not isinstance(campaign, dict) or not isinstance(campaign.get("steps"), list):
        raise ValueError("campaign must declare steps")
    identities = [step.get("id") for step in campaign["steps"] if isinstance(step, dict)]
    if len(identities) != len(campaign["steps"]) or any(not isinstance(i, str) or not i for i in identities):
        raise ValueError("every step must have a nonempty identity")
    if len(set(identities)) != len(identities):
        raise ValueError("duplicate step identity")
    steps = []
    all_issues = list(parse_errors)
    metadata = [row for row in counter_rows if row.get("type") == "metadata"]
    rows = [row for row in counter_rows if row.get("type") == "sample"]
    previous_end = None
    for declared in campaign["steps"]:
        loss = declared.get("loss_percent")
        if not finite(loss) or not 0 <= loss <= 100:
            raise ValueError("step loss must be a percentage from 0 through 100")
        requested = declared.get("requested_s", 120)
        if not finite(requested) or requested <= 0:
            raise ValueError("step duration must be positive")
        step = dict(declared)
        step.update(counter_samples=0, cross_boundary_samples=0, counter_errors=[],
                    metrics={metric: numeric_stats([]) for metric in METRICS},
                    verified_goodput_mbps=None, physical_client_fps=None)
        start_value = declared.get("windows_start_utc", declared.get("start_utc"))
        end_value = declared.get("windows_end_utc", declared.get("end_utc"))
        if start_value is None or end_value is None:
            step["outcome"] = declared.get("outcome", "NOT_RUN")
            step["counter_errors"].append("no measured time interval")
            steps.append(step)
            continue
        start, end = timestamp(start_value), timestamp(end_value)
        if end <= start:
            raise ValueError("step interval must have a positive duration")
        if previous_end is not None and start < previous_end:
            raise ValueError("step time intervals overlap or are out of order")
        previous_end = end
        step["observed_duration_s"] = (end - start).total_seconds()
        alive = declared.get("client_alive_end")
        if alive is not None and not isinstance(alive, bool):
            raise ValueError("client_alive_end must be true, false or unknown")
        # Never promote merely because the script returned or a counter exists.
        step["outcome"] = declared.get("outcome", "UNKNOWN")
        if step["outcome"] not in ("COMPLETE", "SESSION_ENDED", "INCOMPLETE", "NOT_RUN", "UNKNOWN", "FAILED_SETUP"):
            raise ValueError("unknown step outcome")
        if step["outcome"] == "COMPLETE" and (alive is not True or (end-start).total_seconds() < requested):
            step["outcome"] = "INCOMPLETE"
            step["counter_errors"].append("declared complete without duration/client survival proof")
        values = {metric: [] for metric in METRICS}
        instance = declared.get("counter_instance", campaign.get("counter_instance"))
        for number, row in enumerate(rows, 1):
            try:
                sample_end = timestamp(row.get("interval_end_utc", row.get("utc")))
                start_text = row.get("interval_start_utc")
                sample_start = timestamp(start_text) if start_text else sample_end
                if sample_end < sample_start:
                    raise ValueError("counter sample interval is reversed")
                if not (start <= sample_end < end):
                    continue
                if sample_start < start:
                    step["cross_boundary_samples"] += 1
                    continue
                step["counter_samples"] += 1
                if not start_text:
                    step["counter_errors"].append("counter interval start unknown: sample %d" % number)
                    continue
                if row.get("error"):
                    step["counter_errors"].append(str(row["error"]))
                if isinstance(row.get("errors"), list):
                    step["counter_errors"].extend(str(error) for error in row["errors"])
                if isinstance(row.get("missing_metrics"), list):
                    step["counter_errors"].extend(str(metric) + ": counter unavailable" for metric in row["missing_metrics"])
                metrics, issues = sample_metrics(row, instance)
                step["counter_errors"].extend(issues)
                for metric, value in metrics.items():
                    values[metric].append(value)
            except (TypeError, ValueError) as error:
                all_issues.append({"sample": number, "error": str(error)})
        step["metrics"] = {metric: numeric_stats(value) for metric, value in values.items()}
        steps.append(step)
    return {"schema_version": 1, "method": "OpenThinRDP server-counter loss ramp",
            "campaign_provenance": {key: campaign.get(key) for key in
                                    ("provenance", "method_fidelity", "clock_alignment", "counter_instance")},
            "counter_metadata": metadata, "artifact_errors": all_issues, "steps": steps,
            "counter_phase_alignment": "RECORDED" if isinstance(campaign.get("clock_alignment"), dict)
            and campaign["clock_alignment"].get("method")
            and finite(campaign["clock_alignment"].get("uncertainty_ms"))
            and campaign["clock_alignment"]["uncertainty_ms"] >= 0 else "UNKNOWN",
            "notes": ["Server output FPS is not the rate of frames physically displayed on the client.",
                      "Server UDP bandwidth is not verified goodput; that remains null.",
                      "Missing counters, ambiguous instances and PDH errors remain unknown, never zero.",
                      "Samples overlapping the beginning of a step are excluded from that step.",
                      "Cross-host phase alignment depends on campaign.clock_alignment evidence.",
                      "Client survival is separate from active UDP tunnel evidence."]}


def write_report(prefix, report):
    prefix = Path(prefix)
    write_json(prefix.with_suffix(".json"), report)
    with prefix.with_suffix(".csv").open("w", encoding="utf-8", newline="") as stream:
        names = ["step_id", "loss_percent", "outcome", "observed_duration_s", "client_alive_end",
                 "counter_samples", "metric", "n", "mean", "median", "min", "max"]
        writer = csv.DictWriter(stream, fieldnames=names, lineterminator="\n")
        writer.writeheader()
        for step in report["steps"]:
            base = {"step_id": step["id"], **{key: step.get(key) for key in names[1:6]}}
            for metric, value in step["metrics"].items():
                writer.writerow(dict(base, metric=metric, **value))
    lines = ["# OpenThinRDP-method loss ramp", "", "Server counters; no verified goodput or physical client FPS claim.", "",
             "| Loss | Outcome | Observed seconds | Server output FPS mean | Network-skipped FPS mean |",
             "|---:|---|---:|---:|---:|"]
    for step in report["steps"]:
        values = [step["metrics"][name]["mean"] for name in
                  ("server_output_fps", "server_skipped_network_fps")]
        fmt = lambda value: "unknown" if value is None else f"{value:.2f}"
        lines.append(f"| {step['loss_percent']}% | {step['outcome']} | {fmt(step.get('observed_duration_s'))} | {fmt(values[0])} | {fmt(values[1])} |")
    lines.extend(["", "All declared steps, missing metrics and errors are retained in JSON/CSV.", ""])
    prefix.with_suffix(".md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    planner = commands.add_parser("plan")
    planner.add_argument("--source-dir", type=Path, required=True)
    planner.add_argument("--loss", type=float, nargs="+", default=[25, 50, 70])
    planner.add_argument("--seconds", type=float, default=120)
    planner.add_argument("--output", type=Path, required=True)
    reporter = commands.add_parser("report")
    reporter.add_argument("--campaign", type=Path, required=True)
    reporter.add_argument("--counters", type=Path, required=True)
    reporter.add_argument("--output", type=Path, required=True, help="prefix for JSON/CSV/Markdown")
    args = parser.parse_args()
    if args.command == "plan":
        write_json(args.output, plan(args.source_dir, args.loss, args.seconds))
    else:
        rows, errors = read_counter_rows(args.counters)
        campaign = json.loads(args.campaign.read_text(encoding="utf-8-sig"))
        write_report(args.output, summarize(campaign, rows, errors))


if __name__ == "__main__":
    main()
