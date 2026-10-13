#!/usr/bin/env python3
"""Rerun 48/64 KiB queues and controlled ACK-only feedback-delay diagnostics."""
from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import sys
from pathlib import Path

from rdpbench.common import read_json, sha256, write_json

ROOT = Path(__file__).resolve().parent
CAPS = (48, 64)
PROFILES = (("clean", "ten_mbit_clean"), ("impaired", "ten_mbit"))
SEEDS = (11, 29, 47, 71, 101)
DELAYS_MS = (0, 5, 10, 20)
PEER_ENV = {"RDP_BENCH_SEND_QUEUE_BYTES": "131072", "RDP_BENCH_QUEUE_OBSERVER": "1",
            "RDP_BENCH_COUNTERS": "1", "RDP_BENCH_OBSERVER": "1"}


def configuration(workloads, seeds, warmup, measure, drain, delays=None):
    cases = []
    for label, profile in PROFILES:
        for workload in workloads:
            for delay in delays if delays is not None else (None,):
                case = {"id": f"{label}-{workload.replace('_', '-')}" +
                              (f"-ack{delay}" if delay is not None else ""),
                        "profile": profile, "workload": workload}
                if delay is not None:
                    case["ack_feedback"] = {"c2s_delay_ms": 0, "s2c_delay_ms": delay}
                cases.append(case)
    return {"version": 1, "warmup_s": warmup, "measure_s": measure, "drain_s": drain,
            "payload_bytes": 16384, "probe_hz": 20, "seeds": list(seeds), "cases": cases}


def prepare(output, library, metadata):
    output.mkdir(parents=True, exist_ok=True)
    configs = {
        "smoke": configuration(("upload_mixed",), SEEDS[:1], 1, 3, 3, (0, 20)),
        "baseline": configuration(("upload", "upload_mixed"), SEEDS, 5, 30, 10),
        "ack-timing": configuration(("upload_mixed",), SEEDS[:3], 3, 15, 10, DELAYS_MS),
        "control": configuration(("probe",), SEEDS[:3], 3, 10, 3),
    }
    provenance = {
        "version": 1, "library": str(library), "binary_sha256": sha256(library),
        "build_metadata": metadata, "caps_kib": list(CAPS), "fixed_peer_environment": PEER_ENV,
        "source_hashes": {str(p.relative_to(ROOT)): sha256(p)
                          for p in sorted((ROOT / "rdpbench").glob("*.py"))},
        "driver_sha256": sha256(Path(__file__)),
        "profile_source_sha256": sha256(ROOT / "configs/profiles.json"),
        "configurations": configs,
        "notes": [
            "Baseline omits ACK classification and retains the existing relay path.",
            "ACK diagnostics classify in every condition, including the zero-ms control.",
            "Only S→C ACK/ACKVEC-only delivery deadlines receive the selected additional delay.",
            "DATA/piggybacked ACK bytes, network serialization, loss, queue budget and profiles are unmodified.",
            "This is feedback-delay sensitivity, not Windows ACK batching or a receiver implementation.",
            "The fixed peer advertises its genuine default LogWindow 5; both effective windows stay at 31 packets.",
            "LogWindow 15 tracking is covered by production-path regression fixtures, not advertised by this small lab peer.",
            "TLS, RDP setup, Windows peer, rendering and desktop latency remain outside raw transport scope; calibration and all failures remain visible.",
            "The zero-ms diagnostic is shorter than baseline, so cross-phase observer differences are descriptive only.",
        ]}
    saved = output / "ack-sweep-plan.json"
    if saved.exists() and read_json(saved) != provenance:
        raise ValueError("Existing ACK sweep has changed binary/source/provenance; use a new output directory")
    write_json(saved, provenance)
    write_json(output / "peer-env.json", PEER_ENV)
    variants = [{"id": f"q{cap}", "library": str(library), "build_metadata": metadata,
                 "commit": metadata.get("commit"), "build_options": metadata.get("build_options"),
                 "env": dict(PEER_ENV, RDP_BENCH_SEND_QUEUE_BYTES=str(cap * 1024))} for cap in CAPS]
    write_json(output / "variants.json", {"variants": variants})
    for phase, config in configs.items():
        write_json(output / f"{phase}-suite.json", config)
    return configs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True)
    parser.add_argument("--build-metadata", help="Matching frozen build.py metadata JSON")
    parser.add_argument("--output", required=True)
    parser.add_argument("--phase", choices=("all", "smoke", "baseline", "ack-timing", "control"), default="all")
    parser.add_argument("--calibration", help="Optional matching operator acceptance receipt")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    library = Path(args.library).expanduser().resolve()
    if not library.is_file():
        parser.error("native benchmark library is missing")
    native = ctypes.CDLL(str(library))
    if any(not hasattr(native, "freerdp_bench_v1_" + name)
           for name in ("queue_measure", "clock_ns", "queue_stats", "queue_hist")):
        parser.error("native library lacks queue observer ABI; rebuild")
    metadata = read_json(args.build_metadata) if args.build_metadata else {}
    if metadata and metadata.get("binary_sha256") != sha256(library):
        parser.error("build metadata does not match the frozen library")
    output = Path(args.output).expanduser().resolve()
    configs = prepare(output, library, metadata)
    if args.prepare_only:
        print(json.dumps({phase: len(config["cases"]) * len(config["seeds"]) * len(CAPS)
                          for phase, config in configs.items()}, sort_keys=True))
        return 0
    phases = ("smoke", "baseline", "ack-timing") if args.phase == "all" else (args.phase,)
    saved_status = output / "ack-sweep-status.json"
    status = read_json(saved_status) if saved_status.exists() else {"version": 1, "phases": {}}
    failures = []
    for phase in phases:
        target = output / phase
        if target.exists():
            parser.error(f"{target} already exists; select a pending phase or use another output directory")
        config = configs[phase]
        command = [sys.executable, "-B", str(ROOT / "bench.py"), "type1",
                   "--suite", str(output / f"{phase}-suite.json"),
                   "--variants", str(output / "variants.json"),
                   "--profiles", str(ROOT / "configs/profiles.json"),
                   "--peer-library", str(library), "--peer-env", str(output / "peer-env.json"),
                   "--output", str(target)]
        if args.calibration:
            command += ["--calibration", str(Path(args.calibration).expanduser().resolve())]
        expected = len(config["cases"]) * len(config["seeds"]) * len(CAPS)
        print(f"PHASE {phase}: {expected} trials", flush=True)
        result = subprocess.run(command, cwd=ROOT.parent.parent)
        campaign_file = target / "campaign.json"
        complete = campaign_file.is_file()
        rows = read_json(campaign_file).get("trials", []) if complete else []
        status["phases"][phase] = {"complete": complete and len(rows) == expected,
                                  "expected_trials": expected, "observed_trials": len(rows),
                                  "returncode": result.returncode,
                                  "failed_trials": [r["trial_id"] for r in rows
                                                    if r.get("sut_outcome") != "COMPLETE" or r.get("validity") == "INVALID_HARNESS"]}
        status["pass"] = all(v["complete"] and v["returncode"] == 0 for v in status["phases"].values())
        write_json(saved_status, status)
        if result.returncode:
            failures.append(phase)
        if not status["phases"][phase]["complete"]:
            return result.returncode or 1
        if phase == "smoke" and any(r.get("sut_outcome") != "COMPLETE" or
                                    any("queue telemetry" in reason for reason in r.get("reasons", [])) for r in rows):
            return 1
        report = [sys.executable, "-B", str(ROOT / "queue_report.py"), "--root", str(output),
                  "--campaign", phase, "--baseline", "q48", "--candidates", "q64",
                  "--output-prefix", f"{phase}-analysis"]
        if subprocess.run(report, cwd=ROOT.parent.parent).returncode:
            return 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
