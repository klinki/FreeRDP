# Cross-monitor drag: cursor↔window divergence (multimon input mapping)

Status: open, under active diagnosis (2026-09-11). Focus-offset flips FIXED
(`fabad8d5e`); residual cursor↔window detachment persists. Mapping stays
open alongside present-lag / stale-replay — in-range wire values alone do
not prove a position was right for that instant. Do not confuse
with `bugs/rapid-pace-freeze.md` (render saturation) — different symptom,
different layer, though both bite during drags.

## Symptom

Multimon session (M27UP 4K@200% + Dell 1080p@100%, `/multimon`), single-screen
drags fine. Dragging across the monitor boundary (either direction): the
dragged window detaches from the cursor by a large offset (user reports ~500px
class gaps; one capture shows cursor ~500px from the grabbed window mid-drag
with the button held). Keyboard unaffected (no coordinates involved).

## Proven facts (measured, not inferred)

- Input pipeline delivers intact: 4,337 fastpath Mouse-Move PDUs on the
  wire match 4,357 arrival log lines ~1:1; keyboard/sync events present;
  zero transport loss; ACKs track through 16-bit DataSeq wraps live (3x).
  Caveat: healthy delivery does not rule out server-side processing or
  rendering delays — packets arriving says nothing about when the server
  acts on them or when frames present.
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
  values, zero trace fires. Remaining candidates unjudged: `t=1.7`
  `(0,0)->(3904,1773)` session-startup first-focus warp (known pattern);
  `t=42.6` `(3276,1081)->(4632,3)` fast fling, both endpoints in-range
  normalized M27UP positions (M27UP spans 1920–5759). In-range is not
  proven-correct — a position can sit inside the desktop and still be
  wrong for the pointer at that instant. Prior sessions showed ~13
  same-instant flips with off-screen values — gone after fix.
- User feel after fix: better but not done — window still detaches from
  cursor at the boundary (large gap persists).
- Signed 16-bit delta reanalysis (2026-09-11, 13,421 events,
  `20260911-064816-94958`): max real displacement 830px, only 3 events
  exceed 500px. The scary 65,504px jumps were small edge-crossing moves
  (e.g. +107px) wrapping past zero — that artifact class is retracted.
  Remaining (3 events): adjacent positions from different windows/times,
  i.e. discontinuities consistent with stale replay after a stall, but
  staleness is unproven — no event-age evidence (creation vs dequeue vs
  send timestamps) yet. First coalescer only merged same-window runs, so
  these discontinuities sailed through regardless.
- Live jump trace (temporary instrumentation, `motion jump` WARN lines):
  some jumps carried coordinates outside their own window (x=5056 on a
  3840-wide window; y=1766 on a 1080-tall window), real mouse (`which=0`),
  zero warp activity. Windows themselves are correctly sized. Note:
  outside-window is not automatic corruption — during a captured drag,
  window-relative coordinates can legitimately extend past that window.
  Offsets read correct (`off=(-1920,0)` / `(0,-1080)`), `winsize=`
  matches displays, `raw=1019 -> conv=2038` shows the x2 conversion step
  behaving — the conversion stage is consistent, not proven-correct.
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
- 65,504px wrap-artifact jumps: retracted as mapping evidence — signed
  recompute shows they were small edge-crossing moves wrapping past zero.
  Mapping correctness itself stays open (see status).
- Network/transport/wrap/server stall: captures show intact delivery
  throughout; server-side processing / render timing not covered by this.
- Missing input packets: retracted — early verdict was a measurement artifact
  (UDP-only captures hide all TCP input).
- Touch-emulated events: `which=0` (real mouse) on all teleports.
- Warp echoes: zero `moveMouseTo` warps logged across sessions.
- Oversized/overlapping windows: `winsize=` in trace matches displays exactly.

## Open hypotheses (ranked)

1. **Cross-window stale replay after main-thread stall (3 discontinuities
   observed, staleness unproven).** Motion backlog may replay positions
   from different windows/times adjacent. Needs event-age evidence
   (creation vs dequeue vs send) to graduate from discontinuity to stale.
   Fix in worktree: order-preserving global coalescing (latest position
   wins across windows; the run ends at the first queued non-motion
   event so press/release and topology changes keep order; relative
   deltas summed). Awaiting one re-drag test.
2. **Present lag.** Window position on screen may trail under drag burst
   because the main thread cannot present at drag rate (same root as
   rapid-pace freeze: render-upload bursts + event-backlog replay as one
   mechanism). `t=42.6` jump-cut is consistent with coalescing working,
   but wire-clean alone does not prove the mapping — deciding evidence
   is whether positions and displayed frames correspond to the right
   time.
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
  fling intermediates to endpoints as designed; missed cross-window
  discontinuities. Superseded by v2.
- Motion-event coalescing v2 (global across windows, uncommitted in
  `client/SDL/SDL3/sdl_freerdp.cpp`): latest wins across windows, rel
  deltas summed. Ordering bug found on review (2026-09-11): the
  motion-only peek/remove filter could reach past queued button, focus,
  and display-change events (e.g. `motion → release → press → motion`
  merging across the release/press), making it a confound. Fixed in
  worktree: peek the queue head in order, end the run at the first
  non-motion event. Built, awaiting drag test.
- Single-process dual screen recording (two AVFoundation opens race; loser
  wedges silently): rewritten around one ffmpeg + HW encode + readiness gate.
- Cursor-in-video: `-capture_cursor 1` works; cursor absent in most drag
  frames (was on the other display).
- Frame-by-frame cursor tracking: defeated by white UI elements; motion
  differencing found the drag windows instead.

## Next step (in progress)

One recorded Dell->4K crossing correlating four timestamps per motion:
event creation time, dequeue time, outgoing wire coordinates, and frame
presentation time. That distinguishes input backlog from presentation
backlog — the deciding evidence is whether positions and displayed
frames correspond to the right time, not "zero same-instant jumps"
(fast movement and coalescing can legitimately produce large steps, so
that is retired as a closing criterion). Mapping stays open until then.
Open follow-ups (not started): mid-session reconfiguration staleness
(nap/wake offset refresh), frame pacing under burst.
