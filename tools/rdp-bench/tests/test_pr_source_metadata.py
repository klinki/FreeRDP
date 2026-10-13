"""PR metadata fingerprints actual sources and keeps missing evidence explicit."""
import hashlib
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from rdpbench.adapter import PR_NATIVE_SOURCE_NAMES,PR_ADAPTER,native_source_files,native_source_receipt
from rdpbench.calibration import engine_sources
from rdpbench.common import digest,read_json,sha256

class PrSourceMetadataTests(unittest.TestCase):
    def test_actual_pr_files_are_required_in_calibration(self):
        files=native_source_files(ROOT/'../../libfreerdp/core',backend=2)
        self.assertEqual(set(files),set(PR_NATIVE_SOURCE_NAMES))
        self.assertTrue(all(p.is_file() for p in files.values()))
        engines=engine_sources(ROOT)
        for name,path in files.items():self.assertEqual(engines['../../libfreerdp/core/'+name],path)
        self.assertNotIn('../../libfreerdp/core/rdp_bench_private.inc',engines)

    def test_checkout_source_receipt_cannot_silently_be_empty(self):
        receipt=native_source_receipt(ROOT/'../../libfreerdp/core',dict(native_adapter=PR_ADAPTER))
        self.assertEqual(receipt['basis'],'checkout-files')
        self.assertEqual(len(receipt['files']),5)
        for name,value in receipt['files'].items():self.assertEqual(value,sha256(ROOT.parents[1]/name))
        self.assertNotEqual(digest(receipt['files']),digest({}))

    def test_missing_source_is_unknown_or_explicit_recorded_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing=native_source_receipt(temporary,dict(native_adapter=PR_ADAPTER))
            self.assertIsNone(missing['files']);self.assertEqual(missing['basis'],'unavailable')
            records={'libfreerdp/core/rdpudp.c':'ab'*32}
            recorded=native_source_receipt(temporary,dict(native_adapter=PR_ADAPTER,native_source_sha256=records))
            self.assertEqual(recorded['files'],records)
            self.assertIn('declared-build-metadata',recorded['basis'])
            self.assertEqual(set(recorded['missing']),set(PR_NATIVE_SOURCE_NAMES))

    def test_build_helper_enables_udp_and_records_all_five_actual_sources(self):
        spec=importlib.util.spec_from_file_location('pr_bench_build',ROOT/'build.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            build=Path(temporary);(build/'libfreerdp').mkdir()
            (build/'libfreerdp/libfreerdp3.dylib').write_bytes(b'opaque fixture; not loaded')
            (build/'CMakeCache.txt').write_text('WITH_RDPEUDP:BOOL=ON\nWITH_RDP_BENCH:BOOL=ON\n')
            def output(args,**kwargs):return 'fixture' if kwargs.get('text') else b''
            with patch.object(sys,'argv',['build.py','--build',str(build)]),patch.object(module.subprocess,'run') as run,patch.object(module.subprocess,'check_output',side_effect=output):module.main()
            self.assertIn('-DWITH_RDPEUDP=ON',run.call_args_list[0].args[0])
            metadata=read_json(build/'rdp-bench-build.json')
            self.assertEqual(metadata['native_adapter'],PR_ADAPTER)
            self.assertEqual(set(metadata['native_source_sha256']),{'libfreerdp/core/'+n for n in PR_NATIVE_SOURCE_NAMES})
            self.assertEqual(metadata['native_adapter_sha256'],sha256(ROOT.parents[1]/'libfreerdp/core/rdpudp_bench_private.inc'))

if __name__=='__main__':unittest.main()
