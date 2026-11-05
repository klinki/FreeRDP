# Sheltie rebuild on FreeRDP 3.33.0: patch inventory

Source: `main` at `b378540f1` (tag `v3.32.2-sheltie.1`, based on upstream
`3.32.2`). Target: branch `update/sheltie-3.33.0`, created from upstream tag
`3.33.0`.

3.33.0 includes upstream PR #13608 (client-side UDP multitransport, merged
2026-10-20). Our own RDP-UDP implementation is therefore **not** carried over.
Features that read our UDP state are reworked to use upstream's
`rdpudp`/`rdpemt` instead.

Everything removed here remains reachable through `v3.32.2-sheltie.1` and
`archive/master-pre-3.32.2-rebase`. Unmerged branches get their own
`archive/*` tags before deletion (see the end of this document).

## Method

Rebuild instead of running `scripts/sheltie-rebase.py`: re-apply each kept
feature as its own `--no-ff` merge, in the original order. Inside every merge,
drop the parts listed as dropped. Then build, run the tests and do a quick
launcher check before moving to the next step. Fork tooling and documentation
come first, so that the version helpers work during the rebuild.

Decisions: **Keep**: re-apply as is (conflicts expected only from upstream
churn). **Trim**: keep the code but drop the investigation logs and docs
committed with it. **Rework**: keep the feature but adapt it to upstream
code. **Drop**: do not carry over. **Decide**: needs an owner decision.

## First-parent steps on `main`

| # | Step | Size | Decision | Notes |
|---|---|---|---|---|
| 1 | `89ee1b90b` Document the Sheltie fork and versioning policy | README | **Keep** | Update the base description to 3.33.0 (merge with step 30). |
| 2 | `05c595785` Fork versioning and upstream rebase helpers | 5 files | **Keep** | |
| 3 | `aa91a4406` Count merged patch branches as one version step | 3 files | **Keep** | |
| 4 | `68c3cb294` Merge: transport, reliable RDP UDP transport | 90 files, +62k | **Drop** | `rdpeudp.c/h`, `udp.c/h`, multitransport, Soft-Sync, `drdynvc` and `channels.c` routing, UDP autodetect, server accept path, `TestRdpeUdp.c`, `REVIEW.md`, `WINDOWS_ANALYZED.md`, office findings, `tools/udp-review-tests`, the UDP files in `tools/branch-review-20260926`. Replaced by upstream #13608. |
| 5 | `9c37e46eb` Merge: lifecycle, SDL teardown races and shutdown hangs | 2 files | **Keep** | Check whether 3.33.0 fixed the same races. |
| 6 | `f38f066a7` Merge: ui, draggable connection controls (topbar) | 10 files | **Trim** | Keep `client/SDL/SDL3`. Drop `REVIEW.md` and `free-rdp-multi-screen.sh`. |
| 7 | `cce2f2afc` Merge: ui, prevent monitor-probe flashes at startup | 4 files | **Keep** | Upstream `d9a14f857` removed the dummy probe window on Linux only; keep ours for macOS. Upstream #13564 (macOS without a probe window) was closed unmerged. |
| 8 | `c8ddd476f` Merge: input, coalesce mouse motion | 1 file | **Keep** | |
| 9 | `2a82187ae` Merge: multi-mon, scaling and cross-monitor input | 48 files, +26.7k | **Trim** | Keep `sdl_monitor_scale.*`, `sdl_input_mapping.hpp`, the SDL changes and both tests. Drop `ai/MULTIMONITOR.md`, `bugs/drag-*.md`, `record-screens.sh`, `tools/udp-review-tests/logs/drag-20260916`, `tools/branch-review-20260926/probe_case.cpp`. Upstream PR #13431 (per-monitor scaling) is still open; drop ours if it merges. Interacts with upstream `d74f158e9` (physical size in mm) and `eb87e5c84` (monitor order). |
| 10 | `cf6d0bccc` Merge: performance, SDL rendering and input scheduling | 20 files | **Trim** | Keep the SDL code, render metrics and `tools/render-benchmark`. Drop `bugs/rapid-pace-freeze.md`, `VIDEO-PLAYBACK-FINDINGS-20260918.md` and `tools/branch-review-20260926/skip_case.cpp`. |
| 11 | `6aca32944` Merge: ui, reconnecting overlay and overlay teardown | 10 files | **Keep**, verify | SDL-level, not tied to our UDP code. Check that it still fires when upstream ends a stalled tunnel (16 s ordered-gap limit, then reconnect). Drop `bugs/connection-stall-feedback.md` and `overlay_case.cpp`. |
| 12 | `508de10a2` Merge: performance, reuse YUV workers, shorter batch waits | 10 files | **Trim** | Keep `yuv.c`, codec headers and tests. Upstream changed `yuv.c` twice in 3.33.0; expect conflicts. Good upstream PR candidate. Drop the `branch-review` YUV harnesses. |
| 13 | `b986f22ca` Merge: tooling, authenticated graphics capture replay | 10 files | **Keep** | `tools/graphics-replay` replays captured graphics through production SDL rendering, for rendering benchmarks. The extractor parses captures of our UDP layout; adapt it to upstream framing when it is next used. Drop `run-graphics-replay.sh` and `GRAPHICS-REPLAY-20260920.md`. |
| 14 | `8ec965914` Merge: tooling, macOS build and diagnostic launchers | 11 files | **Drop** | `build.sh`, `free-rdp-*.sh/.py`, `run-with-counters*.sh`, `freeze-snapshot.sh`, `free-rdp-vm-loop.sh`. Replaced by `scripts/sheltie/build-engine.sh`; nothing in `scripts/sheltie` or CI references them. Move `tools/render-benchmark/README.md` changes into step 10 if still relevant. |
| 15 | `0aaca3386` Merge: review, regression harnesses and diagnostic reports | 10 files | **Drop** | `tools/branch-review-20260926`, `COMMITS-FEATURES.md` (replaced by this inventory), render validation reports, `bugs/connection-failures.md`. |
| 16 | `a4039dd57` Merge: diagnostic tools and investigation records | 36 files | **Drop** | UDP logs, integration rigs, VM capture launchers and `bugs/*` notes. Copy any still-open bug into the Sheltie repo under `docs/bugs/current/` first. |
| 17 | `b343f2ef9` Merge: macOS launcher bridge for SDL3 | 17 files | **Keep** | Core of the fork. |
| 18 | `9012c708c` Fix session hangs after display removal | 7 files | **Keep** | |
| 19 | `3d6b01df4` Honor the first selected monitor as primary | 6 files | **Keep** | Check against upstream `eb87e5c84` (monitor order with `/monitors:`). |
| 20 | `23af86df4` Optional launcher TLS key logging | 7 files | **Keep**, verify | Check that the upstream UDP tunnel (`rdpemt`) also writes its TLS secrets. |
| 21 | `058afbb4e` Per-session mouse wheel reversal | 10 files | **Keep** | |
| 22 | `b3af78a65` Merge transport recovery fix | 6 files | **Keep**, verify | SDL session-loop recovery, not our UDP code. Re-run `TestSDLReconnect` and check reconnects after upstream tunnel loss (`be112202b`, `e0a5bc436`). |
| 23 | `f296d4459` Report actual fullscreen desktop dimensions | 4 files | **Keep** | Interacts with upstream `0be2fd799` (menu bar in fullscreen on macOS) and `eec280288` (resize delay after DisplayControlCaps). |
| 24 | `b4b4f8b9c` Refresh display layout after monitor reconnection | 9 files | **Keep** | |
| 25 | `c8211568a` Restore selected monitors and scaling after reconnection | 9 files | **Keep** | |
| 26 | `9f7f892e4` Merge: session performance monitoring | 29 files | **Rework** | Remove the `rdpeudp.c/h` hooks. Feed UDP datagram, retransmission and loss counters from upstream `rdpudp`, and the bandwidth estimate from upstream autodetect (`autodetect.c` changed in 3.33.0). Keep TCP, rendering, FPS and overlay parts. Sheltie's `udpReliability` fields follow whatever upstream exposes; launcher changes go in a separate Sheltie PR. |
| 27 | `c2d0d3c7c` Update merge instructions (`AGENTS.md`) | 1 file | **Keep** | |
| 28 | `60eb45d02` Fix MacBook notch sizing after monitor disconnection | 3 files | **Keep** | |
| 29 | `da8b8a869` Window geometry reporting and restoration | 9 files | **Keep** | |
| 30 | `658a9b4a3` Remove raw UDP review logs | −69k | **Drop** | Unnecessary once steps 4, 9 and 16 are trimmed. |
| 31 | `2b784495d` Describe the 3.32.2 base and disabled upstream workflows | README | **Keep** | Update to 3.33.0. |
| 32 | `1d955ae55` Link the launcher bridge into TestSDLRenderWindow | 1 file | **Keep** | |
| 33 | `2e82785d8` Number fork releases with a global counter | 3 files | **Keep** | |
| 34 | `b378540f1` Merge: Sheltie runtime build, test and publish | 17 files | **Trim** | Drop the committed `scripts/sheltie/__pycache__` and add it to `.gitignore`. Update the workflow's base references to 3.33.0. |

## Unmerged branches

| Branch | Decision | Notes |
|---|---|---|
| `codex/screen-refresh-based-on-main` | **Keep**, rework one commit | Carry over manual screen refresh (`bb49bfe34`), hover refresh methods (`734cee794`), FFmpeg hardware acceleration in runtime builds (`5eb7b6a0f`) and unchanged-layout suppression (`9d64e995f`). `c37d523b5` (UDP bandwidth diagnostics) targets our autodetect/UDP code; fold it into the step 26 rework. Then delete `codex/screen-refresh`. |
| `codex/windows-protocol-smoke` | **Partly keep** | 7 commits: Microsoft RDP protocol test harness (`tools/windows-protocol-tests`, ~54k lines), its investigation evidence (~170k lines) and one engine fix. Carry only the engine part of `054b42e81` (MCS userData and Demand Active length validation in `rdp.c`/`connection.c`, plus `TestRdpPacketLengths.c`) as its own merge, and offer it upstream. Tag the branch `archive/windows-protocol-tests`; the harness can return later without its evidence folders. |
| `feat/multimon-layout-2` | **Keep as upstream PR branch** | Source of upstream PR #13431; the same feature ships in step 9. Rebase the PR if upstream asks. |
| `perf-bench` … `codex/pr13608-reliability` (6 branches) | **Drop** | Fixes to our UDP code plus the benchmark suite. Tag the tip `archive/udp-bench-2026-10` first. `tools/rdp-bench` can come back later (without its result files) to benchmark the upstream UDP code. |
| `pr-13608-flight-reference` … `codex/pr13608-protocol-loss-recovery` (3 branches) | **Drop** | Experiments on the PR's code. Tag the tip `archive/pr13608-experiments`. `49fcbb72c` (16 s ordered-gap limit) can be offered upstream as a discussion. |
| `origin/feat/further-performance-improvements`, `origin/feat/performance-macos-native`, `origin/codex/launcher-bridge` | **Drop** | Pre-squash versions of work already in `main`. Tag as `archive/old-feat-*`. |

## Checks after the rebuild

- The launcher's `-multitransport` still forces TCP only (3.33.0 parses it as
  `SupportMultitransport = false`). The default is now UDP on.
- UDP works with the office host and the Windows VM: tunnel up, graphics over
  UDP, reconnect after tunnel loss, the reconnecting overlay, TLS key logging
  of the tunnel.
- Performance overlay and launcher statistics show sensible UDP values.
- Multi-monitor, mixed DPI, notch, fullscreen and monitor reconnection on the
  MacBook + M27UP + Dell setup.
- A few days of real use before tagging `v3.33.0-sheltie.1`.
