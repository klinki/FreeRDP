#!/usr/bin/env python3
"""Summarize FreeRDP SDL render-metrics JSONL records.

The recorder writes one record per window and approximately one-second
interval. This tool combines records by window/monitor, drops an optional
number of warmup intervals for each group, and reports upload throughput and
timing distributions. It never turns redraw counts into a frame-rate claim.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, Iterable


DEFINITIONS = {
    "timing": "All *_wall_ns values are monotonic elapsed wall-clock durations, not CPU time.",
    "attempted_dirty_pixels": "Sum of the pixel areas supplied for redraw attempts; rectangle multiplicity is retained.",
    "uploaded_pixels": "Sum of pixel areas submitted by upload calls; rectangle multiplicity is retained.",
    "uploaded_bytes": "Sum of bytes submitted by upload calls, as reported by the renderer.",
    "percentiles": "p50/p95/p99 use nearest-rank over all retained samples after warmup; each interval keeps at most 256 reservoir samples, so these are approximate for long runs.",
    "warmup": "--warmup-intervals drops the first N records for each window/monitor group before aggregation.",
    "render_v2": "Render schema version 2 adds present_skips, target_recreates, gdi_recreates, topbar_draws and stalled_presents. Version 1 keys are unchanged; stalled overlay presents are excluded from present_calls.",
    "queue": "freerdp.sdl_queue_metrics records are process-global (window 0, monitor 0), written about once per second when the event queue is active. Offline replay drives rendering without the event queue, so replay files normally contain no queue records. queue_wait_ns is push-to-pop delay; average wait is queue_wait_ns / pops.",
}


def _nonnegative_int(value: Any, default: int = 0) -> int:
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return max(value, 0)
    if isinstance(value, float) and value.is_integer():
        return max(int(value), 0)
    return default


def _percentile(values: list[int], percentage: float) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil((percentage / 100.0) * len(ordered)))
    return ordered[rank - 1]


def _samples(record: dict[str, Any], key: str) -> list[int]:
    values = record.get(key, [])
    if not isinstance(values, list):
        return []
    return [_nonnegative_int(value) for value in values if isinstance(value, (int, float))]


def _sum(records: Iterable[dict[str, Any]], key: str) -> int:
    return sum(_nonnegative_int(record.get(key)) for record in records)


def _read_records(path: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]], int]:
    records: list[dict[str, Any]] = []
    queue_records: list[dict[str, Any]] = []
    errors = 0
    source = sys.stdin if path == "-" else Path(path).open(encoding="utf-8")
    try:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                print(f"{path}:{line_number}: invalid JSON ({error.msg})", file=sys.stderr)
                errors += 1
                continue
            if not isinstance(value, dict):
                print(f"{path}:{line_number}: skipped non-object record", file=sys.stderr)
                errors += 1
                continue
            schema = value.get("schema")
            if schema == "freerdp.sdl_queue_metrics":
                if value.get("version") != 1:
                    print(
                        f"{path}:{line_number}: skipped unsupported queue metrics version "
                        f"{value.get('version')!r}",
                        file=sys.stderr,
                    )
                    errors += 1
                    continue
                queue_records.append(value)
                continue
            if schema != "freerdp.sdl_render_metrics":
                print(f"{path}:{line_number}: skipped record with unknown schema", file=sys.stderr)
                errors += 1
                continue
            if value.get("version") not in (1, 2):
                print(
                    f"{path}:{line_number}: skipped unsupported metrics version "
                    f"{value.get('version')!r}",
                    file=sys.stderr,
                )
                errors += 1
                continue
            records.append(value)
    finally:
        if path != "-":
            source.close()
    return records, queue_records, errors


def _group_records(records: list[dict[str, Any]]) -> dict[tuple[int, int], list[dict[str, Any]]]:
    groups: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for record in records:
        window = _nonnegative_int(record.get("window_id"))
        monitor = _nonnegative_int(record.get("monitor_id"))
        groups.setdefault((window, monitor), []).append(record)
    for group in groups.values():
        group.sort(key=lambda record: _nonnegative_int(record.get("interval_start_ns")))
    return groups


def _timing_stats(samples: list[int]) -> dict[str, float | int | None]:
    return {
        "samples": len(samples),
        "p50_wall_ms": _as_ms(_percentile(samples, 50)),
        "p95_wall_ms": _as_ms(_percentile(samples, 95)),
        "p99_wall_ms": _as_ms(_percentile(samples, 99)),
    }


def _as_ms(value: int | None) -> float | None:
    return None if value is None else value / 1_000_000.0


def _average_wall_ms(records: list[dict[str, Any]], duration_key: str, count_key: str) -> float | None:
    count = _sum(records, count_key)
    return None if count == 0 else _as_ms(_sum(records, duration_key)) / count


def _summarize(records: list[dict[str, Any]], warmup_intervals: int) -> dict[str, Any]:
    records = records[warmup_intervals:]
    duration_ns = _sum(records, "interval_duration_ns")
    uploaded_bytes = _sum(records, "uploaded_bytes")
    uploaded_pixels = _sum(records, "uploaded_pixels")
    attempted_pixels = _sum(records, "attempted_dirty_pixels")
    redraw_samples = [sample for record in records for sample in _samples(record, "redraw_wall_ns_samples")]
    frame_samples = [sample for record in records for sample in _samples(record, "frame_interval_wall_ns_samples")]

    return {
        "intervals": len(records),
        "duration_s": duration_ns / 1_000_000_000.0,
        "frames": _sum(records, "frames"),
        "attempted_dirty_pixels": attempted_pixels,
        "uploaded_pixels": uploaded_pixels,
        "uploaded_bytes": uploaded_bytes,
        "uploaded_bytes_per_s": (uploaded_bytes * 1_000_000_000.0 / duration_ns)
        if duration_ns
        else None,
        "upload_calls": _sum(records, "upload_calls"),
        "average_upload_wall_ms": _average_wall_ms(records, "upload_wall_ns", "upload_calls"),
        "draw_calls": _sum(records, "draw_calls"),
        "average_draw_wall_ms": _average_wall_ms(records, "draw_wall_ns", "draw_calls"),
        "present_calls": _sum(records, "present_calls"),
        "average_present_wall_ms": _average_wall_ms(records, "present_wall_ns", "present_calls"),
        "present_skips": _sum(records, "present_skips"),
        "target_recreates": _sum(records, "target_recreates"),
        "gdi_recreates": _sum(records, "gdi_recreates"),
        "topbar_draws": _sum(records, "topbar_draws"),
        "stalled_presents": _sum(records, "stalled_presents"),
        "redraw_wall": _timing_stats(redraw_samples),
        "frame_interval_wall": _timing_stats(frame_samples),
    }


def _summarize_queue(records: list[dict[str, Any]]) -> dict[str, Any]:
    records = sorted(records, key=lambda record: _nonnegative_int(record.get("interval_start_ns")))
    duration_ns = _sum(records, "interval_duration_ns")
    pops = _sum(records, "pops")
    wait_ns = _sum(records, "queue_wait_ns")
    return {
        "intervals": len(records),
        "duration_s": duration_ns / 1_000_000_000.0,
        "pushes": _sum(records, "pushes"),
        "attempted_rects": _sum(records, "attempted_rects"),
        "merged_rects": _sum(records, "merged_rects"),
        "collapsed_events": _sum(records, "collapsed_events"),
        "pops": pops,
        "empty_pops": _sum(records, "empty_pops"),
        "pop_rects": _sum(records, "pop_rects"),
        "queue_wait_ns": wait_ns,
        "average_queue_wait_ms": (wait_ns / 1_000_000.0 / pops) if pops else None,
        "update_events_received": _sum(records, "update_events_received"),
        "update_events_acted": _sum(records, "update_events_acted"),
        "motions_coalesced": _sum(records, "motions_coalesced"),
    }


def _summarize_file(path: str, warmup_intervals: int) -> dict[str, Any]:
    records, queue_records, errors = _read_records(path)
    groups = _group_records(records)
    summaries = []
    for (window, monitor), group in sorted(groups.items()):
        summary = _summarize(group, warmup_intervals)
        summary["window_id"] = window
        summary["monitor_id"] = monitor
        summaries.append(summary)
    label = "stdin" if path == "-" else Path(path).stem
    return {
        "label": label,
        "records": len(records),
        "parse_errors": errors,
        "groups": summaries,
        "queue": _summarize_queue(queue_records),
    }


def _format_number(value: Any) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def _print_text(result: dict[str, Any]) -> None:
    print(f"{result['label']}: {result['records']} intervals")
    if result["parse_errors"]:
        print(f"  skipped records: {result['parse_errors']}")
    for group in result["groups"]:
        print(
            f"  window={group['window_id']} monitor={group['monitor_id']} "
            f"intervals={group['intervals']} duration_s={_format_number(group['duration_s'])}"
        )
        print(
            f"    attempted_pixels={group['attempted_dirty_pixels']} "
            f"uploaded_pixels={group['uploaded_pixels']} uploaded_bytes={group['uploaded_bytes']} "
            f"uploaded_bytes_per_s={_format_number(group['uploaded_bytes_per_s'])}"
        )
        print(
            f"    upload_calls={group['upload_calls']} average_upload_wall_ms="
            f"{_format_number(group['average_upload_wall_ms'])} "
            f"draw_calls={group['draw_calls']} average_draw_wall_ms="
            f"{_format_number(group['average_draw_wall_ms'])} "
            f"present_calls={group['present_calls']} average_present_wall_ms="
            f"{_format_number(group['average_present_wall_ms'])}"
        )
        for name in ("redraw_wall", "frame_interval_wall"):
            timing = group[name]
            print(
                f"    {name}_ms p50={_format_number(timing['p50_wall_ms'])} "
                f"p95={_format_number(timing['p95_wall_ms'])} "
                f"p99={_format_number(timing['p99_wall_ms'])} samples={timing['samples']}"
            )
        print(
            f"    present_skips={group['present_skips']} "
            f"target_recreates={group['target_recreates']} "
            f"gdi_recreates={group['gdi_recreates']} "
            f"topbar_draws={group['topbar_draws']} "
            f"stalled_presents={group['stalled_presents']}"
        )
    queue = result.get("queue", {})
    if queue.get("intervals"):
        print(
            f"  queue intervals={queue['intervals']} duration_s={_format_number(queue['duration_s'])} "
            f"pushes={queue['pushes']} attempted_rects={queue['attempted_rects']} "
            f"merged_rects={queue['merged_rects']} collapsed_events={queue['collapsed_events']}"
        )
        print(
            f"    pops={queue['pops']} empty_pops={queue['empty_pops']} "
            f"pop_rects={queue['pop_rects']} "
            f"average_queue_wait_ms={_format_number(queue['average_queue_wait_ms'])} "
            f"update_received={queue['update_events_received']} "
            f"update_acted={queue['update_events_acted']} "
            f"motions_coalesced={queue['motions_coalesced']}"
        )
    else:
        print("  queue: no queue records (expected for offline replay)")
    if not result["groups"]:
        print("  no valid metric groups")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare FreeRDP SDL render-metrics JSONL files by window and monitor.",
        epilog=(
            "Definitions: "
            + " ".join(f"{key}={value}" for key, value in DEFINITIONS.items())
            + " Redraw counts are reported as counts only and are never converted to a frame rate."
        ),
    )
    parser.add_argument("jsonl", nargs="+", help="JSONL file(s), or - for stdin")
    parser.add_argument(
        "--warmup-intervals",
        type=int,
        default=0,
        metavar="N",
        help="exclude the first N intervals per window/monitor group (default: 0)",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args()
    if args.warmup_intervals < 0:
        parser.error("--warmup-intervals must be non-negative")

    results = [_summarize_file(path, args.warmup_intervals) for path in args.jsonl]
    if args.format == "json":
        print(json.dumps({"definitions": DEFINITIONS, "datasets": results}, indent=2, sort_keys=True))
    else:
        print("Timing is monotonic wall duration, not CPU time; percentile samples are approximate when bounded.")
        for result in results:
            _print_text(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
