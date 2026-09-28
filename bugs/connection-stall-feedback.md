# Connection stall: frozen screen with no user feedback

Status: implemented in worktree (2026-09-11), needs manual stall test.
Related: `bugs/connection-failures.md` (aborts/errors),
`bugs/rapid-pace-freeze.md` (render saturation looks identical on screen).

## Implementation

- `SdlContext::_reconnecting` (atomic) + `SDL_EVENT_USER_RECONNECTING`:
  RDP thread sets it around `client_auto_reconnect` in
  `sdl_client_thread_run`; main thread presents / clears.
- `SdlWindow::updateStalledSurface(dots)`: re-presents the last frame,
  dims fullscreen black @48/255 (~19%, tunable via `stalledDimAlpha`),
  centers cached `Reconnecting.`/`..`/`...` (32pt OpenSans + shadow).
  Render target untouched → next normal frame paints clean, no explicit
  clear needed.
- Animation: main-loop 1s timeout tick advances dots (500ms phase).
- Honest scope: covers the transport-dead → retry window only. Silent
  TCP death is still bounded by OS timeout (no heartbeat knob —
  `SupportHeartbeatPdu` is deprecated); render-only stalls deliberately
  do not trigger it.

## Symptom

When the connection stalls for any reason (network drop, server stop,
transport death), the user is told nothing. The remote image simply
freezes: no input effect, no message, no indication anything is wrong.
A frozen-by-stall screen is indistinguishable from a frozen-by-render
screen, so users cannot tell whether to wait, reconnect, or restart.

## Desired behavior

Detect the stall and say so on every session window:

- Dim all session screens slightly (10–25%, rate adjustable — exact
  value to be tuned) so the state change is visible but content stays
  readable.
- Overlay centered text `Reconnecting...` with a loading indicator.
  Simplest proposal: cycling dots (`.`, `..`, `...`, repeat) on a timer;
  a spinner is a fine alternative if it falls out of existing primitives.
- Clear automatically on recovery; on final failure hand over to the
  existing error path (`sdl_client_cleanup` → error dialog).

## Detection (open design)

Candidate signals, to be confirmed against the transport code:

- `sdl_client_thread_run` loop: `freerdp_check_event_handles` failing /
  `WaitForMultipleObjects` unsticking without progress, plus the
  `client_auto_reconnect` retry window — the overlay should cover exactly
  the "transport dead, retry in flight or imminent" interval.
- Must not fire for render-only stalls (main-thread saturation with a
  live transport) — or if it does, the text must not claim the network
  is down. Distinguishing these two is the hard part of this ticket.

## Rendering notes

- Overlay should be client-side only (drawn onto the render target in
  `drawToWindow`/`updateSurface` style paths), no protocol change.
- Must work per-window in multimon (all windows dim together) and must
  not fight the topbar pill or cursor layers.
- Animation needs a timer that keeps firing while the RDP thread is
  stuck — main-loop timer, not transport-thread driven.

## Acceptance

1. Kill the network mid-session → all windows dim + indicator within a
   few seconds (exact budget TBD).
2. Restore the network → overlay clears, session resumes, no restart.
3. Kill the server permanently → overlay shows during retries, then the
   normal failure dialog takes over (no stuck overlay).
4. Offline host at startup → existing fast error path, no overlay flash.
