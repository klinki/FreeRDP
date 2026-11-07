# Sheltie rebuild on FreeRDP 3.33.0: completed record

Reviewed: **2026-11-07**. This is the historical rebuild audit: source decisions,
implemented adaptations, validation evidence and explicit follow-ups. Feature
contracts remain with their implementation documentation.

## Inputs and integrated result

- **Original fork:** `main` at `b378540f1a9b920b67c3ea109aea6ddf2837fec8`,
  preserved by `v3.32.2-sheltie.1`, based on upstream 3.32.2.
- **New upstream base:** tag `3.33.0`, commit
  `5a10d7b6b79a2fbc62e8eba77085cd356dc303a6`.
- **Integrated result:** the rebuilt patch stack is on fork `main`. The code
  checkpoint reviewed here is `14c3a42e503d666384d303b8edb001ec8d94dc7b`.
- **Hosted validation:** [runtime CI for that checkpoint](https://github.com/klinki/FreeRDP/actions/runs/42230888505)
  passed on 2026-11-06, including build, tests, packaging, smoke checks and
  artifact upload.
- **Release boundary at review:** the published fork release remained
  `v3.32.2-sheltie.1`. The next available release tag was
  `v3.33.0-sheltie.2`; publishing it and updating Sheltie's pin are separate work.

The rebuild replaced the fork's UDP implementation with the reliable transport
from upstream 3.33.0 / [PR #13608](https://github.com/FreeRDP/FreeRDP/pull/13608).
Consumers of UDP state were adapted to upstream `rdpudp`/`rdpemt`. The old patch
stack remains preserved by the release tag and
`archive/master-pre-3.32.2-rebase`.

The initial review branch was `update/sheltie-3.33.0`. Its historical candidates
included `3423c92ca` and `5e867cab5`; they identify binaries used in the November 6
investigation. Later patch-stack reconstruction changed descendant commit IDs.
The implementation references below use the integrated history; original source
references are retained in the inventory. Historical hashes must not be replaced
in diagnostic evidence merely because the current history has different IDs.

## Rebuild method and original patch inventory

The stack was rebuilt feature by feature from tag 3.33.0 instead of replaying
the entire old UDP-dependent history with `scripts/sheltie-rebase.py`. Kept
multi-commit features retained merge boundaries; the documented logs, harnesses
and obsolete UDP code were omitted while each feature was replayed. Build/test
checkpoints accompanied the lifecycle/rendering, launcher/display and monitoring
adaptations, followed by a clean runtime build and packaged smoke tests.

The following 34 rows describe the **original 3.32.2 source stack**. Original
commit IDs and keep/trim/rework/drop decisions are preserved; the last column
records what happened in the rebuild. An original "verify" note is not evidence
that every live scenario passed: see the validation record and follow-ups.

| # | Original step | Original size | Original decision | Rebuild treatment |
|---|---|---|---|---|
| 1 | `89ee1b90b` Document the Sheltie fork and versioning policy | README | **Keep** | Retained the fork policy and updated the supported upstream base to 3.33.0 in `6456d9b5f`. |
| 2 | `05c595785` Fork versioning and upstream rebase helpers | 5 files | **Keep** | Retained version calculation and rebase helpers in `b9851e102`. |
| 3 | `aa91a4406` Count merged patch branches as one version step | 3 files | **Keep** | Retained first-parent version-step counting in `f30d2f5a3`. |
| 4 | `68c3cb294` Merge: transport, reliable RDP UDP transport | 90 files, +62k | **Drop** | Omitted the fork UDP implementation, its server path and transport-specific fixtures (`rdpeudp.c/h`, `udp.c/h`, old routing/Soft-Sync/autodetect changes, `TestRdpeUdp.c`, and bulk UDP review material). Replaced it with the transport in upstream 3.33.0 / PR #13608. |
| 5 | `9c37e46eb` Merge: lifecycle, SDL teardown races and shutdown hangs | 2 files | **Keep** | Retained the SDL teardown and shutdown repairs in `c47f0fd05`; automated lifecycle coverage passed. This does not close unrelated live-session reports. |
| 6 | `f38f066a7` Merge: ui, draggable connection controls (topbar) | 10 files | **Trim** | Retained SDL connection controls in `27d3565d3`; omitted `REVIEW.md` and the workspace-specific `free-rdp-multi-screen.sh` launcher. |
| 7 | `cce2f2afc` Merge: ui, prevent monitor-probe flashes at startup | 4 files | **Keep** | Retained windowless macOS probing in `9f64f1c9a` alongside upstream Wayland/X11 handling. At the rebuild, upstream #13564 had closed unmerged; upstream `d9a14f857` covered Linux only. |
| 8 | `c8ddd476f` Merge: input, coalesce mouse motion | 1 file | **Keep** | Retained motion coalescing without input-event reordering in `fddbcb187`. |
| 9 | `2a82187ae` Merge: multi-mon, scaling and cross-monitor input | 48 files, +26.7k | **Trim** | Retained `sdl_monitor_scale.*`, `sdl_input_mapping.hpp`, SDL changes and tests in `5c942ab52`. Omitted the large display logs and review harnesses. Reconciled upstream physical-size/monitor-order changes (`d74f158e9`, `eb87e5c84`); PR #13431 remains separate upstream work. |
| 10 | `cf6d0bccc` Merge: performance, SDL rendering and input scheduling | 20 files | **Trim** | Retained rendering/input scheduling, render metrics and `tools/render-benchmark` in `fbda7289c`. Omitted `bugs/rapid-pace-freeze.md`, `VIDEO-PLAYBACK-FINDINGS-20260918.md` and the branch-review skip harness. |
| 11 | `6aca32944` Merge: ui, reconnecting overlay and overlay teardown | 10 files | **Keep**, verify | Retained the reconnecting overlay and teardown handling in `df8bcc4b6`; omitted its historical bug note and review harness. Controlled live UDP-loss/overlay validation remains outstanding below. |
| 12 | `508de10a2` Merge: performance, reuse YUV workers, shorter batch waits | 10 files | **Trim** | Retained YUV workers, codec headers and tests in `de1c2efe0`, resolving the 3.33.0 codec changes. Omitted benchmark/review harness material. |
| 13 | `b986f22ca` Merge: tooling, authenticated graphics capture replay | 10 files | **Keep** | The approved `tools/graphics-replay` preservation was initially missed, then completed in `14c3a42e5`. Omitted `run-graphics-replay.sh` and `GRAPHICS-REPLAY-20260920.md`; the restored README documents legacy capture limits. |
| 14 | `8ec965914` Merge: tooling, macOS build and diagnostic launchers | 11 files | **Drop** | Omitted old `build.sh`, `free-rdp-*.sh/.py`, `run-with-counters*.sh`, `freeze-snapshot.sh` and `free-rdp-vm-loop.sh`. Runtime build/package tooling is under `scripts/sheltie`; retained render-benchmark documentation remains with its tool. |
| 15 | `0aaca3386` Merge: review, regression harnesses and diagnostic reports | 10 files | **Drop** | Omitted bulk branch-review harnesses, `COMMITS-FEATURES.md`, rendering investigations and old connection-failure notes from the rebuilt stack. The old release preserves them. |
| 16 | `a4039dd57` Merge: diagnostic tools and investigation records | 36 files | **Drop** | Omitted bulk diagnostic logs, VM launchers and investigations from the current stack. Their archived history remains available; omission does not classify unresolved reports as fixed. |
| 17 | `b343f2ef9` Merge: macOS launcher bridge for SDL3 | 17 files | **Keep** | Retained the native macOS launcher bridge in `a3404a7ca`. |
| 18 | `9012c708c` Fix session hangs after display removal | 7 files | **Keep** | Retained display-removal hang handling in `ddd99656a`. |
| 19 | `3d6b01df4` Honor the first selected monitor as primary | 6 files | **Keep** | Retained the selected-primary-monitor implementation in `5f51bdaa0`, including reconnect subsets, instead of reducing it to upstream's simpler initial-order behavior. |
| 20 | `23af86df4` Optional launcher TLS key logging | 7 files | **Keep**, verify | Retained opt-in launcher TLS logging in `292bcd71a`. Live inspection exposed the missing UDP SSL callback; `20901bbac` completed tunnel logging and added `TestTlsSecretsFile`. |
| 21 | `058afbb4e` Per-session mouse wheel reversal | 10 files | **Keep** | Retained per-session wheel reversal in `a77233eff`. |
| 22 | `b3af78a65` Merge transport recovery fix | 6 files | **Keep**, verify | Retained SDL transport recovery in `7328581c4`, including upstream session-loop behavior and `errorToExitCode` after failed recovery. `TestSDLReconnect` passed; a dedicated Windows UDP-loss matrix remains outstanding. |
| 23 | `f296d4459` Report actual fullscreen desktop dimensions | 4 files | **Keep** | Retained actual fullscreen desktop dimensions in `9bcb61b65`. |
| 24 | `b4b4f8b9c` Refresh display layout after monitor reconnection | 9 files | **Keep** | Retained monitor reconnection scheduling in `adc1b87e5`; `e4d0a7565` additionally applies the upstream display-capability resize delay and updates its regression test. |
| 25 | `c8211568a` Restore selected monitors and scaling after reconnection | 9 files | **Keep** | Retained selected monitors and scaling restoration in `32bb1d9c9`. |
| 26 | `9f7f892e4` Merge: session performance monitoring | 29 files | **Rework** | Retained monitoring in `9b9170f9a` and adapted its hooks to upstream `rdpudp`/`rdpemt` and autodetect. Datagram, byte, retransmission, loss-report and tunnel-state hooks were carried over. `bbf074d93` removed the retired transport fixture; direct loss-report counting still lacks automated coverage. |
| 27 | `c2d0d3c7c` Update merge instructions (`AGENTS.md`) | 1 file | **Keep** | Retained merge/integration instructions in `4c2100968`. |
| 28 | `60eb45d02` Fix MacBook notch sizing after monitor disconnection | 3 files | **Keep** | Retained the MacBook fallback sizing repair in `c147efcca`; physical transition evidence for the new base remains separate from unit tests. |
| 29 | `da8b8a869` Window geometry reporting and restoration | 9 files | **Keep** | Retained geometry reporting/restoration in `9d821e1a8`. |
| 30 | `658a9b4a3` Remove raw UDP review logs | −69k | **Drop** | No separate cleanup replay was needed because the raw UDP review logs were not imported into the rebuilt stack. |
| 31 | `2b784495d` Describe the 3.32.2 base and disabled upstream workflows | README | **Keep** | Retained fork documentation and updated the base/UDP explanation in `6456d9b5f`. Inherited workflows remain in the tree; their GitHub enablement is managed separately. |
| 32 | `1d955ae55` Link the launcher bridge into TestSDLRenderWindow | 1 file | **Keep** | Retained the launcher-bridge link for `TestSDLRenderWindow` in `1d00cf1f3`. |
| 33 | `2e82785d8` Number fork releases with a global counter | 3 files | **Keep** | Retained the fork-wide release counter in `98b522831`; changing upstream base does not restart the counter. |
| 34 | `b378540f1` Merge: Sheltie runtime build, test and publish | 17 files | **Trim** | Retained runtime build, test, source packaging and publication in `b7faf87d4`. Omitted committed Python cache files; `0117830cd` enabled upstream UDP in runtime builds. |

## Additional carried work and adaptations

- **Screen refresh:** `b197241dd` carried manual full-desktop refresh and hover
  recovery methods from the old screen-refresh work. `2c9095e27` subsequently
  made combined recovery the top-bar default and gated the advanced menu with
  `FREERDP_SDL_REFRESH_MENU`. The bridge default for requests without a mode
  remains server redraw; see [the bridge contract](../../client/SDL/SDL3/launcher-bridge.md).
- **Runtime decoding/layout:** `70e5aa37a` enabled FFmpeg hardware acceleration;
  `505f4761e` suppressed unchanged layouts after work-area notifications.
- **Parser hardening:** `8517e64fe` retained the engine portion of the Microsoft
  protocol-test work (`054b42e81`) and its native regression, leaving the large
  external harness/evidence in `archive/windows-protocol-tests`. It was submitted
  as [upstream PR #14079](https://github.com/FreeRDP/FreeRDP/pull/14079), open at
  review. The fork continues carrying it until an adopted upstream base includes
  the repair.
- **Primary monitor:** the selected-primary implementation covers reconnect
  subsets and removed displays. It was retained alongside upstream's initial
  monitor ordering, not replaced with that narrower behavior.
- **Display capability timing:** the scheduler retained upstream's resize delay
  after DisplayControlCaps. The delayed send is covered by
  `TestSDLFullscreenResize`.
- **Recovery exit codes:** failed in-session recovery follows upstream
  `errorToExitCode` behavior. The old generic `CONN_FAILED` return was not restored.
- **UDP build policy:** the runtime explicitly sets `WITH_RDPEUDP=ON`; upstream
  3.33.0's build option defaults OFF and is experimental. The legacy launcher
  `-multitransport` option still disables multitransport in this base.
- **Tunnel TLS keys:** `20901bbac` connects the separate UDP SSL session to the
  existing opt-in `TlsSecretsFile` callback. The original launcher-only logging
  port did not log UDP tunnel secrets. This repaired diagnostics, not the cause
  of the separately reported session disconnects.
- **SDL compatibility:** `1ee1bad7f` corrected the inherited event-name guard from
  SDL 3.4.20 to 3.6.0. Local SDL 3.4.16 had skipped the block; hosted SDL 3.4.20
  exposed the missing event identifiers. The original failure is retained in
  [CI run 42207104037](https://github.com/klinki/FreeRDP/actions/runs/42207104037).
- **Offline replay:** `14c3a42e5` restored the eight approved tool files that the
  initial rebuild missed. Its production-code runner works with the rebuilt
  client; [the extractor's README](../../tools/graphics-replay/README.md) records
  its legacy UDP-created-channel restriction. TCP-created channels migrated by
  Soft-Sync remain unsupported by that extractor.

## Branch and archive disposition

The following archive tag objects were verified published in the fork on
2026-11-07. They preserve the original work independently of moving branch names.

| Preserved work | Published archive tag(s) | Disposition at review |
|---|---|---|
| Original pre-3.32.2 fork stack | `archive/master-pre-3.32.2-rebase` | Preserved alongside the immutable 3.32.2 release tag. |
| Six-branch fork UDP/benchmark chain | `archive/udp-bench-2026-10` | Its six obsolete local and remote branches were retired. |
| Three upstream-UDP experiment branches | `archive/pr13608-experiments` | Their three local/remote branches were retired; a complete local snapshot preserves the experiment worktree and its artifacts. |
| Microsoft protocol harness and evidence | `archive/windows-protocol-tests` | The old local/remote branch and worktree were retired after full local preservation of prepared suites, results and build outputs. |
| Pre-main screen-refresh branch | `archive/screen-refresh-pre-main` | Original branch tip preserved; its obsolete local/remote branch and duplicate worktree were retired. |
| Later 3.32.2 diagnostic candidate | `archive/screen-refresh-3.32.2-candidate` | Preserves exact tip `c37d523b5`; the obsolete local/remote branch and duplicate worktree were retired. |
| Initial 3.33.0 diagnostic candidate | `archive/rebuild-3.33.0-initial` | Preserves exact tip `5e867cab5`; `update/sheltie-3.33.0` was retired locally and remotely after the worktree was backed up in full. |
| Older pre-squash feature histories | `archive/old-codex-launcher-bridge`, `archive/old-feat-further-performance-improvements`, `archive/old-feat-performance-macos-native` | Preserved. These superseded remote branch names were already absent at cleanup. |

Thirteen fully archived local/remote development branches were retired in total:
the original ten, followed by both screen-refresh branches and the initial
3.33.0 rebuild branch. The stale local upstream `master` branch was also removed;
its tip remained reachable from upstream history. The fork's remote `master`
was retained.

The two screen-refresh working-tree patches had stable patch IDs matching the
already integrated `2c9095e27`. Both uncommitted patches were preserved anyway,
together with every other file in the retired worktrees. The exact historical
candidate tips `c37d523b5` and `5e867cab5`, previously lacking containing tags,
were tagged and verified published before their branches were removed.

### Local artifact preservation

On 2026-11-07, six obsolete checkouts were retired: the upstream-UDP flight
benchmark, initial 3.33.0 rebuild, notch fallback, Microsoft protocol tests and
both screen-refresh worktrees. Complete private local snapshots preserved all
files, including ignored caches, raw test results, build outputs and uncommitted
changes. SHA-256 verification covered every regular file before retirement;
manifests, original Git state and restoration instructions accompany the copies.
This consolidated workspace paths without discarding those artifacts or claiming
to reclaim their disk space.

An offline FreeRDP Git bundle was independently cloned and integrity-checked,
including the published archive tags and both diagnostic candidates. The primary
checkout, parser-hardening PR worktree, launcher worktree and running Sheltie app
were retained. Captures and TLS key logs in Sheltie's log directory were retained.

The published `archive/windows-protocol-tests` tag already contains the Microsoft
test harness, configuration, 11 driver patches and historical qualification and
investigation evidence. It does not contain the complete Microsoft source or
every ignored local result. The complete retired-worktree snapshot additionally
preserves the prepared suite, four older prepared suites, raw results and build
outputs. An independent offline backup of official Microsoft source revision
`29ddb4238a5443b7499e82934c90cc898861c5e7` passed `git fsck --full`; all 11 driver
patches were checked and applied to a fresh export of its required source paths.
These were preservation checks, not a rebuild or rerun of the Microsoft suite.

### Retained upstream submissions

The upstream submission branches are separate ongoing work:

- `fix/packet-length-validation`: [PR #14079](https://github.com/FreeRDP/FreeRDP/pull/14079),
  based on upstream master, not the fork stack.
- `feat/multimon-layout-2`: [PR #13431](https://github.com/FreeRDP/FreeRDP/pull/13431).

Both PRs were open at review. Their submission is complete; upstream acceptance
and later adoption are not implied.

### Hosted workflow and tag cleanup

The newly inherited `emscripten-builder` workflow was disabled in the fork's
GitHub settings, verified on 2026-11-07. Its YAML remains in the source tree so
upstream updates do not require deleting/re-adding workflow files.

An accidental all-tags push was cleaned up by deleting 115 tag refs whose names
and objects matched upstream. All 11 fork release/archive/backup tags and all
branch refs were preserved during that tag cleanup. The seven newly created
archive tags are now published. Upstream tags remain available locally; the
primary clone's tested pre-push hook rejects publishing them to `origin`.
That hook and the namespaced upstream-tag fetch configuration are local clone
settings, not a server-side rule or a guarantee for other clones.

## Validation record

These are recorded results, not a claim that this documentation update reran
engine or hardware tests. Evidence types and limitations remain explicit.

| Evidence | Date and source | Established result and scope |
|---|---|---|
| Hosted runtime validation | 2026-11-06, [run 42230888505](https://github.com/klinki/FreeRDP/actions/runs/42230888505) at `14c3a42e5` | Version/manifest helpers, runtime compilation, SDL/YUV tests, relocatable packaging, bridge/loopback/reverse-wheel smoke tests and artifact upload passed. The sample-server fixture is TCP; this does not establish Windows UDP loss recovery or physical-display behavior. |
| Local runtime checkpoint | 2026-11-06, recorded build/package results for the earlier pre-reconstruction checkpoint `d24efe44e` | All 17 SDL tests and both YUV tests passed. Client/sample-server bundles verified; bridge checks, release loopback sessions and reverse-wheel delivery passed. Historical revision retained for provenance. |
| Fork native parser/transport checks | 2026-11-06, validation recorded with `8517e64fe` | `TestRdpPacketLengths`, `TestMcs`, `TestDisconnect` and `TestRdpUdp` passed in an internal-testing build. |
| UDP TLS logging | 2026-11-06, `20901bbac` / original candidate `5e867cab5`, local crypto CTest and live diagnostic capture | `TestTlsSecretsFile` verified independent SSL contexts sharing a key file and an opt-out session. A live new-engine capture had both TCP and UDP keys; reconstructed tunnel records authenticated. This is diagnostic validation, not a disconnect repair. |
| Offline graphics replay | 2026-11-06, restoration validation recorded in `14c3a42e5` | Eight extractor tests and four malformed-input tests passed. Existing capture extraction yielded 4,181 messages; a 100-message production replay passed 84 pixel checks with no decode errors. This covers the documented legacy capture path and offline rendering. |
| Windows desktop use | 2026-11-06, user confirmations during the Sheltie disconnect investigation, separately supported by log/capture inspection | The user reported that the old control held and the original new engine later continued working. New-engine bidirectional UDP graphics was observed. Later complete logs also showed a generic network interruption/reconnect; do not describe the entire control interval as uninterrupted, or close the original disconnect report from these observations. Detailed incident evidence remains in Sheltie's private tracker. |
| Upstream parser submission | 2026-11-07, `53585f098` based on upstream master `8a312188d`, prepared for [PR #14079](https://github.com/FreeRDP/FreeRDP/pull/14079) | All 11 core CTests passed on macOS arm64 Debug/OpenSSL. The 84-scenario packet test failed against original sources and with either check removed independently; upstream formatting passed. The full Microsoft suite was not rerun on that fresh upstream base. |

The October 15 Microsoft-suite qualification belongs to the preserved old
protocol-test history. It is not a Microsoft-suite qualification of the 3.33
rebuild or current upstream master. Local and hosted regression passes likewise
do not substitute for an unrecorded physical-hardware scenario.

## Remaining follow-ups

The rebuild is integrated and the hosted runtime pipeline passes. The following
items remain validation gaps or unfinished extensions; they
are not all confirmed defects and this list does not authorize implementation.

- Record controlled Windows UDP network-loss/reconnect behavior, reconnecting
  overlay behavior, recovery exhaustion and performance-counter behavior.
- Record physical mixed-DPI monitor unplug/replug/reordering, selected-primary
  fallback, scaling restoration, notch and fullscreen transitions on the new
  engine base. Earlier confirmations on an older base remain historical evidence.
- Add regression coverage for direct UDP loss-report counting. `bbf074d93`
  removed the old transport-only fixture; counter aggregation coverage remains,
  but that does not exercise the new `rdpudp.c` loss-report hook itself.
- Extend graphics extraction to follow a graphics channel created on TCP and
  moved onto UDP by Soft-Sync, if that capture path is needed.
- Keep the separately tracked live disconnect investigation open until its cause
  or repair is validated against the reported scenario. Passing TLS-key tests,
  ordinary later connectivity and green runtime CI are insufficient to close it.

## Release handoff

At this review, `sheltie-version.py --next-tag --upstream-ref 3.33.0` returned
`v3.33.0-sheltie.2`. The fork-wide counter continues across upstream versions;
`.1` is already used by the 3.32.2 release. Recompute the next tag when preparing
publication rather than treating this dated result as a permanent reservation.

Use the [runtime release procedure](../../scripts/sheltie/README.md) for a
reviewed release commit and immutable tag. A passing main-branch development run
is not itself a published engine release. Sheltie still pinned
`v3.32.2-sheltie.1` at review; update its release lock from the published runtime
manifest and checksums as a separate consumer change.
