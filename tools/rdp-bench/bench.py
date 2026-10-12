#!/usr/bin/env python3
"""Production UDP2 transport benchmarks. See README.md and SPECIFICATION.md."""
import argparse
import json
import shutil
import sys
from pathlib import Path
from rdpbench.common import audit, environment, read_json

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="op", required=True)
    run = commands.add_parser("type1", help="Run a transport campaign")
    run.add_argument("--suite", default=str(ROOT / "configs/type1.json"))
    run.add_argument("--profiles", default=str(ROOT / "configs/profiles.json"))
    run.add_argument("--variants", help="JSON file describing candidate libraries/options")
    run.add_argument("--case", help="Run one case ID")
    run.add_argument("--output", required=True)
    run.add_argument("--library", help="Candidate FreeRDP shared library")
    run.add_argument("--peer-library", help="Fixed reference library; defaults to first variant")
    run.add_argument("--peer-env", help="JSON object of environment settings for the fixed peer")
    run.add_argument("--commit", help="Commit of the candidate binary")
    run.add_argument("--driver", choices=["native", "udp-calibration"], default="native")
    engine = run.add_mutually_exclusive_group()
    engine.add_argument("--direct", action="store_true", help="Unshaped local sockets; loopback profile only")
    engine.add_argument("--netns", help="JSON state created by linux/ns_lab.py on Linux")
    run.add_argument("--offered-mbps", type=float, default=8, help="Raw UDP calibration load only")
    run.add_argument("--calibration", help="Evidence-backed lab acceptance receipt")
    live = commands.add_parser("type2", help="Run a live Windows RDP campaign with numbered input probes")
    live.add_argument("--suite", default=str(ROOT / "configs/type2.smoke.json"))
    live.add_argument("--profiles", default=str(ROOT / "configs/profiles.json"))
    live.add_argument("--lab", required=True, help="Provisioned Windows/observer/network adapter JSON")
    live.add_argument("--variants", required=True, help="Frozen SDL client binaries and build metadata")
    live.add_argument("--case", help="Run one live case ID")
    live.add_argument("--output", required=True, help="New private campaign directory")
    replay = commands.add_parser("replay", help="Independent production receiver fixtures")
    replay.add_argument("--library", required=True)
    replay.add_argument("--output", required=True)
    replay.add_argument("--wrap", action="store_true")
    report = commands.add_parser("report", help="Export JSON/CSV/SQLite and offline HTML")
    report.add_argument("--input", required=True)
    report.add_argument("--output", required=True)
    comparison = commands.add_parser("compare", help="Paired comparison of calibrated campaigns")
    for name in ("input", "baseline", "candidate", "metric", "output"):
        comparison.add_argument("--" + name, required=True)
    check = commands.add_parser("audit", help="Check one trial's artifact hashes")
    check.add_argument("folder")
    plan = commands.add_parser("plan", help="List planned case/seed/variant combinations")
    plan.add_argument("--suite", required=True)
    plan.add_argument("--variants", required=True)
    commands.add_parser("doctor", help="Report local environment/tools")
    args = parser.parse_args()
    if args.op == "type1":
        if args.driver == "native" and not (args.variants or args.library):
            parser.error("native mode requires --library or --variants")
        from rdpbench.type1 import run_suite
        return run_suite(args)
    if args.op == "type2":
        from rdpbench.live import run_suite
        return run_suite(args)
    if args.op == "replay":
        from rdpbench.replay import run
        return run(str(Path(args.library).expanduser().resolve()), args.output, args.wrap)
    if args.op == "report":
        from rdpbench.report import dashboard
        print("trials:", dashboard(args.input, args.output))
    elif args.op == "compare":
        from rdpbench.report import compare
        print(json.dumps(compare(args.input, args.baseline, args.candidate, args.metric, args.output), indent=2))
    elif args.op == "audit":
        errors = audit(args.folder)
        print(json.dumps({"errors": errors}, indent=2))
        return bool(errors)
    elif args.op == "plan":
        config = read_json(args.suite)
        variants = read_json(args.variants)
        print(json.dumps([
            {"case": case["id"], "variant": variant["id"], "seed": seed}
            for case in config["cases"] for seed in config["seeds"]
            for variant in variants["variants"]
        ], indent=2))
    elif args.op == "doctor":
        info = environment()
        info["tools"] = {name: shutil.which(name) for name in ("python3", "cmake", "ip", "tc", "iperf3")}
        print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        print("Interrupted; completed trial artifacts are retained.", file=sys.stderr)
        sys.exit(130)
