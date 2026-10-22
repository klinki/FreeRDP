"""Regression checks using disposable Git repositories, without network access."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "sheltie_version", Path(__file__).with_name("sheltie-version.py")
)
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Version Test")
        self.git("config", "user.email", "version@example.invalid")
        self.metadata = self.repo / "cmake/GetProjectVersion.cmake"
        self.metadata.parent.mkdir()
        self.metadata.write_text('set(RAW_VERSION_STRING "3.32.3-dev0")\n')
        self.commit("upstream")
        self.base = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/upstream/master", self.base)

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args], text=True, stderr=subprocess.PIPE
        ).strip()

    def commit(self, message):
        self.git("add", ".")
        self.git("commit", "--allow-empty", "-m", message)

    def calculate(self, **kwargs):
        return version.calculate(self.repo, **kwargs)

    def test_initial_master_snapshot_preserves_dev_suffix(self):
        self.commit("fork patch")
        result = self.calculate()
        self.assertEqual(result["upstream_base"], self.base)
        self.assertEqual(result["next_tag"], "v3.32.3-dev0-sheltie.1")
        self.assertRegex(result["version"], r"^3\.32\.3-dev0-sheltie\.1\.dev\.1\+g[0-9a-f]{12}$")

    def test_release_tag_is_authoritative_and_annotated_fork_tag_is_exact(self):
        self.git("tag", "3.32.2")
        self.commit("fork patch")
        self.git("tag", "-a", "v3.32.2-sheltie.1", "-m", "release")
        result = self.calculate()
        self.assertEqual(result["version"], "3.32.2-sheltie.1")
        self.assertEqual(result["next_tag"], "v3.32.2-sheltie.2")
        self.commit("next patch")
        self.assertEqual(self.calculate()["commits_since_anchor"], 1)

    def test_rebased_unreachable_tags_reserve_numbers_but_are_not_anchors(self):
        self.git("tag", "3.32.2")
        self.commit("old patch stack")
        self.git("tag", "v3.32.2-sheltie.9")
        self.git("switch", "-c", "rebased", self.base)
        self.commit("new patch stack")
        result = self.calculate()
        self.assertEqual(result["next_tag"], "v3.32.2-sheltie.10")
        self.assertIsNone(result["previous_tag"])
        self.assertEqual(result["commits_since_anchor"], 1)

    def test_new_upstream_version_restarts_fork_numbering(self):
        self.git("tag", "v3.32.2-sheltie.9")
        self.metadata.write_text('set(RAW_VERSION_STRING "3.33.0")\n')
        self.commit("new upstream release")
        self.git("tag", "3.33.0")
        self.git("update-ref", "refs/remotes/upstream/master", self.git("rev-parse", "HEAD"))
        self.assertEqual(self.calculate()["next_tag"], "v3.33.0-sheltie.1")

    def test_dirty_tracked_files_and_historical_refs(self):
        (self.repo / "local.log").write_text("untracked output")
        self.assertFalse(self.calculate()["dirty"])
        self.metadata.write_text("modified locally\n")
        self.assertTrue(self.calculate()["dirty"])
        self.assertTrue(self.calculate()["version"].endswith(".dirty"))
        self.git("restore", "cmake/GetProjectVersion.cmake")
        self.commit("fork patch")
        self.metadata.write_text("modified locally again\n")
        self.assertFalse(self.calculate(ref=self.base)["dirty"])

    def test_explicit_upstream_ref_and_missing_ref(self):
        self.git("tag", "3.32.2")
        self.assertEqual(self.calculate(upstream_ref="3.32.2")["upstream_version"], "3.32.2")
        with self.assertRaises(version.VersionError):
            self.calculate(upstream_ref="missing-upstream")


if __name__ == "__main__":
    unittest.main()
