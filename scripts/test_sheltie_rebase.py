"""Exercise update preparation and recovery in disposable local repositories."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "sheltie_rebase", Path(__file__).with_name("sheltie-rebase.py")
)
rebase = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rebase)


class RebaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Rebase Test")
        self.git("config", "user.email", "rebase@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.commit_file("shared.txt", "base\n", "old upstream")
        self.base = self.git("rev-parse", "HEAD")
        self.git("tag", "old-upstream")
        self.git("switch", "-c", "upstream-side")
        self.commit_file("upstream.txt", "new upstream\n", "new upstream")
        self.target = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/upstream/master", self.target)
        self.git("switch", "main")
        self.commit_file("downstream.txt", "Sheltie patch\n", "downstream patch")
        self.source = self.git("rev-parse", "HEAD")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args],
                                       text=True, stderr=subprocess.PIPE).strip()

    def commit_file(self, name, text, message):
        (self.repo / name).write_text(text)
        self.git("add", name)
        self.git("commit", "-m", message)

    def plan(self, **kwargs):
        return rebase.plan(self.repo, self.target, **kwargs)

    def test_plan_is_read_only_and_finds_old_upstream_boundary(self):
        before = self.git("show-ref")
        update = self.plan()
        self.assertEqual(update["base"], self.base)
        self.assertEqual(update["count"], "1")
        self.assertEqual(self.git("show-ref"), before)
        self.assertEqual(self.git("symbolic-ref", "--short", "HEAD"), "main")

    def test_apply_retains_source_and_backup_even_with_update_refs_enabled(self):
        self.git("config", "rebase.updateRefs", "true")
        update = self.plan()
        self.assertEqual(rebase.apply(update), 0)
        self.assertEqual(self.git("rev-parse", "main"), self.source)
        self.assertEqual(self.git("rev-parse", update["backup"]), self.source)
        self.assertEqual(self.git("symbolic-ref", "--short", "HEAD"), update["update"])
        self.git("merge-base", "--is-ancestor", self.target, "HEAD")
        self.assertTrue((self.repo / "downstream.txt").exists())
        self.assertTrue((self.repo / "upstream.txt").exists())

    def test_dirty_worktree_is_rejected_before_creating_refs(self):
        (self.repo / "shared.txt").write_text("uncommitted\n")
        before = self.git("show-ref")
        with self.assertRaisesRegex(rebase.RebaseError, "tracked changes"):
            rebase.apply(self.plan())
        self.assertEqual(self.git("show-ref"), before)
        self.assertEqual((self.repo / "shared.txt").read_text(), "uncommitted\n")

    def test_conflict_can_be_aborted_without_losing_original_stack(self):
        self.git("switch", "upstream-side")
        self.commit_file("shared.txt", "upstream edit\n", "upstream conflict")
        self.target = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/upstream/master", self.target)
        self.git("switch", "main")
        self.commit_file("shared.txt", "fork edit\n", "fork conflict")
        original = self.git("rev-parse", "HEAD")
        update = self.plan()
        self.assertNotEqual(rebase.apply(update), 0)
        self.assertIn("UU shared.txt", self.git("status", "--porcelain"))
        self.assertEqual(self.git("rev-parse", "main"), original)
        self.assertEqual(self.git("rev-parse", update["backup"]), original)
        self.git("rebase", "--abort")
        self.assertEqual(self.git("rev-parse", "HEAD"), original)
        self.assertEqual((self.repo / "shared.txt").read_text(), "fork edit\n")

    def test_merge_structure_survives_rebase(self):
        self.git("switch", "-c", "feature")
        self.commit_file("feature.txt", "feature\n", "feature")
        self.git("switch", "main")
        self.commit_file("another.txt", "another\n", "another patch")
        self.git("merge", "--no-ff", "feature", "-m", "Merge: Add feature")
        update = self.plan()
        self.assertEqual(rebase.apply(update), 0)
        self.assertEqual(self.git("rev-list", "--count", "--merges", f"{self.target}..HEAD"), "1")
        self.assertTrue((self.repo / "feature.txt").exists())

    def test_existing_update_branch_and_invalid_boundary_are_rejected(self):
        with self.assertRaises(rebase.RebaseError):
            self.plan(update_branch="main")
        with self.assertRaises(rebase.RebaseError):
            self.plan(old_base=self.target)
        with self.assertRaisesRegex(rebase.RebaseError, "already the current base"):
            rebase.plan(self.repo, self.base)

    def test_apply_requires_the_source_checkout(self):
        update = self.plan(old_base="old-upstream", update_branch="update/test")
        self.git("switch", "upstream-side")
        with self.assertRaisesRegex(rebase.RebaseError, "Check out main"):
            rebase.apply(update)
        self.assertEqual(self.git("branch", "--list", update["backup"]), "")


if __name__ == "__main__":
    unittest.main()
