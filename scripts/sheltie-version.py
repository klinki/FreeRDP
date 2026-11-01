#!/usr/bin/env python3
"""Calculate Sheltie build versions and release tags without modifying Git state."""

import argparse
import json
from pathlib import Path
import re
import subprocess


class VersionError(Exception):
    pass


def git(repo, *args):
    result = subprocess.run(
        ["git", "-C", str(repo), *args], capture_output=True, text=True
    )
    if result.returncode:
        raise VersionError(result.stderr.strip() or "Git command failed")
    return result.stdout.strip()


def calculate(repo, ref="HEAD", upstream_ref="upstream/master"):
    if git(repo, "rev-parse", "--is-shallow-repository") == "true":
        raise VersionError("Use a full clone; shallow history cannot determine versions reliably.")
    revision = git(repo, "rev-parse", "--verify", f"{ref}^{{commit}}")
    base = git(repo, "merge-base", revision, upstream_ref)
    metadata = git(repo, "show", f"{base}:cmake/GetProjectVersion.cmake")
    match = re.search(r'set\(RAW_VERSION_STRING\s+"([^"\n]+)"\)', metadata)
    if not match or not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", match[1]):
        raise VersionError("Cannot read the upstream version from cmake/GetProjectVersion.cmake.")
    upstream_version = match[1]
    # A release tag is authoritative when its CMake metadata still says dev/rc.
    release_tags = git(repo, "tag", "--points-at", base).splitlines()
    versions = {tag.removeprefix("v") for tag in release_tags
                if re.fullmatch(r"v?\d+\.\d+\.\d+", tag)}
    if len(versions) > 1:
        raise VersionError("The upstream base has conflicting release version tags.")
    if versions:
        upstream_version = versions.pop()

    # Fork release numbers are global: they continue across upstream bases, so
    # every fork release tag reserves its number regardless of upstream version.
    pattern = re.compile(r"v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?-sheltie\.([1-9]\d*)")
    tags = []
    for tag in git(repo, "tag", "--list").splitlines():
        match = pattern.fullmatch(tag)
        if match:
            tags.append((int(match[1]), tag))
    next_number = max((number for number, _ in tags), default=0) + 1
    next_tag = f"v{upstream_version}-sheltie.{next_number}"

    # Treat a merged patch branch as one integration step. Release anchors must
    # be on this same path, so a tag inside a merged branch cannot reset it.
    first_parents = set(git(repo, "rev-list", "--first-parent", revision).splitlines())
    previous = max((item for item in tags
                    if git(repo, "rev-parse", f"{item[1]}^{{commit}}") in first_parents),
                   default=None)
    previous_tag = previous[1] if previous else None
    anchor = previous_tag or base
    distance = int(git(repo, "rev-list", "--count", "--first-parent", f"{anchor}..{revision}"))
    # Ignore untracked local logs/build artifacts, and do not attach worktree
    # dirtiness to a different revision requested with --ref.
    dirty = revision == git(repo, "rev-parse", "HEAD") and bool(
        git(repo, "status", "--porcelain", "--untracked-files=no")
    )
    exact_release = previous_tag if previous_tag and distance == 0 else None
    short_revision = revision[:12]
    if exact_release:
        version = exact_release[1:]
        if dirty:
            version += f"+g{short_revision}.dirty"
    else:
        version = f"{next_tag[1:]}.dev.{distance}+g{short_revision}"
        if dirty:
            version += ".dirty"
    return dict(version=version, next_tag=next_tag, upstream_version=upstream_version,
                upstream_base=base, revision=revision, release_tag=exact_release,
                previous_tag=previous_tag, commits_since_anchor=distance, dirty=dirty)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--ref", default="HEAD", help="Commit or branch to describe (default: HEAD)")
    parser.add_argument("--upstream-ref", default="upstream/master",
                        help="Upstream branch or release tag (default: upstream/master)")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--next-tag", action="store_true", help="Print the next unused local release tag")
    output.add_argument("--json", action="store_true", help="Print version and source metadata as JSON")
    args = parser.parse_args()
    try:
        result = calculate(args.repo, args.ref, args.upstream_ref)
    except VersionError as error:
        parser.exit(1, f"error: {error}\n")
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(result["next_tag"] if args.next_tag else result["version"])


if __name__ == "__main__":
    main()
