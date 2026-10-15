#!/usr/bin/env python3
"""Build the pinned Microsoft RDP client suite; do not install system packages."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from profiles import ROOT, PIN, suite_patches


def command(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def prepare(source, output):
    source, output = source.resolve(), output.resolve()
    if output.exists():
        raise ValueError(f"Output already exists; choose a new directory: {output}")
    if not source.exists():
        command("git", "clone", "--filter=blob:none", "--no-checkout", PIN["repository"], str(source))
        command("git", "sparse-checkout", "set", "AssemblyInfo", "ProtoSDK", "TestSuites/RDP/Client", cwd=source)
        command("git", "checkout", "--detach", PIN["revision"], cwd=source)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if revision != PIN["revision"]:
        raise ValueError(f"Expected Microsoft revision {PIN['revision']}, found {revision}")
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=source):
        raise ValueError("Microsoft source has tracked changes; use a clean pinned checkout")
    patches = suite_patches()
    output.mkdir(parents=True)
    # Select a supported stable SDK without changing the user's global SDK selection.
    (output / "global.json").write_text(json.dumps({"sdk": {
        "version": "8.0.100", "rollForward": "latestFeature", "allowPrerelease": False,
    }}, indent=2) + "\n")
    # Export pinned Git objects instead of modifying the user's checkout or
    # building untracked files. Keep source/obj/drop out of the portable suite.
    with tempfile.TemporaryDirectory(prefix="wpts-build-", dir=output.parent) as directory:
        temporary = Path(directory)
        archive = temporary / "source.tar"
        build_source = temporary / "source"
        build_source.mkdir()
        command("git", "archive", "--format=tar", "-o", str(archive), PIN["revision"],
                "AssemblyInfo", "ProtoSDK", "TestSuites/RDP/Client", "LICENSE.txt", cwd=source)
        command("tar", "-xf", str(archive), "-C", str(build_source))
        # Prevent git apply from discovering a containing FreeRDP repository.
        command("git", "init", "--quiet", cwd=build_source)
        # dotnet resolves SDK selection from its working directory (output).
        for patch in patches:
            path = str(ROOT / patch["path"])
            command("git", "apply", "--check", path, cwd=build_source)
            command("git", "apply", path, cwd=build_source)
        suite = build_source / "TestSuites/RDP/Client"
        project = suite / "src/TestSuite/RDP_ClientTestSuite.csproj"
        with (output / "build.log").open("w") as log:
            result = subprocess.run([
                "dotnet", "publish", str(project), "-c", "Release", "-o", str(output / "Bin"),
                f"-p:TestSuiteRoot={build_source}", "--disable-build-servers", "-m:1",
            ], cwd=output, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"Microsoft suite build failed; see {output / 'build.log'}")
        data = output / "TestData"
        data.mkdir()
        for relative in ("src/TestSuite/TestData", "src/TestSuite/RDPEGFX/H264TestData",
                         "src/TestSuite/RDPEGFX/H264TestData/BaseImage",
                         "src/TestSuite/RDPEDISP/RdpedispEnhancedAdapterImages"):
            for path in (suite / relative).iterdir():
                if path.is_file():
                    shutil.copy2(path, data / path.name)
        shutil.copytree(suite / "Setup/Data", output / "Data")
        shutil.copy2(build_source / "LICENSE.txt", output / "MICROSOFT-LICENSE.txt")
    (output / "provenance.json").write_text(json.dumps({
        "repository": PIN["repository"], "revision": revision, "patches": patches,
        "sdk": subprocess.check_output(["dotnet", "--version"], cwd=output, text=True).strip(),
    }, indent=2) + "\n")
    print(f"Built Microsoft RDP test suite: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / ".cache/source")
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/suite")
    args = parser.parse_args()
    try:
        prepare(args.source, args.output)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
