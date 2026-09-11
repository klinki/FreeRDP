# Startup slideshow: visible fullscreen flash per display before connect

Status: fixed in worktree (2026-09-11), pending visual confirm on hardware.
Do not confuse with `bugs/drag-mapping.md` (input mapping) or
`bugs/rapid-pace-freeze.md` (render saturation) — different layer.

## Symptom

Starting the SDL client flashes a fullscreen window on each display in
turn — a "slideshow" — before the session appears. It happens even when
the remote host is offline and the connect fails, so the entire flash
was meaningless: no session ever follows it.

The earlier hidden-window reveal (`72a6c0d04`) did not stop it.

## Root cause (measured, not inferred)

The slideshow was never the real session windows. Chain:

- `main()` → `detectDisplays()` → `addOrUpdateDisplay(id)` per display
- → `SdlWindow::query(id)` → `createDummy(id)` created a **visible**
  (`HIDDEN=false`) 64x64 window, then `SetWindowPosition` +
  `SetWindowFullscreen(true)` per display to read back pixel dimensions
- all before any network I/O (`freerdp_connect` runs later on the RDP
  thread; real windows are created in `postConnect` → `createWindows`,
  born hidden)

So every startup toured every display fullscreen just to probe monitor
sizes, then tried to connect. Offline host failed after the tour.

## Fix

`client/SDL/SDL3/sdl_window.cpp` only:

- `createDummy`: `HIDDEN=true`, no position/fullscreen transitions —
  never visible, ever.
- `query(id)` / `rect(id)`: windowless probe — pixel size = logical
  `SDL_GetDisplayBounds` × scale; scale from the hidden dummy's
  `GetWindowDisplayScale` (fallback: content scale → 1.0). Same
  `rdpMonitor` fields and debug logs as before.
- Real windows unchanged: still deferred to `postConnect` (sizes come
  from negotiation, cannot pre-date connect), still born hidden →
  positioned → fullscreened while hidden → shown once on the right
  display.

Result: zero visible windows before connect; on success, session
windows appear directly on their target displays.

## Verification

- `build/videotoolbox` rebuilds clean (`sdl3-freerdp` links).
- No `SetWindowFullscreen` remains on any pre-connect path (only
  `SdlWindow::fullscreen`, which runs on hidden real windows).
- Manual test (needs hardware):
  1. Offline host → expect clean connect error, no flash at all.
  2. Online multimon → expect windows appearing directly on the
     correct displays, no cross-display tour.
