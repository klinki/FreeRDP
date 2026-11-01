#!/usr/bin/env python3
"""Record the actual hosted toolchain without making reproducibility claims."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import subprocess


def command(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        return {"unavailable": str(error)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = dict(source_commit=command("git", "rev-parse", "HEAD"), recorded_at=datetime.now(timezone.utc).isoformat(), platform=platform.platform(),
                  architecture=platform.machine(), macos=command("sw_vers"),
                  xcode=command("xcodebuild", "-version"), sdk=command("xcrun", "--sdk", "macosx", "--show-sdk-version"),
                  compiler=command("clang", "--version"), cmake=command("cmake", "--version"),
                  homebrew=command("brew", "list", "--versions"),
                  environment={key: os.environ.get(key) for key in
                               ("ImageOS", "ImageVersion", "RUNNER_OS", "RUNNER_ARCH", "GITHUB_SHA", "GITHUB_REF", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT")})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
