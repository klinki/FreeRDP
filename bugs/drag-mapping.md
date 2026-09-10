# Cross-monitor drag: cursor↔window divergence (multimon input mapping)

Status: open, under active diagnosis (2026-09-10). NOT fixed. Do not confuse
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
- Teleport events exist in captures: consecutive sent coords jump ~1900px
  within the same millisecond (e.g. 1660->3580->1660, 83->2003->182), always
  ~one screen width apart. No physical pointer does this.
- Live jump trace (temporary instrumentation, `motion jump` WARN lines):
  teleports carry coordinates EXCEEDING their own window
  (x=5056 on a 3840-wide window; y=1766 on a 1080-tall window), real mouse
  (`which=0`), zero warp activity. Windows themselves are correctly sized.
- All teleport endpoints divided by 4 land inside points space on both
  displays — suggests a double x2 conversion somewhere, unproven.
- Display IDs renumber between sessions (built-in back as Main reshuffles
  everything); scale overrides keyed by SDL ID can silently target the wrong
  monitor (mitigated: override application now logged at INFO with names).

## Ruled out

- Constant origin-normalization error in `applyMonitorOffset`: verified the
  math both ways; a committed "fix" (raw negative coords) made clicks land a
  screen away and was reverted the same night. The stock formula matches the
  click-test ground truth. Wire layout (-1920) vs server working space
  (normalized) was a red herring — clicks prove the stock mapping lands.
- Network/transport/wrap/server stall: captures show perfect sync throughout.
- Missing input packets: retracted — early verdict was a measurement artifact
  (UDP-only captures hide all TCP input).
- Touch-emulated events: `which=0` (real mouse) on all teleports.
- Warp echoes: zero `moveMouseTo` warps logged across sessions.
- Oversized/overlapping windows: `winsize=` in trace matches displays exactly.

## Open hypotheses (ranked)

1. **Double coordinate conversion (x4 total).** Every teleport endpoint fits
   points-space after /4 on both displays. One `SDL_ConvertEventToRenderCoordinates`
   site exists in code; the second x2 is unlocated. Could also be HiDPI
   points-vs-pixels confusion inside SDL on 200%-scaled displays.
2. **Stale events from a previous topology/mode.** Teleports cluster around
   drags and mode switches; a backlog of pre-switch events delivered late
   would carry then-valid, now-foreign coords. Fits the maximize-incident
   shape. Check: event timestamps vs switch time.
3. **Two event sources for one pointer** (e.g. relative+absolute both firing):
   would produce paired contradictory values. No second source identified yet.

## Tried

- Offset separation patch (input vs draw offsets): made it worse, reverted.
- Single-process dual screen recording (two AVFoundation opens race; loser
  wedges silently): rewritten around one ffmpeg + HW encode + readiness gate.
- Cursor-in-video: `-capture_cursor 1` works; cursor absent in most drag
  frames (was on the other display).
- Frame-by-frame cursor tracking: defeated by white UI elements; motion
  differencing found the drag windows instead.

## Next step (in progress)

Stage-logging trace (raw SDL coords vs post-conversion vs sent, per jump) is
built into `build/videotoolbox`. One Dell->4K drag with it pinpoints the
multiplying stage: raw points -> conv pixels must read x2 on M27UP, x1 on
Dell. Anything else names the culprit. Awaiting one drag + client.log.
Open follow-ups (not started): mid-session reconfiguration staleness
(nap/wake offset refresh), motion-event coalescing (drop superseded moves
per frame — also fixes backlog-replay jumps), frame pacing under burst.
