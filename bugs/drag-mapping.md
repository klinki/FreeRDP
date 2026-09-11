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

## Directional observations (2026-09-11, user-reported, uninstrumented)

- **M27UP → Dell:** cursor grabbed at the middle of the dragged window's
  top bar appears on the **right side** of the window once on the Dell.
  Dragging further left → gap **grows**; dragging back right → gap
  **shrinks**; back on M27UP → cursor correct again (middle of top bar).
- **Dell → M27UP:** cursor appears **above** the dragged window once on
  the M27UP. Dragging back to the Dell → correct position again.
- Read: the residual error is **position-dependent** (gap scales with
  distance), **destination-display-only**, and **self-correcting on
  return** — in both directions. Not yet confirmed on the wire.

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
- Slow-drag wire verdict (2026-09-11, `20260911-085454-33384`,
  M27UP→Dell, 9.6s deliberate drag, 798 moves): grab `DOWN (3015,1212)`
  → release `UP (371,1489)`, x strictly decreasing (one 1px up-blip),
  max step 25px. Boundary crossing `(1921,1365)→(1916,1363)` is a clean
  5px step. Pacing p50 8ms / p95 13ms, no backlog bursts. Zero teleports,
  zero wrap artifacts, zero stale replays — none of the discontinuity
  mechanisms fired during a drag the user watched detach. End click
  `DOWN/UP (1146,1824)` identical, no bounce (awaiting on-screen target
  from user as a calibration anchor). Open flag: y drifted +305px over
  the drag — hand drift vs y-component error, inseparable without local
  coords.
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

1. **Wrong-display scale following the pointer across the boundary.**
   The gap grows with distance (multiplicative signature), appears only
   on the destination display, and vanishes on return — fits events
   converted with the *source* display's scale/offset after the pointer
   crossed (e.g. M27UP ×2 applied to Dell coords). Same family as the
   cross-window stale replay below, but in mapping rather than timing.
   Decisive test: fit sent = a·local + b per display from one slow
   instrumented drag — a≠1 names scale, b≠0 names offset.
2. **Cross-window stale replay after main-thread stall (3 discontinuities
   observed in fast/bursty sessions, staleness unproven).** Falsified as
   the cause of the slow-drag symptom: the 2026-09-11 symptomatic slow
   drag shows zero discontinuities. Remains possible for fast flings.
   Needs event-age evidence (creation vs dequeue vs send) to graduate
   from discontinuity to stale. Fix in worktree: order-preserving global
   coalescing (latest position wins across windows; the run ends at the
   first queued non-motion event so press/release and topology changes
   keep order; relative deltas summed). Awaiting one re-drag test.
3. **Server-side DPI virtualization (Windows moves the window itself).**
   M27UP@200% vs Dell@100%: when a window crosses DPIs, Windows rescales
   + re-anchors it mid-cross independent of anything we send. Predicts a
   visual jump even with a perfect wire — distinguishable only by the
   timestamp-correlation experiment (positions right, framesDPI-shifted).
   Untouched.
4. **Present lag.** Window position on screen may trail under drag burst
   because the main thread cannot present at drag rate (same root as
   rapid-pace freeze: render-upload bursts + event-backlog replay as one
   mechanism). `t=42.6` jump-cut is consistent with coalescing working,
   but wire-clean alone does not prove the mapping — deciding evidence
   is whether positions and displayed frames correspond to the right
   time.
5. **Event storm at crossing** (`Cocoa_SyncWindow` 79% sample):
   display-change events firing as pointer crosses stall the main thread
   exactly when smoothness matters. Untouched.
6. **Stale events from a previous topology/mode.** Teleports cluster around
   drags and mode switches; a backlog of pre-switch events delivered late
   would carry then-valid, now-foreign coords. Fits the maximize-incident
   shape. Check: event timestamps vs switch time.
7. **Two event sources for one pointer** (e.g. relative+absolute both firing):
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

Two cheap calibration items, no code, no rebuild:
1. On-screen target of the `(1146,1824)` end click — one (local→sent)
   anchor.
2. Deliberate clicks on known landmarks, 3–4 per display: fits
   sent = a·local + b per display and per direction (a≠1 names scale,
   b≠0 names offset), settling H1 vs H3 in one shot.
Same runs feed the timestamp correlation (creation, dequeue, wire
coords, frame presentation) that separates input backlog, presentation
lag, and server DPI moves. Mapping stays open until then.
Open follow-ups (not started): mid-session reconfiguration staleness
(nap/wake offset refresh), frame pacing under burst.
