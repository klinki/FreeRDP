#!/usr/bin/env python3
"""Exercise disconnect verification in actual prepared Microsoft SDK assemblies."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite-bin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    suite, output = args.suite_bin.resolve(), args.output.resolve()
    required = ['Microsoft.Protocols.TestTools.StackSdk.dll',
                'Microsoft.Protocols.TestTools.StackSdk.Transport.dll',
                'Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.dll']
    for name in required:
        if not (suite / name).is_file():
            parser.error(f'Missing Microsoft assembly: {suite / name}')
    if output.exists():
        parser.error(f'Choose a new output directory: {output}')
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='wpts-disconnect-regression-') as directory:
        working = Path(directory)
        for name in ['Program.cs', 'Regression.csproj']:
            shutil.copy2(ROOT / name, working / name)
        (working / 'global.json').write_text(json.dumps({'sdk': {
            'version': '8.0.100', 'rollForward': 'latestFeature', 'allowPrerelease': False,
        }}) + '\n')
        destination = working / 'bin'
        with (output / 'build.log').open('w') as log:
            build = subprocess.run([
                'dotnet', 'build', 'Regression.csproj', f'-p:SuiteBin={suite}',
                '--disable-build-servers', '-o', str(destination),
            ], cwd=working, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        if build.returncode:
            raise RuntimeError(f'Regression build failed; see {output / "build.log"}')
        # Ensure runtime uses the selected exact assemblies, independently of
        # MSBuild reference-resolution caching and dependency-copy behavior.
        for path in suite.glob('*.dll'):
            shutil.copy2(path, destination / path.name)
        hashes = {name: hashlib.sha256((suite / name).read_bytes()).hexdigest()
                  for name in required}
        for name, digest in hashes.items():
            assert hashlib.sha256((destination / name).read_bytes()).hexdigest() == digest
        result = subprocess.run(['dotnet', str(destination / 'Regression.dll')],
                                cwd=working, text=True, capture_output=True, timeout=30)
        (output / 'run.log').write_text(result.stdout + result.stderr)
        report = dict(suite_bin=str(suite), assembly_sha256=hashes,
                      program_sha256=hashlib.sha256((ROOT / 'Program.cs').read_bytes()).hexdigest(),
                      exit_code=result.returncode,
                      passed=[line[5:] for line in result.stdout.splitlines() if line.startswith('PASS ')],
                      failed=[line[5:] for line in result.stdout.splitlines() if line.startswith('FAIL ')])
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(result.stdout, end='')
        if result.stderr:
            print(result.stderr, end='')
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
