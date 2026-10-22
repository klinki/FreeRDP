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
upstream FreeRDP release plus the current Sheltie patch stack. As an initial
exception, its upstream base is a snapshot of upstream `master` rather than a
release tag. Future upstream updates normally target published FreeRDP releases.

The downstream patches are rebased onto each new upstream base, reviewed and
tested before publication. Consequently, `main` may be rebased and force-pushed;
its commit history is not a stable release reference. Contributors should fetch
and rebase their work onto the updated `main` after an upstream update.

Stable fork releases use immutable tags such as `v3.32.0-sheltie.1`. The upstream
version identifies the FreeRDP base; the `sheltie.N` suffix identifies the fork
release on that base. Additional fork releases increment `N`; moving to a new
upstream release starts again at `1`. Published release tags are never moved.
Sheltie release builds pin an exact engine tag or commit rather than following
the moving `main` branch.

Feature and fix branches are temporary development branches and may be rebased
before integration. Maintenance branches are created from release tags only when
an older release needs a fix; a permanent branch for every release is not required.

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
