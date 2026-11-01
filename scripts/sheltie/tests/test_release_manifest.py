import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release_manifest", Path(__file__).parents[1] / "release_manifest.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

REVISION = "a" * 40


class ReleaseManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.runtime = Path(self.temp.name) / "runtime"
        (self.runtime / release.MANIFEST).parent.mkdir(parents=True)
        (self.runtime / release.MANIFEST).write_text(json.dumps(
            dict(engine_revision=REVISION, architecture="arm64", minimum_macos="26.0", dependencies=[])))
        self.version = dict(version="3.32.2-sheltie.1", release_tag="v3.32.2-sheltie.1", upstream_version="3.32.2",
                            upstream_base="b" * 40, revision=REVISION, dirty=False)

    def tearDown(self):
        self.temp.cleanup()

    def client(self, output):
        path = self.runtime / release.CLIENT
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"#!/bin/sh\ntest \"$1\" = /launcher-capabilities || exit 2\ncat <<'JSON'\n{output}\nJSON\n")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def testCompletesManifestFromTheBuiltClient(self):
        self.client(json.dumps(dict(schemaVersion=1, bridgeProtocolVersion=1,
                                    capabilities=["window_geometry", "auth", "auth"])))
        manifest = release.complete(self.runtime, self.version, "v3.32.2-sheltie.1")
        self.assertEqual(manifest["bridge"], dict(schema_version=1, protocol_version=1,
                                                  capabilities=["auth", "window_geometry"]))
        self.assertEqual(manifest["engine_version"], "3.32.2-sheltie.1")
        self.assertEqual(manifest["release_tag"], "v3.32.2-sheltie.1")
        self.assertEqual(manifest["upstream_base"], "b" * 40)
        self.assertEqual(json.loads((self.runtime / release.MANIFEST).read_text()), manifest)

    def testDevelopmentBuildHasNoReleaseTag(self):
        self.client(json.dumps(dict(schemaVersion=1, bridgeProtocolVersion=1, capabilities=["auth"])))
        version = dict(self.version, version="3.32.2-sheltie.1.dev.3+gaaaaaaaaaaaa", release_tag=None)
        self.assertIsNone(release.complete(self.runtime, version, "")["release_tag"])

    def testRejectsMissingCapabilitiesAndForeignRevision(self):
        for output in (json.dumps(dict(schemaVersion=1, bridgeProtocolVersion=1, capabilities=[])),
                       json.dumps(dict(schemaVersion="1", bridgeProtocolVersion=1, capabilities=["auth"])),
                       "not json"):
            self.client(output)
            with self.assertRaises(ValueError, msg=output):
                release.complete(self.runtime, self.version, "")
        with self.assertRaisesRegex(ValueError, "versioned revision"):
            release.complete(self.runtime, dict(self.version, revision="c" * 40), "", query=lambda _: {})

    def testReleaseTagMustBeTheDerivedCleanRelease(self):
        release.check_release(self.version, "v3.32.2-sheltie.1")
        for tag, change in (("v3.32.2-sheltie.2", {}), ("sheltie/beta.3", {}), ("v3.32.2", {}),
                            ("v3.32.2-sheltie.1", dict(release_tag=None)),
                            ("v3.32.2-sheltie.1", dict(dirty=True)),
                            ("v3.32.2-sheltie.1", dict(upstream_version="3.33.0"))):
            with self.assertRaises(ValueError, msg=(tag, change)):
                release.check_release(dict(self.version, **change), tag)


if __name__ == "__main__":
    unittest.main()
