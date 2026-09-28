# FreeRDP session follow-ups — checked against current master

Originally audited 2026-09-27; rechecked 2026-09-28 against local `master`
at **`5b259d5fd2bf378a3464ec7552da645f708a53ea`**.

The primary checkout remains on `feat/further-performance-improvements`.
This reassessment inspected the actual master ref and the clean matching checkout
at `/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP`, rather than
mistaking the primary checkout's older files for master.

## What changed since the first audit

**The reviewed fixes are now integrated into master.** Master's complete tracked
tree is identical to `codex/macos-performance-review-fixes`:
`34314719758d8d0928c1d71627f03cb3abf56057`.

The reorganization changed commit identities and grouped concerns behind twelve
merge commits; it did not omit any tracked file from that reviewed result.
The previous statement that master lacked the review fixes is superseded.

**The launcher bridge remains a separate integration.** The prepared
`cleanup/launcher-bridge` branch is based on current master and is six commits
ahead, at `fa6d179b2`. It already includes master's reviewed fixes through its
base, but its six launcher commits are not merged into master. The original
`codex/launcher-bridge` checkout remains separate and has additional uncommitted
Dock-presentation changes.

**The fullscreen confirmation gap is closed.** The later launcher fix
`9248f74` put the confirmation on the fullscreen session's Space, and the
user subsequently confirmed it works. Remove the earlier physical fullscreen
close-confirmation check from the open list.

## Fixed in master

These conclusions are based on source/tree comparison and the existing
regressions, not just commit subjects.

| Earlier issue | Current master status |
| --- | --- |
| UDP ambiguous write retries duplicating reliable-stream bytes | Fixed; failed writes become terminal instead of replaying delivered bytes. |
| Unsafe omitted-zero guesses based on time/chunk count | Fixed; a genuinely delayed zero is preserved. Ambiguous loss still fails safely. |
| Wrap recovery disabled by earlier recovered loss | Fixed; DataSeq-ordered channel bounds recover the six recorded office stalls. |
| Quiet wrap gap failing to wake maintenance | Regression coverage is present; genuine unresolved bytes use watchdog/reconnect recovery. |
| Explicitly empty DVC migration list interpreted as migrate-all | Fixed. |
| YUV cached work causing unnecessary waits after a large frame | Fixed; only submitted work is waited on. |
| Skipped/stalled render metrics failing to flush | Fixed. |
| Reconnect texture destruction after renderer teardown | Fixed. |
| Monitor probing confusing content scale and pixel density | Fixed. |
| Log rotation dropping append mode | Fixed. |
| Continuous redraw starvation, monitor clipping and upload batching | Implemented and preserved, with regression tests. |
| Captured mixed-DPI drag mapping and focus offsets | Fixed and preserved, with input/scale tests and earlier live evidence. |
| Startup fullscreen probing flashes | Code fix is integrated; targeted hardware acceptance remains separate. |
| Review harnesses, office debug launcher and graphics replay | Preserved in master. |

Reference:
[review fixes](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/tools/branch-review-20260926/FIXES.md),
[office capture evidence](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/tools/branch-review-20260926/OFFICE-CAPTURE.md).

The old reports retain historical commit hashes. A rewritten commit's absence
from master's ancestry is not evidence that its fix is missing.

## Still open, or missing from master

### 1. Monitor removal and reconnect shutdown — still open

Session: [Diagnose FreeRDP spinning ball](codex://threads/01a0e46c-d4c8-7d43-b2a9-4b6b1dae8537).

Master still erases the current map element inside the range loop in
[removeDisplayWindow()](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/client/SDL/SDL3/sdl_context.cpp:1053).
That invalidates the iterator. The lifecycle merge's subject does not establish
that this newer display-removal defect was repaired.

The shutdown path sets the abort event and then joins the worker
[sdl_context.cpp](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/client/SDL/SDL3/sdl_context.cpp:120).
The reconnect delay loop in
[client.c](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/client/common/client.c:1466)
does not itself check that event; master's SDL call supplies no callback.
The prepared bridge callback checks launcher cancellation, but still ignores the
shutdown abort event.

The September 27 sample confirmed the actual wait in the bridge build. Its
triggering display event was not proved, and this audit did not reproduce the
hang in a new master session. Safe erasure and prompt reconnect cancellation
remain the concrete changes to investigate.

### 2. Two retry-crash fixes exist on the bridge branch, but are absent from master

- **Monitor selection bookkeeping:** master still writes `NumMonitorIds` from
  automatically detected monitors without maintaining the explicit settings
  array, and lacks the empty-display/array guards. The fix and
  `TestSDLMonitorDetection` are on `cleanup/launcher-bridge`.
  [Master source](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/client/SDL/SDL3/sdl_monitor.cpp:317).
- **Repeated keyboard initialization:** master still asserts that
  `_remapTable` is null every time initialization runs. The bridge fix reuses the
  existing setup; `TestSDLInputInitialization` is also absent from master.
  [Master source](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/client/SDL/SDL3/sdl_input.cpp:686).

These were fixed after the reviewed performance branch was created. The
repeated sign-in path that exposed them belongs to the launcher integration.
Keep them as fixes awaiting integration into master, rather than calling the
original bridge defects unfixed everywhere.

Prepared commits: `f06743cd2` (monitor retries/close handoff) and `fa6d179b2`
(keyboard retries).

### 3. Launcher bridge integration — prepared, still unmerged

Session: [Trace FreeRDP branch relationships](codex://threads/01a0e499-b220-76d3-a9c3-eca0de6cb9e9).

Remaining integration is the six commits on `cleanup/launcher-bridge`:
bridge IPC, optional CMake support, close confirmation/previews, capabilities
query, monitor retry fix and keyboard retry fix. Master has none of the new
bridge files or their four tests.

The launcher source's default executable still points at the original bridge
checkout. This is a source default; it does not establish the user's saved
binary preference or the contents of a running process.

The source consolidation/concern-based maintenance arrangement is implemented.
Deploying and accepting the intended combined build remains separate work.

### 4. Scaling PR — still open with conflicts

Session: [Fix mixed-DPI drag and drop](codex://threads/01a08833-d650-7fd2-ae47-7f04d8624c95).

Rechecked [PR #13431](https://github.com/FreeRDP/FreeRDP/pull/13431) on September 28:
open, head `db4652794`, `mergeable=false`, `mergeable_state=dirty`.
The shared-code follow-up is pushed. Resolve conflicts and refresh validation;
the PR description still identifies the older tested commit `f1e49516d`.

The input fix is present in downstream master. Upstream acceptance is unfinished.

### 5. Historical first-frame and visual incidents — diagnosis remains unfinished

Session: [Diagnose stale connection](codex://threads/01a0ab78-1aef-7dc3-85a0-496d053d35b3).

- **Black startup window/beachball:** the saved
  [connection failure report](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/bugs/connection-failures.md:22)
  remains undiagnosed. No new recurrence was tested here.
- **Video blocks/artifacts:** rendering starvation was repaired; the cause of
  the blocky image itself was not established.
- **Transient Start-menu corruption:** recovered without reconnect; client
  repaint and Windows shell/compositor explanations remain unresolved.

These are historical investigations, not proof that current master still
reproduces every symptom. Capture a thread sample during a startup recurrence;
for visual anomalies compare decoded framebuffer pixels with the displayed
image using a complete capture.
[Video evidence](/Users/david/.codex/worktrees/freerdp-feature-cleanup/FreeRDP/VIDEO-PLAYBACK-FINDINGS-20260918.md),
[Start-menu findings](/Users/david/projects/FreeRDP/diagnostics/start-menu-20260920-210654/FINDINGS.md).

### 6. Live acceptance — narrower than the original list

Still require explicit acceptance on the intended new build:

- Startup with offline host and multimonitor placement, checking for flashes.
- Reconnect overlay on all displays, recovery and retry exhaustion.
- Sleep/wake and mid-session display reconfiguration/input offsets.
- Long-session recovery and end-to-end performance under the normal workload.

Earlier live video observations support the responsiveness fix. The latest
fullscreen confirmation is user-verified. Neither should be reopened simply
because an older report still says “pending.”

### 7. Preservation — partly completed, remaining work already being reviewed

Master contains the office launcher, shared review harness, graphics replay,
log-rotation fix and rendering reports. Remove those from the missing-tooling list.

Unique files still excluded from master include `free-rdp-debug-vm.py`,
the reconnect/repeated-connection integration tools,
`VIDEO-PLAYBACK-VALIDATION-20260918.md`,
`bugs/udp-connect-repeat-20260916.md`, and some compact analysis scripts/results.
The primary checkout also has unique report edits. This is preservation work,
not an unresolved product defect, and is currently being organized in
**Trace FreeRDP branch relationships**.

A launcher path edit must be ported selectively to retain master's newer
append-mode log writer. Long-session replay still needs capture beginnings
preserved before the rolling ring overwrites them.

## Companion launcher findings

FreeRDP master does not contain the Swift launcher repository. Its integration
does not close launcher-only validation or presentation findings.

Current source still shows the Advanced/bridge policy mismatch (F1), Save/build
validation mismatch (F2), managed-channel policy gap (F3), and preset-only scale
import (remaining F4). Existing scale values and Automatic recommendations are
supported; valid nonpreset imports are still restricted.
[Parameter audit](/Users/david/projects/freerdp-launcher/docs/sdl-parameter-ui-audit.md).

F5–F8 and the seven visual findings remain separate audit follow-ups:
host/folder/account consistency, monitor boundary validation, hidden display
state, stale errors, selected-label contrast, disconnect semantics, status
colors, favorites, Logging toggles and the standard-client indicator.
The stale corrected-input error path was spot-checked and remains.
[Visual audit](/Users/david/projects/freerdp-launcher/docs/visual-consistency-audit.md).

Fullscreen close-confirmation placement is now closed by `9248f74` and the user's
confirmation. Wake-on-LAN is implemented in `bd87e59`; it is no longer an active
unfinished implementation. Dock grouping/focus work has uncommitted changes
in both projects and still has interactive validation gaps. Full VoiceOver
and macOS 14 runtime acceptance remain separate.

## Verification performed for this reassessment

- Pinned and rechecked master at `5b259d5fd`.
- Verified whole-tree equality with the reviewed fixes branch.
- Compared the prepared bridge against master, including the two missing
  retry fixes and regression tests.
- Rebuilt the Release SDL3 client and existing selected test targets.
- Reran **eight targeted CTests: 8/8 passed**, covering UDP, YUV, render geometry,
  render windows, metrics, update queue, input mapping and monitor scaling.
  [Fresh test log](/private/tmp/freerdp-session-followups-master-20260928-ctest.log).
- Reviewed recent session closure for the fullscreen confirmation, current
  preservation work and Wake-on-LAN implementation.
- Rechecked GitHub PR metadata and spot-checked relevant launcher source.

No new live RDP session or physical display experiment was run. Passing these
tests does not close the display-removal bug, the omitted bridge-only retry
tests, or historical visual incidents.

## Diagnostic preservation update — 2026-09-28

The reusable diagnostics and written findings listed in section 7 are being
preserved on `cleanup/diagnostic-records`, based on the master revision named
above, for integration through a separate feature merge. This includes the VM
debug launcher, live connection/reconnect tools, playback and connection reports,
offline analysis scripts, and aggregate verification and regression results.

This preservation does not change the reviewed source consolidation, prepared
launcher integration, or remaining acceptance findings described above.
Packet captures, TLS keys, payload streams, packet-level extracts, and raw
session logs remain local. No new live RDP or hardware acceptance checks were
run for this preservation work.

## Recommended remaining order

1. Safe display-window removal and reconnect cancellation during shutdown.
2. Integrate the prepared launcher branch, retaining the two retry-crash fixes.
3. Align launcher Advanced/Save/bridge validation.
4. Resolve scaling PR conflicts and refresh its checks.
5. Complete targeted hardware acceptance and diagnose historical symptoms on
   recurrence; finish the existing preservation workflow.
