"""Exercise real Mach-O relocation and rejection of broken distribution inputs."""
import importlib.util
import io
import plistlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import tarfile
import unittest

spec = importlib.util.spec_from_file_location("macos_bundle", Path(__file__).parents[1] / "macos_bundle.py")
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


@unittest.skipUnless(shutil.which("xcrun"), "Requires macOS with Xcode")
class MachOBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name).resolve()
        self.source = self.folder / "source"
        self.source.mkdir()
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        subprocess.run(["git", "-C", str(self.source), "config", "user.email", "fixture@example.test"], check=True)
        subprocess.run(["git", "-C", str(self.source), "config", "user.name", "Packaging Fixture"], check=True)
        subprocess.run(["git", "-C", str(self.source), "remote", "add", "origin", "https://example.test/fixture.git"], check=True)
        (self.source / "LICENSE").write_text("Fixture license\n")
        subprocess.run(["git", "-C", str(self.source), "add", "LICENSE"], check=True)
        subprocess.run(["git", "-C", str(self.source), "commit", "-qm", "Fixture"], check=True)
        self.build = self.folder / "build"
        self.build.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def compile(self, output, code, *flags, target="26.0"):
        output.parent.mkdir(parents=True, exist_ok=True)
        source = output.with_suffix(".c")
        source.write_text(code)
        subprocess.run(["xcrun", "clang", "-arch", "arm64", f"-mmacosx-version-min={target}",
                        str(source), "-o", str(output), *map(str, flags)], check=True, capture_output=True)
        return output

    def testRelocationWorksWithOriginalLibrariesRemoved(self):
        library = self.compile(self.build / "libfixture.dylib", "int fixture(void) { return 0; }",
                               "-dynamiclib", "-Wl,-headerpad_max_install_names")
        executable = self.compile(self.build / "fixture", "extern int fixture(void); int main(void) { return fixture(); }",
                                  library, "-Wl,-headerpad_max_install_names")
        output = self.folder / "runtime"
        bundle.stage(executable, self.build, self.source, output, "26.0")
        shutil.rmtree(self.build)
        moved = self.folder / "A folder with spaces"
        shutil.move(output, moved)
        bundle.verify(moved, "26.0")
        subprocess.run([str(moved / "Contents/Helpers/fixture")], check=True)

    def testSourceArchiveIncludesRuntimeChangesAndBuildRecipes(self):
        (self.source / "wheel-feature.txt").write_text("Per-session mouse wheel reversal\n")
        subprocess.run(["git", "-C", str(self.source), "add", "wheel-feature.txt"], check=True)
        subprocess.run(["git", "-C", str(self.source), "commit", "-qm", "Runtime feature"], check=True)
        executable = self.compile(self.build / "sdl-freerdp", "int main(void) { return 0; }")
        runtime = self.folder / "runtime"
        bundle.stage(executable, self.build, self.source, runtime, "26.0")
        output = self.folder / "runtime-sources.tar.gz"
        bundle.collect_sources(runtime, self.source, output)
        with tarfile.open(output) as archive:
            prefix = "runtime-sources/launcher-build-recipes/"
            for file in ["build-engine.sh", "macos_bundle.py"]:
                self.assertTrue(archive.getmember(prefix + file).isfile())
            with tarfile.open(fileobj=io.BytesIO(archive.extractfile("runtime-sources/FreeRDP.tar").read())) as engine:
                self.assertEqual(engine.extractfile("wheel-feature.txt").read(), b"Per-session mouse wheel reversal\n")

    def testRejectsTooNewMinimumOS(self):
        executable = self.compile(self.build / "fixture", "int main(void) { return 0; }", target="27.0")
        with self.assertRaisesRegex(ValueError, "incompatible minimum"):
            bundle.stage(executable, self.build, self.source, self.folder / "runtime", "26.0")

    def testUniversalHelpersRequireExplicitVerifierOptIn(self):
        arm = self.compile(self.build / "helper-arm", "int main(void) { return 0; }")
        intel = self.build / "helper-intel"
        subprocess.run(["xcrun", "clang", "-arch", "x86_64", "-mmacosx-version-min=26.0",
                        str(arm.with_suffix(".c")), "-o", str(intel)], check=True, capture_output=True)
        universal = self.build / "helper"
        subprocess.run(["lipo", "-create", str(arm), str(intel), "-output", str(universal)], check=True)
        with self.assertRaisesRegex(ValueError, "expected arm64 only"):
            bundle.inspect(universal, "26.0")
        info = bundle.inspect(universal, "26.0", allow_universal=True)
        self.assertEqual(set(info["architectures"]), {"arm64", "x86_64"})

    def testRejectsMissingLibrary(self):
        library = self.compile(self.build / "libfixture.dylib", "int fixture(void) { return 0; }", "-dynamiclib")
        executable = self.compile(self.build / "fixture", "extern int fixture(void); int main(void) { return fixture(); }", library)
        library.unlink()
        with self.assertRaisesRegex(ValueError, "unresolved dependency"):
            bundle.stage(executable, self.build, self.source, self.folder / "runtime", "26.0")

    def testRejectsDuplicateLibraryFilenames(self):
        left = self.compile(self.build / "left/libduplicate.dylib", "int left(void) { return 0; }", "-dynamiclib")
        right = self.compile(self.build / "right/libduplicate.dylib", "int right(void) { return 0; }", "-dynamiclib")
        executable = self.compile(self.build / "fixture", "extern int left(void); extern int right(void); int main(void) { return left() + right(); }", left, right)
        with self.assertRaisesRegex(ValueError, "filename collision"):
            bundle.stage(executable, self.build, self.source, self.folder / "runtime", "26.0")

    def testBuildTreeFreeRDPWinsOverInstalledLibrary(self):
        own = self.compile(self.build / "libwinpr3.3.dylib", "int fixture(void) { return 0; }", "-dynamiclib")
        installed = self.compile(self.folder / "installed/libwinpr3.3.dylib", "int fixture(void) { return 42; }", "-dynamiclib")
        executable = self.compile(self.build / "fixture", "extern int fixture(void); int main(void) { return fixture(); }",
                                  installed, "-Wl,-headerpad_max_install_names")
        output = self.folder / "runtime"
        bundle.stage(executable, self.build, self.source, output, "26.0")
        subprocess.run([str(output / "Contents/Helpers/fixture")], check=True)
        self.assertEqual(bundle.build_libraries(self.build)["libwinpr3.3.dylib"], own.resolve())

    def testVerifierRejectsEscapingSymlink(self):
        executable = self.compile(self.build / "fixture", "int main(void) { return 0; }")
        output = self.folder / "runtime"
        bundle.stage(executable, self.build, self.source, output, "26.0")
        (output / "Contents/Frameworks/escape").symlink_to(self.build)
        with self.assertRaisesRegex(ValueError, "Symlink escapes"):
            bundle.verify(output, "26.0")

    def testAppExecutableComesFromBundleMetadataAfterRebranding(self):
        executable = self.compile(self.build / "Sheltie", "int main(void) { return 0; }")
        output = self.folder / "Sheltie.app"
        bundle.stage(executable, self.build, self.source, output, "26.0")
        macos = output / "Contents/MacOS"
        macos.mkdir()
        shutil.move(output / "Contents/Helpers/Sheltie", macos / "Sheltie")
        metadata = dict(CFBundleExecutable="Sheltie", CFBundleIdentifier="test.sheltie.fixture",
                        CFBundleName="Sheltie", CFBundlePackageType="APPL",
                        CFBundleShortVersionString="1.0.0", CFBundleVersion="1",
                        LSMinimumSystemVersion="26.0")
        plist = output / "Contents/Info.plist"
        plist.write_bytes(plistlib.dumps(metadata))
        subprocess.run(["codesign", "--force", "--sign", "-", str(output)], check=True)
        bundle.verify(output, "26.0")
        metadata["CFBundleExecutable"] = "Missing"
        plist.write_bytes(plistlib.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "Missing app executable"):
            bundle.verify(output, "26.0")
