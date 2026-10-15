#!/usr/bin/env python3
"""Check the prepared Microsoft UDP compatibility repairs without a FreeRDP client."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("suite_bin", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent
suite_bin = args.suite_bin.resolve()
if not (suite_bin / "Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpeudp2.dll").is_file():
    parser.error("suite_bin must contain the prepared Microsoft SDK assemblies")
with tempfile.TemporaryDirectory(prefix="wpts-udp-regressions-") as temporary:
    output = Path(temporary)
    shutil.copy2(root / "regressions.csproj", output / "regressions.csproj")
    shutil.copy2(root / "RegressionChecks.cs", output / "Program.cs")
    (output / "global.json").write_text(json.dumps({"sdk": {
        "version": "8.0.100", "rollForward": "latestFeature", "allowPrerelease": False}}))
    subprocess.run(["dotnet", "build", "regressions.csproj", f"-p:SuiteBin={suite_bin}",
                    "--ignore-failed-sources"], cwd=output, check=True, timeout=120)
    subprocess.run(["dotnet", str(output / "bin/Debug/net8.0/regressions.dll")],
                   cwd=output, check=True, timeout=30)
