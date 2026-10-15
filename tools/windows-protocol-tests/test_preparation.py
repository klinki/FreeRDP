"""Preparation integrity: patches are pinned and stale assemblies cannot run."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import prepare
from profiles import PIN, suite_patches
from run import run


class PreparationTests(unittest.TestCase):
    def test_patch_bytes_and_paths_are_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'patches').mkdir()
            file = root / 'patches/driver.patch'
            file.write_bytes(b'reviewed patch\n')
            item = dict(path='patches/driver.patch', sha256=hashlib.sha256(file.read_bytes()).hexdigest())
            manifest = dict(patches=[item])
            self.assertEqual(suite_patches(manifest, root), [item])
            file.write_bytes(b'changed patch\n')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                suite_patches(manifest, root)
            with self.assertRaisesRegex(ValueError, 'Invalid Microsoft patch path'):
                suite_patches(dict(patches=[dict(item, path='../driver.patch')]), root)
            file.write_bytes(b'reviewed patch\n')
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                suite_patches(dict(patches=[item, item]), root)

    def test_old_assemblies_cannot_run_with_new_driver_patches(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'provenance.json').write_text(json.dumps(dict(repository=PIN['repository'], revision=PIN['revision'])))
            binary = root / 'client'
            binary.write_text('#!/bin/sh\nexit 0\n'); binary.chmod(0o700)
            args = SimpleNamespace(profile='quick', case=None, client=binary, suite=root, output=root/'results')
            with patch('run.suite_patches', return_value=[dict(path='patches/new.patch', sha256='a'*64)]):
                with self.assertRaisesRegex(ValueError, 'Suite driver patches do not match'):
                    run(args)
            self.assertFalse(args.output.exists())

    def test_preparation_applies_patch_to_export_without_mutating_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output, harness = root/'source', root/'output', root/'harness'
            source.mkdir(); harness.mkdir(); (harness/'patches').mkdir()
            def git(*args):
                return subprocess.check_output(['git', *args], cwd=source, text=True).strip()
            git('init', '--quiet')
            (source/'AssemblyInfo').mkdir(); (source/'AssemblyInfo/test.txt').write_text('before\n')
            (source/'ProtoSDK').mkdir(); (source/'ProtoSDK/test.txt').write_text('sdk\n')
            suite = source/'TestSuites/RDP/Client'
            for relative in ('src/TestSuite/TestData', 'src/TestSuite/RDPEGFX/H264TestData',
                             'src/TestSuite/RDPEGFX/H264TestData/BaseImage',
                             'src/TestSuite/RDPEDISP/RdpedispEnhancedAdapterImages', 'Setup/Data'):
                folder = suite/relative; folder.mkdir(parents=True, exist_ok=True)
                (folder/'fixture.txt').write_text('fixture\n')
            (source/'LICENSE.txt').write_text('license\n')
            git('add', '.')
            git('-c', 'user.name=Harness Test', '-c', 'user.email=test@example.invalid', 'commit', '--quiet', '-m', 'Fixture')
            revision = git('rev-parse', 'HEAD')
            (source/'AssemblyInfo/untracked.txt').write_text('must not build\n')
            patch_file = harness/'patches/driver.patch'
            patch_file.write_text('--- a/AssemblyInfo/test.txt\n+++ b/AssemblyInfo/test.txt\n@@ -1 +1 @@\n-before\n+after\n')
            records = [dict(path='patches/driver.patch', sha256=hashlib.sha256(patch_file.read_bytes()).hexdigest())]
            real_run, real_output = subprocess.run, subprocess.check_output
            def invoke(args, **kwargs):
                if args[:2] == ['dotnet', 'publish']:
                    project = Path(args[2]); build_source = project.parents[5]
                    self.assertEqual((build_source/'AssemblyInfo/test.txt').read_text(), 'after\n')
                    self.assertFalse((build_source/'AssemblyInfo/untracked.txt').exists())
                    (output/'Bin').mkdir()
                    return SimpleNamespace(returncode=0)
                return real_run(args, **kwargs)
            def capture(args, **kwargs):
                return '8.0.test\n' if args == ['dotnet', '--version'] else real_output(args, **kwargs)
            with patch.object(prepare, 'ROOT', harness), patch.object(prepare, 'PIN', dict(repository='fixture', revision=revision)), \
                 patch.object(prepare, 'suite_patches', return_value=records), \
                 patch('prepare.subprocess.run', side_effect=invoke), patch('prepare.subprocess.check_output', side_effect=capture):
                prepare.prepare(source, output)
            self.assertEqual((source/'AssemblyInfo/test.txt').read_text(), 'before\n')
            self.assertEqual(git('status', '--porcelain', '--untracked-files=no'), '')
            self.assertEqual(json.loads((output/'provenance.json').read_text())['patches'], records)
            self.assertFalse(list(root.glob('wpts-build-*')))


if __name__ == '__main__':
    unittest.main()
