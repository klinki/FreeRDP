#!/usr/bin/env python3
"""Repeat the bounded send-queue sweep with a fixed capable peer and packet window."""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from rdpbench.common import read_json, sha256, write_json

ROOT = Path(__file__).resolve().parent
CAPS = (32, 48, 64, 128)
PROFILES = (("clean", "ten_mbit_clean"), ("impaired", "ten_mbit"))
SEEDS = (11, 29, 47, 71, 101)
PEER_ENV = {"RDP_BENCH_SEND_QUEUE_BYTES": "131072", "RDP_BENCH_QUEUE_OBSERVER": "1",
            "RDP_BENCH_COUNTERS": "1", "RDP_BENCH_OBSERVER": "1"}


def configuration(workloads, seeds, warmup, measure, drain):
    return {"version": 1, "warmup_s": warmup, "measure_s": measure, "drain_s": drain,
            "payload_bytes": 16384, "probe_hz": 20, "seeds": list(seeds),
            "cases": [{"id": f"{label}-{workload.replace('_', '-')}",
                       "profile": profile, "workload": workload}
                      for label, profile in PROFILES for workload in workloads]}


def prepare(output, library, metadata):
    output.mkdir(parents=True, exist_ok=True)
    provenance = {"version": 1, "library": str(library), "binary_sha256": sha256(library),
                  "caps_kib": list(CAPS), "fixed_peer_environment": PEER_ENV,
                  "source_hashes": {str(p.relative_to(ROOT)): sha256(p)
                                    for p in sorted((ROOT / "rdpbench").glob("*.py"))},
                  "notes": ["Queue capacity is the treatment; packet window remains 31.",
                            "Residence is transport-byte-weighted acceptance to first emission.",
                            "Unloaded controls and observer study are shorter diagnostics.",
                            "Calibration can remain provisional; all failures are retained."]}
    saved = output / "sweep-plan.json"
    if saved.exists() and read_json(saved) != provenance:
        raise ValueError("Existing sweep has different binary/source/provenance; use a new output directory")
    write_json(saved, provenance)
    write_json(output / "peer-env.json", PEER_ENV)
    variants = []
    for cap in CAPS:
        variants.append({"id": f"q{cap}", "library": str(library), "build_metadata": metadata,
                         "commit": metadata.get("commit"), "build_options": metadata.get("build_options"),
                         "env": dict(PEER_ENV, RDP_BENCH_SEND_QUEUE_BYTES=str(cap * 1024))})
    write_json(output / "variants.json", {"variants": variants})
    observer_variants = []
    for cap in (32, 128):
        for enabled in ("1", "0"):
            observer_variants.append({"id": f"q{cap}-{'on' if enabled == '1' else 'off'}",
                                      "library": str(library), "build_metadata": metadata,
                                      "commit": metadata.get("commit"),
                                      "build_options": metadata.get("build_options"),
                                      "env": dict(PEER_ENV,
                                                  RDP_BENCH_SEND_QUEUE_BYTES=str(cap * 1024),
                                                  RDP_BENCH_QUEUE_OBSERVER=enabled)})
    write_json(output / "observer-variants.json", {"variants": observer_variants})
    configs = {
        "smoke": configuration(("upload_mixed",), (11,), 1, 3, 3),
        "campaign": configuration(("upload", "upload_mixed"), SEEDS, 5, 30, 10),
        "controls": configuration(("probe",), SEEDS[:3], 3, 10, 3),
        "observer-study": configuration(("upload_mixed",), SEEDS[:3], 2, 10, 8),
    }
    for phase, config in configs.items():
        write_json(output / f"{phase}-suite.json", config)
    return configs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, help="Frozen native benchmark library with queue observer ABI")
    parser.add_argument("--build-metadata", help="Matching build.py metadata JSON")
    parser.add_argument("--output", required=True)
    parser.add_argument("--phase", choices=("all", "smoke", "campaign", "controls", "observer-study"), default="all")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    library = Path(args.library).expanduser().resolve()
    if not library.is_file():
        parser.error("native benchmark library is missing")
    import ctypes
    native = ctypes.CDLL(str(library))
    if any(not hasattr(native, "freerdp_bench_v1_" + name)
           for name in ("queue_measure", "clock_ns", "queue_stats", "queue_hist")):
        parser.error("native library lacks the queue observer ABI; rebuild the benchmark library")
    metadata = read_json(args.build_metadata) if args.build_metadata else {}
    if metadata and metadata.get("binary_sha256") != sha256(library):
        parser.error("build metadata does not match the frozen library")
    output = Path(args.output).expanduser().resolve()
    configs = prepare(output, library, metadata)
    if args.prepare_only:
        print(json.dumps({phase: len(config["cases"]) * len(config["seeds"]) * 4
                          for phase, config in configs.items()}, sort_keys=True))
        return 0
    phases = ("smoke", "observer-study", "controls", "campaign") if args.phase == "all" else (args.phase,)
    failures = []
    for phase in phases:
        target = output / phase
        if target.exists():
            parser.error(f"{target} already exists; use another output directory or select a pending phase")
        config = configs[phase]
        variants = "observer-variants.json" if phase == "observer-study" else "variants.json"
        command = [sys.executable, "-B", str(ROOT / "bench.py"), "type1",
                   "--suite", str(output / f"{phase}-suite.json"),
                   "--variants", str(output / variants),
                   "--profiles", str(ROOT / "configs/profiles.json"),
                   "--peer-library", str(library), "--peer-env", str(output / "peer-env.json"),
                   "--output", str(target)]
        print(f"PHASE {phase}: {len(config['cases']) * len(config['seeds']) * 4} trials", flush=True)
        result = subprocess.run(command, cwd=ROOT.parent.parent)
        if result.returncode:
            failures.append(phase)
        campaign_file = target / "campaign.json"
        if not campaign_file.is_file():
            write_json(output / "sweep-status.json", {"completed": False, "failed_phases": failures,
                                                      "blocked_phase": phase, "reason": "campaign incomplete"})
            return result.returncode or 1
        rows = read_json(campaign_file)["trials"]
        # Integrity/telemetry failures in the smoke require repair before a long run.
        if phase == "smoke" and any(r.get("sut_outcome") != "COMPLETE" or
                                    any("queue telemetry" in reason for reason in r.get("reasons", []))
                                    for r in rows):
            write_json(output / "sweep-status.json", {"completed": False, "failed_phases": failures,
                                                      "blocked_phase": phase, "reason": "smoke correctness/telemetry failure"})
            return 1
    write_json(output / "sweep-status.json", {"completed": True, "phases_this_invocation": list(phases),
                                              "failed_phases": failures, "pass": not failures})
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
