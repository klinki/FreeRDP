# FreeRDP: A Remote Desktop Protocol Implementation

This repository is a downstream fork of [FreeRDP](https://github.com/FreeRDP/FreeRDP)
for use with **Sheltie**, a native macOS Remote Desktop launcher. The fork focuses
on macOS improvements, including multimonitor support, per-monitor scaling,
rendering and session reliability, and a native launcher bridge between Sheltie
and FreeRDP's SDL3 client.

The Sheltie bridge provides structured communication for session lifecycle,
authentication, display configuration and session controls. FreeRDP remains the
RDP engine; Sheltie provides the macOS interface around it.

FreeRDP is a free implementation of the Remote Desktop Protocol (RDP), released under the Apache license.
Enjoy the freedom of using your software wherever you want, the way you want it, in a world where
interoperability can finally liberate your computing experience.

## Fork versioning and upstream updates

`main` is the rolling downstream integration branch: the latest supported
upstream FreeRDP release plus the current Sheltie patch stack. It is currently
based on FreeRDP 3.33.0. Upstream updates normally target published FreeRDP
releases. The patch stack on FreeRDP 3.32.2 is preserved at tag
`v3.32.2-sheltie.1`, and the earlier stack on an upstream `master` snapshot at
`archive/master-pre-3.32.2-rebase`.

Since 3.33.0 the fork uses upstream's UDP multitransport, built with the
experimental `WITH_RDPEUDP` option, instead of its own UDP transport.

The downstream patches are rebased onto each new upstream base, reviewed and
tested before publication. Consequently, `main` may be rebased and force-pushed;
its commit history is not a stable release reference. Contributors should fetch
and rebase their work onto the updated `main` after an upstream update.

Stable fork releases use immutable tags such as `v3.32.2-sheltie.1`. The upstream
version identifies the FreeRDP base; `sheltie.N` is a fork-wide release counter.
Every fork release increments `N`, including the first release on a new upstream
base, so `v3.32.2-sheltie.1` may be followed by `v3.32.2-sheltie.2` and then
`v3.33.0-sheltie.3`. Each `N` is used once across all upstream versions, giving
every engine release a unique number. Published release tags are never moved.
Sheltie release builds pin an exact engine tag or commit rather than following
the moving `main` branch.

Each fork release tag builds, tests and publishes the runtime Sheltie bundles;
see [Sheltie runtime releases](scripts/sheltie/README.md).

Feature and fix branches are temporary development branches and may be rebased
before integration. Maintenance branches are created from release tags only when
an older release needs a fix; a permanent branch for every release is not required.

### Calculating fork versions

Use Python 3.9 or newer to calculate versions from the local Git history:

```sh
python3 scripts/sheltie-version.py              # Development or exact release version
python3 scripts/sheltie-version.py --next-tag   # Next available fork release tag
python3 scripts/sheltie-version.py --json       # Version, base commit and revision metadata
```

The script finds the common ancestor with `upstream/master` and reads its
FreeRDP version, preferring an upstream release tag at that commit. For a
release-based branch, pass the upstream tag explicitly with
`--upstream-ref 3.32.0`. Use `--ref` to describe a different branch or commit.

An exact fork release returns a version such as `3.32.2-sheltie.1`. Other
commits return a development version such as
`3.32.2-sheltie.2.dev.4+g0123456789ab`: the next fork release number, the number
of first-parent steps since the latest fork release on that path (or upstream
base), and the commit ID. A branch with 20 commits merged using `--no-ff` counts
as one step: the merge commit. Direct and fast-forwarded commits each count as
one step. Tags inside merged side branches reserve release numbers but do not
reset this counter. The rebase helper still reports the full downstream commit
count, including side-branch commits, to describe the work being replayed.
Modified tracked files add a `dirty` marker; untracked files are
ignored. Upstream development suffixes are retained, so a branch based on an
upstream development snapshot would use tags such as `v3.32.3-dev0-sheltie.1`,
distinct from release-based tags.

The next release number is one higher than the highest `N` among all local
`v<version>-sheltie.N` tags, for any upstream version, including tags preserved
across rebases. Legacy tags such as `sheltie/beta.2` do not affect it. After a
rebase onto a new upstream base, earlier release tags are no longer on the
branch, so the development distance counts from the new upstream base. Use a
full clone and fetch the fork's release tags before calculating a
release tag; unfetched remote tags cannot be considered. The script does not
fetch, create tags, change FreeRDP's build version, or modify the repository.

### Preparing an upstream rebase

Fetch the intended upstream release and inspect a rebase plan before applying it:

```sh
git fetch --no-tags upstream tag 3.33.0
python3 scripts/sheltie-rebase.py --onto 3.33.0
python3 scripts/sheltie-rebase.py --onto 3.33.0 --apply
```

Replace the example tag with the desired upstream release. The helper uses local
refs and does not fetch automatically. Its default source is `main`; the old
upstream base is its common ancestor with `upstream/master`. Check the printed
base before applying. Use `--from <old-base>` when the old base is known explicitly,
or `--upstream-ref <ref>` to change how it is inferred. An upstream branch such
as `upstream/master` can also be passed to `--onto` when intentionally updating
to a development snapshot.

Without `--apply`, the helper only prints a plan. Applying requires the source
branch to be checked out with no tracked changes or Git operation in progress.
It creates a `backup/sheltie-rebase-...` branch at the original commit, checks out
a new `update/sheltie-...` branch, and rebases the downstream range onto the chosen
commit while preserving merge structure. Use `--update-branch <name>` to name the
review branch. The source branch and release tags stay in place; automatic ref
updates, stashing and autosquashing are disabled for the rebase. Untracked files
are left to Git's normal overwrite protections.

If conflicts occur, resolve and stage the affected files, then run
`git rebase --continue`; `git rebase --abort` restores the update branch to its
starting point. The backup remains available in either case. The helper prints
a `git range-diff` command for comparing the old and new patch stacks; also
review merge resolutions, which `range-diff` does not cover. Build and test the
update branch before deliberately moving `main` to it. The helper does not move
`main`, publish tags, push history, or run project validation automatically.

The inherited upstream GitHub Actions workflows are disabled in this fork's
repository settings, not removed from the tree, so rebases do not conflict with
upstream workflow changes. After each upstream update, check the repository's
Actions workflow list and disable any newly added upstream workflow.

Run the helper regression checks without network access:

```sh
python3 -m unittest discover -s scripts -p 'test_sheltie_*.py'
```

## Code Quality Status

[![abi-checker](https://github.com/FreeRDP/FreeRDP/actions/workflows/abi-checker.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/abi-checker.yml)
[![clang-tidy-review](https://github.com/FreeRDP/FreeRDP/actions/workflows/clang-tidy.yml/badge.svg?event=pull_request_target)](https://github.com/FreeRDP/FreeRDP/actions/workflows/clang-tidy.yml)
[![CodeQL](https://github.com/FreeRDP/FreeRDP/actions/workflows/codeql-analysis.yml/badge.svg?branch=master)](https://github.com/FreeRDP/FreeRDP/actions/workflows/codeql-analysis.yml)
[![mingw-builder](https://github.com/FreeRDP/FreeRDP/actions/workflows/mingw.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/mingw.yml)
[![macos-builder](https://github.com/FreeRDP/FreeRDP/actions/workflows/macos.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/macos.yml)
[![[arm,ppc,ricsv] architecture builds](https://github.com/FreeRDP/FreeRDP/actions/workflows/alt-architectures.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/alt-architectures.yml)
[![[freebsd] architecture builds](https://github.com/FreeRDP/FreeRDP/actions/workflows/freebsd.yml/badge.svg)](https://github.com/FreeRDP/FreeRDP/actions/workflows/freebsd.yml)
[![coverity](https://scan.coverity.com/projects/616/badge.svg)](https://scan.coverity.com/projects/freerdp)

## Resources

Project website: https://www.freerdp.com/

Issue tracker: https://github.com/FreeRDP/FreeRDP/issues

Sources: https://github.com/FreeRDP/FreeRDP/

Downloads: https://pub.freerdp.com/releases/

Wiki: https://github.com/FreeRDP/FreeRDP/wiki

API documentation: https://pub.freerdp.com/api/

Security policy: https://github.com/FreeRDP/FreeRDP/security/policy

FAQ: https://github.com/FreeRDP/FreeRDP/wiki/FAQ

### Contact

* Matrix room : `#FreeRDP:matrix.org` (main)
  * IRC channel : `#freerdp @ irc.oftc.net` (bridged)
* Mailing list: https://lists.sourceforge.net/lists/listinfo/freerdp-devel

## Microsoft Open Specifications

Information regarding the Microsoft Open Specifications can be found at:
https://www.microsoft.com/openspecifications/

A list of reference documentation is maintained here:
https://github.com/FreeRDP/FreeRDP/wiki/Reference-Documentation

## Compilation

Instructions on how to get started compiling FreeRDP can be found on the wiki:
https://github.com/FreeRDP/FreeRDP/wiki/Compilation
