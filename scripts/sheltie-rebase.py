#!/usr/bin/env python3
"""Plan an upstream update, or rebase a separate review branch with --apply."""

import argparse
from datetime import datetime, timezone
from pathlib import Path
import shlex
import subprocess
import uuid


class RebaseError(Exception):
    pass


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args],
                            capture_output=True, text=True)
    if result.returncode:
        raise RebaseError(result.stderr.strip() or "Git command failed")
    return result.stdout.strip()


def plan(repo, onto, branch="main", old_base=None, upstream_ref="upstream/master",
         update_branch=None):
    repo = Path(repo).resolve()
    if git(repo, "rev-parse", "--is-shallow-repository") == "true":
        raise RebaseError("Use a full clone to identify and replay the downstream patch stack.")
    git(repo, "check-ref-format", "--branch", branch)
    source = git(repo, "rev-parse", "--verify", f"refs/heads/{branch}^{{commit}}")
    target = git(repo, "rev-parse", "--verify", f"{onto}^{{commit}}")
    base = (git(repo, "rev-parse", "--verify", f"{old_base}^{{commit}}") if old_base
            else git(repo, "merge-base", source, upstream_ref))
    git(repo, "merge-base", "--is-ancestor", base, source)
    if base == target:
        raise RebaseError("The requested upstream base is already the current base.")
    # Work on a separate branch so main and existing release tags stay intact.
    suffix = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    update = update_branch or f"update/sheltie-{suffix}"
    backup = f"backup/sheltie-rebase-{suffix}"
    for name in (update, backup):
        git(repo, "check-ref-format", "--branch", name)
        if git(repo, "branch", "--list", name):
            raise RebaseError(f"Branch already exists: {name}")
    return dict(repo=repo, branch=branch, source=source, target=target, base=base,
                update=update, backup=backup,
                count=git(repo, "rev-list", "--count", f"{base}..{source}"))


def apply(update):
    repo = update["repo"]
    current = git(repo, "symbolic-ref", "--quiet", "--short", "HEAD")
    if current != update["branch"]:
        raise RebaseError(f"Check out {update['branch']} before applying this plan.")
    if git(repo, "status", "--porcelain", "--untracked-files=no"):
        raise RebaseError("Commit or stash tracked changes first; this helper does not auto-stash.")
    for marker in ("rebase-merge", "rebase-apply", "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "sequencer"):
        path = Path(git(repo, "rev-parse", "--git-path", marker))
        if not path.is_absolute():
            path = repo / path
        if path.exists():
            raise RebaseError("Finish or abort the existing Git operation first.")
    if git(repo, "rev-parse", "HEAD") != update["source"]:
        raise RebaseError("The source branch changed while preparing the plan; run again.")
    git(repo, "branch", "--no-track", update["backup"], update["source"])
    git(repo, "switch", "--no-track", "-c", update["update"], update["source"])
    # Explicitly prevent users' updateRefs/autostash/autosquash settings from
    # moving main or folding patch commits during this review rebase.
    result = subprocess.run([
        "git", "-C", str(repo), "-c", "rebase.updateRefs=false", "rebase",
        "--rebase-merges", "--no-autostash", "--no-autosquash", "--onto",
        update["target"], update["base"],
    ])
    if result.returncode:
        print("\nRebase stopped on the update branch. main/source and the backup remain intact.")
        print("Resolve conflicts, git add the resolved files, then git rebase --continue.")
        print("Or run git rebase --abort to restore the update branch to its starting commit.")
        return result.returncode
    print("\nRebase completed on the update branch; the source branch has not moved.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--onto", required=True, help="Fetched upstream release tag or branch")
    parser.add_argument("--branch", default="main", help="Local source branch (default: main)")
    parser.add_argument("--from", dest="old_base", help="Explicit old upstream base")
    parser.add_argument("--upstream-ref", default="upstream/master",
                        help="Ref used to infer the old base (default: upstream/master)")
    parser.add_argument("--update-branch", help="Name of the new branch to review")
    parser.add_argument("--apply", action="store_true", help="Create backup/update branches and start rebase")
    args = parser.parse_args()
    try:
        update = plan(args.repo, args.onto, args.branch, args.old_base,
                      args.upstream_ref, args.update_branch)
        print(f"Source: {update['branch']} ({update['source']})")
        print(f"Old upstream base: {update['base']}")
        print(f"New upstream base: {update['target']}")
        print(f"Commits in downstream range: {update['count']} (including merges)")
        print(f"Backup branch: {update['backup']}")
        print(f"Update branch: {update['update']}")
        print("After rebasing, compare patches with:")
        print(shlex.join(["git", "-C", str(update["repo"]), "range-diff",
                          f"{update['base']}..{update['backup']}",
                          f"{update['target']}..{update['update']}"]))
        if not args.apply:
            print("\nPlan only; no changes made. Check both bases, then rerun with --apply.")
            return 0
        return apply(update)
    except RebaseError as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
