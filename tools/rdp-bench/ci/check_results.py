#!/usr/bin/env python3
"""Gate correctness and artifact/timing validity; optionally require calibrated evidence."""
import argparse
import json
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rdpbench.report import collect

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("folder")
parser.add_argument("--require-calibration", action="store_true")
args = parser.parse_args()
trials = collect(args.folder)
errors = [] if trials else ["no trials"]
for trial in trials:
    if trial.get("sut_outcome") != "COMPLETE":
        errors.append(trial["trial_id"] + ": " + trial.get("sut_outcome", "missing outcome"))
    allowed = ("VALIDATED_LAB",) if args.require_calibration else ("VALIDATED_LAB", "PROVISIONAL_UNCALIBRATED")
    if trial.get("validity") not in allowed:
        errors.append(trial["trial_id"] + ": " + trial.get("validity", "missing validity"))
print(json.dumps({"gate": "correctness/artifact/timing validity", "errors": errors}, indent=2))
sys.exit(1 if errors else 0)
