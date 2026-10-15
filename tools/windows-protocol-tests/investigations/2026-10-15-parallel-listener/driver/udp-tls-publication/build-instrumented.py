#!/usr/bin/env python3
"""Build a clearly instrumented Microsoft SDK for a deterministic scheduling regression."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
TARGET = Path('ProtoSDK/MS-RDPEMT/SecureChannel/RdpeudpTLSChannel.cs')
FIX = 'patches/udp-tls-callback-publication.patch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Clean pinned Microsoft Git checkout')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--variant', choices=('baseline', 'fixed'), required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    pin = json.loads((ROOT / 'suite.json').read_text())
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    if revision != pin['revision']:
        parser.error('Microsoft checkout does not match the pinned revision')
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=source):
        parser.error('Microsoft checkout has tracked changes')
    if output.exists():
        parser.error('Output must be a new directory')
    patches = []
    for item in pin['patches']:
        if args.variant == 'baseline' and item['path'] == FIX:
            continue
        patch = ROOT / item['path']
        if hashlib.sha256(patch.read_bytes()).hexdigest() != item['sha256']:
            parser.error(f'Patch hash mismatch: {patch}')
        patches.append(item)
    output.mkdir(parents=True)
    (output / 'global.json').write_text(json.dumps({'sdk': {'version': '8.0.100', 'rollForward': 'latestFeature', 'allowPrerelease': False}}) + '\n')
    with tempfile.TemporaryDirectory(prefix='wpts-publication-source-', dir=output) as directory:
        temporary = Path(directory); export = temporary / 'source'; export.mkdir()
        archive = temporary / 'source.tar'
        subprocess.run(['git', 'archive', '--format=tar', '-o', str(archive), revision, 'AssemblyInfo', 'ProtoSDK', 'TestSuites/RDP/Client'], cwd=source, check=True)
        subprocess.run(['tar', '-xf', str(archive), '-C', str(export)], check=True)
        subprocess.run(['git', 'init', '--quiet'], cwd=export, check=True)
        for item in patches:
            subprocess.run(['git', 'apply', str(ROOT / item['path'])], cwd=export, check=True)
        before = (export / TARGET).read_bytes()
        subprocess.run(['python3', str(HERE / 'instrument.py'), str(export)], check=True)
        after = (export / TARGET).read_bytes()
        with (output / 'sdk-build.log').open('w') as log:
            subprocess.run(['dotnet', 'build', str(export / 'ProtoSDK/MS-RDPEMT/Rdpemt.csproj'), '-c', 'Release', '-o', str(output / 'Bin'), '--disable-build-servers', '-m:1'], cwd=output, stdout=log, stderr=subprocess.STDOUT, check=True)
        provenance = {
            'instrumented': True, 'variant': args.variant, 'revision': revision, 'patches': patches,
            'constructor_source_before_hook_sha256': hashlib.sha256(before).hexdigest(),
            'constructor_source_after_hook_sha256': hashlib.sha256(after).hexdigest(),
            'instrumentation_script_sha256': hashlib.sha256((HERE / 'instrument.py').read_bytes()).hexdigest(),
        }
        (output / 'instrumentation-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(f'Instrumented {args.variant} SDK: {output}')


if __name__ == '__main__':
    main()
