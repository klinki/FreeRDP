# Cross-monitor drag: cursor↔window divergence (multimon input mapping)

Status: open, under active diagnosis (2026-09-11). Mapping flips FIXED
(`fabad8d5e`); residual cursor↔window detachment persists, now tracked as
present-lag / stale-replay, not mapping. Do not confuse
with `bugs/rapid-pace-freeze.md` (render saturation) — different symptom,
different layer, though both bite during drags.

## Symptom

Multimon session (M27UP 4K@200% + Dell 1080p@100%, `/multimon`), single-screen
drags fine. Dragging across the monitor boundary (either direction): the
dragged window detaches from the cursor by a large offset (user reports ~500px
class gaps; one capture shows cursor ~500px from the grabbed window mid-drag
with the button held). Keyboard unaffected (no coordinates involved).

## Proven facts (measured, not inferred)

- Input pipeline is healthy: 4,337 fastpath Mouse-Move PDUs on the wire match
  4,357 arrival log lines ~1:1; keyboard/sync events present; zero transport
  loss; ACKs track through 16-bit DataSeq wraps live (3x).
- Negotiated layout is stable across sessions: primary 0..3839x0..2159,
  secondary -1920..-1 x 1080..2159 (gcc log, multiple sessions).
- Click test (2026-09-10, VM + davidpc): all clicks landed where aimed on both
  screens with the stock offset code. No systematic constant offset exists.
- Focus-synthesized moves skipped the monitor offset: every keyboard
  focus-in fired a synthetic move at the current pointer
  (`keyboard_focus_in` -> `screenToPixel`, no offset step), while real
  motion went through offset mapping. During boundary crossings focus
  flapped Dell<->M27UP, injecting unoffset positions between
  correctly-offset ones — the exact ±1920/±1080 same-millisecond flips.
  Fixed via single `screenToRdp()` (pixel conversion + offset) on the
  focus path; committed as `fabad8d5e` (13 insertions).
- Post-fix wire (2026-09-11, 2,430 motion events): zero out-of-range
  teleports, zero trace fires. Remaining candidates benign: `t=1.7`
  `(0,0)->(3904,1773)` session-startup first-focus warp; `t=42.6`
  `(3276,1081)->(4632,3)` fast fling, both endpoints valid normalized
  M27UP positions (M27UP spans 1920–5759). Prior sessions showed ~13
  same-instant flips with off-screen values — gone after fix.
- User feel after fix: better but not done — window still detaches from
  cursor at the boundary (large gap persists).
- Signed 16-bit delta reanalysis (2026-09-11, 13,421 events,
  `20260911-064816-94958`): max real displacement 830px, only 3 events
  exceed 500px. The scary 65,504px jumps were small edge-crossing moves
  (e.g. +107px) wrapping past zero. There was never mapping corruption
  on the wire. What is real (3 events): cross-window stale replays —
  positions from different windows/times surfacing adjacent after a
  stall. First coalescer only merged same-window runs, so these sailed
  through.
- Live jump trace (temporary instrumentation, `motion jump` WARN lines):
  teleports carry coordinates EXCEEDING their own window
  (x=5056 on a 3840-wide window; y=1766 on a 1080-tall window), real mouse
  (`which=0`), zero warp activity. Windows themselves are correctly sized.
  Offsets verified correct (`off=(-1920,0)` / `(0,-1080)`),
  `winsize=` matches displays, `raw=1019 -> conv=2038` proves exact x2
  conversion on M27UP — conversion stage correct.
- Display IDs renumber between sessions (built-in back as Main reshuffles
  everything); scale overrides keyed by SDL ID can silently target the wrong
  monitor (mitigated: override application now logged at INFO with names).

## Ruled out

- Constant origin-normalization error in `applyMonitorOffset`: verified the
  math both ways; a committed "fix" (raw negative coords) made clicks land a
  screen away and was reverted the same night. The stock formula matches the
  click-test ground truth. Wire layout (-1920) vs server working space
  (normalized) was a red herring — clicks prove the stock mapping lands.
- Double coordinate conversion (x4 total): retracted 2026-09-11. Signed
  16-bit delta recompute shows no x4 values on the wire; `/4 fits points`
  was unsigned-wrap coincidence. `raw->conv` x2 verified exact.
- Mapping corruption on the wire: retracted — signed recompute proves all
  large jumps were wrap artifacts; only 3 real stale-replay events remain.
- Network/transport/wrap/server stall: captures show perfect sync throughout.
- Missing input packets: retracted — early verdict was a measurement artifact
  (UDP-only captures hide all TCP input).
- Touch-emulated events: `which=0` (real mouse) on all teleports.
- Warp echoes: zero `moveMouseTo` warps logged across sessions.
- Oversized/overlapping windows: `winsize=` in trace matches displays exactly.

## Open hypotheses (ranked)

1. **Cross-window stale replay after main-thread stall (3 events proven).**
   Motion backlog replays positions from different windows/times adjacent.
   Fix in worktree: global coalescing (latest position wins across
   windows; button press/release breaks run; relative deltas summed).
   Awaiting one re-drag test.
2. **Present lag, not mapping.** Wire is clean (correct values, no
   teleports); window position on screen trails seconds behind under
   drag burst because the main thread cannot present at drag rate
   (same root as rapid-pace freeze: render-upload bursts + event-backlog
   replay as one mechanism). `t=42.6` jump-cut was coalescing working,
   not corruption.
3. **Event storm at crossing** (`Cocoa_SyncWindow` 79% sample):
   display-change events firing as pointer crosses stall the main thread
   exactly when smoothness matters. Untouched.
4. **Stale events from a previous topology/mode.** Teleports cluster around
   drags and mode switches; a backlog of pre-switch events delivered late
   would carry then-valid, now-foreign coords. Fits the maximize-incident
   shape. Check: event timestamps vs switch time.
5. **Two event sources for one pointer** (e.g. relative+absolute both firing):
   would produce paired contradictory values. No second source identified yet.

## Tried

- Offset separation patch (input vs draw offsets): made it worse, reverted.
- Focus-offset fix (`screenToRdp`): fixed flips, committed `fabad8d5e`.
- Motion-event coalescing v1 (same-window runs): live in binary, collapsed
  fling intermediates to endpoints as designed; missed cross-window stale
  events. Superseded by v2.
- Motion-event coalescing v2 (global across windows, uncommitted in
  `client/SDL/SDL3/sdl_freerdp.cpp`): latest wins across windows, button
  breaks run, rel deltas summed. Built, awaiting drag test.
- Single-process dual screen recording (two AVFoundation opens race; loser
  wedges silently): rewritten around one ffmpeg + HW encode + readiness gate.
- Cursor-in-video: `-capture_cursor 1` works; cursor absent in most drag
  frames (was on the other display).
- Frame-by-frame cursor tracking: defeated by white UI elements; motion
  differencing found the drag windows instead.

## Next step (in progress)

Restart with global-coalescing build, one Dell->4K drag, share `client.log`.
Wire should show zero same-instant jumps of any size, signed or not.
If clean but feel still detaches, mapping file closes and remaining work
moves to frame pacing under burst. Open follow-ups (not started):
mid-session reconfiguration staleness (nap/wake offset refresh),
frame pacing under burst.
