#!/usr/bin/env python3
"""Reproduce slow-path negative-test diagnostics in an isolated working directory.

No shared FreeRDP or Microsoft source/binaries are changed. Generated certificates
and binaries remain under --work; never copy them into evidence folders.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
import run
from profiles import CaseOptions, PIN, suite_patches

CASES = {
    "mcs": "S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidMCSLength",
    "tpkt": "S4_SlowPathInputTest_NegativeTest_ServerToClientSlowPath_InvalidTPKTLength",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="Clean pinned Microsoft Git checkout")
    parser.add_argument("--work", required=True, type=Path, help="New scratch directory")
    parser.add_argument("--client", type=Path, default=ROOT.parents[1] / "build/protocol-tests/client/SDL/SDL3/sdl-freerdp")
    parser.add_argument("--private-core", type=Path, help="Optional macOS directory containing diagnostic libfreerdp3.3.dylib from demand-active reproduction")
    args = parser.parse_args()
    source, work, binary = args.source.resolve(), args.work.resolve(), args.client.resolve()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if revision != PIN["revision"]:
        raise ValueError(f"Expected pinned source {PIN['revision']}, got {revision}")
    work.mkdir(mode=0o700)
    os.umask(0o077)
    build_source = work / "source"
    build_source.mkdir()
    archive = work / "source.tar"
    subprocess.run(["git", "archive", "-o", str(archive), revision, "AssemblyInfo", "ProtoSDK", "TestSuites/RDP/Client", "LICENSE.txt"], cwd=source, check=True)
    subprocess.run(["tar", "-xf", str(archive), "-C", str(build_source)], check=True)
    subprocess.run(["git", "init", "--quiet"], cwd=build_source, check=True)
    for patch in suite_patches():
        subprocess.run(["git", "apply", str(ROOT / patch["path"])], cwd=build_source, check=True)
    subprocess.run(["git", "apply", str(Path(__file__).with_name("instrumentation.patch"))], cwd=build_source, check=True)
    suite = work / "suite"
    shutil.copytree(ROOT / ".cache/suite", suite)
    shutil.copy2(suite / "global.json", work / "global.json")
    with (work / "build.log").open("w") as log:
        subprocess.run(["dotnet", "publish", str(build_source / "TestSuites/RDP/Client/src/TestSuite/RDP_ClientTestSuite.csproj"), "-c", "Release", "-o", str(suite / "Bin"), f"-p:TestSuiteRoot={build_source}", "-p:NuGetAudit=false", "--disable-build-servers", "-m:1"], cwd=work, check=True, stdout=log, stderr=subprocess.STDOUT)
    cert, fingerprint = run.make_certificate(work)
    variants = ["original", "corrected-tpkt"]
    if args.private_core:
        variants.append("patched-client-mcs")
    previous_dyld = os.environ.get("DYLD_LIBRARY_PATH")
    results = []
    for variant in variants:
        os.environ["WPTS_SLOW_TPKT_FIX"] = "1" if variant == "corrected-tpkt" else "0"
        if variant == "patched-client-mcs":
            os.environ["DYLD_LIBRARY_PATH"] = str(args.private_core.resolve())
        elif previous_dyld is None:
            os.environ.pop("DYLD_LIBRARY_PATH", None)
        else:
            os.environ["DYLD_LIBRARY_PATH"] = previous_dyld
        for mode in ("standard", "bridge"):
            for label, case in CASES.items():
                if variant == "corrected-tpkt" and label != "tpkt":
                    continue
                if variant == "patched-client-mcs" and label != "mcs":
                    continue
                destination = work / (f"{variant}-{label}-{mode}" if variant == "original" else f"{variant}-{mode}")
                result = run.run_case(binary, suite, mode, case, destination, cert, fingerprint, 60, options=CaseOptions())
                result.update(variant=variant, evidence=destination.name)
                results.append(result)
                run.write_json(work / "summary.json", results)
                print(json.dumps(result), flush=True)
    run.write_json(work / "provenance.json", {"microsoft_revision": revision, "freerdp_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(), "client_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "diagnostic_instrumentation_sha256": hashlib.sha256(Path(__file__).with_name("instrumentation.patch").read_bytes()).hexdigest()})


if __name__ == "__main__":
    main()
