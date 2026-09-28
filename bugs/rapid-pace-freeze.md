# UI freeze under rapid motion — render-thread saturation (not network)

Status: diagnosed 2026-09-09; SDL redraw scheduling fix implemented 2026-09-18,
awaiting live video-playback validation. Self-recovering performance cliff, not a
deadlock or crash. No crash report exists; the process stays alive throughout.

## Symptom

Fullscreen SDL client (3840x2160, AVC444, external 1080p display) shows the
macOS beachball and stops responding to input while the picture keeps updating
slowly. Stops on its own when the motion stops. No reconnect needed.

## Repro recipe (benchmark case)

Drag a Windows Terminal window rapidly. Worst-case motion (full-window
invalidation every pointer tick) times worst-case content (sharp glyph edges
force maximum AVC444 detail bitrate: heaviest decode, heaviest upload).
A full 4K dirty frame is ~33 MB of CPU→GPU upload; sustained dragging asks for
~GB/s on the main thread of a MacBook Air.

## Evidence

- `sample` of frozen PID (`/tmp/freerdp-84209-freeze.sample.txt`): main thread
  ~60% in `drawToWindow → blit → SDL_UpdateTexture → AGX Metal writeRegion`,
  ~40% in `updateSurface → SDL_RenderPresent`. RDP thread healthy in
  `tunnel_recv_full → bio_read → send_ack` the whole time.
- Debug-bundle capture (`~/rdp-debug/20260909-001032-84204/`, 102k packets,
  TLS-decrypted): zero retransmits, ACKs track every server DataSeq including
  three live 16-bit wrap crossings, no loss anywhere.
- Decrypted client stream: 4,337 fastpath Mouse-Move PDUs matching 4,357
  arrival log lines ~1:1, plus keyboard/sync events. Input path (SDL → core →
  fastpath → TCP → server) verified healthy end to end.
- `client.log`: no transport errors; only cert warnings and the (misleadingly
  named, actually per-event arrival) `input_skip` DEBUG lines.

## Root cause

`SdlWindow::blit` uploads every dirty rect via `SDL_UpdateTexture` on the main
thread with no pacing. Under full-frame bursts the upload queue never drains:
event pumping starves (multi-second input lag reads as "cannot interact"),
while decode/network (other threads) keep working — hence updates continue
and everything recovers when the burst ends.

## Ruled out (with wrong turns recorded)

- UDP/transport bug: captures prove perfect sync; wrap handling held live 3x.
- Dead server: server kept producing; one real server-side kill (DavidPC event
  log 23:54:55, error 0x80090330 + TCP 1236 local abort) was a separate
  Tailscale/path incident, diagnosed independently.
- Missing client input: an early "zero input on the wire" verdict was a
  measurement artifact (UDP-only capture files hide all TCP input; dissector
  stack strings don't name fastpath input). Decrypted full capture retracts it.
- Hang/crash of 2026-09-08 session: separate SIGTERM-deaf teardown wedge,
  fixed in 764326c61 (no-throw drain + abort in term handler).

## Mitigations (no code)

- `+async-update` cannot help this build: it is forced off in `update.c`, as
  confirmed by the startup log (see the note below).
- `/size:1920x1080`: quarters upload bandwidth; expected ~4x sustainable fps
  if upload-bound (predicted, not yet measured).

## Proposed fix

Frame pacing under burst, not faster uploads: when the main thread falls
behind, coalesce superseded dirty regions and present the latest instead of
grinding through every queued rect (what mstsc does). Validate with the repro
recipe above: drag Terminal, no beachball, input stays live.

## Benchmark: flashing-stripes console test (2026-09-09)

Recipe: console app emitting quick flashing stripes (rapid full-frame
high-frequency detail changes) while interacting. This stresses encode bitrate,
decode, upload, and present simultaneously — the pipeline worst case.
Reference point: office Windows-to-Windows mstsc over WAN feels smoother than
the current client on 3 ms home Wi-Fi; transport measured clean (zero
retransmits in 16k+ datagrams, ~60 Hz server cadence intact), so the entire
deficit is client-side present pacing.
Acceptance bar: sustained ~50 fps, no beachball, input stays live throughout.
 Wire evidence: `~/rdp-debug/20260909-212852-65076/` (capture + TLS secrets +
client.log); video evidence to be added on retake with `-capture_cursor 1`.

## 2026-09-09 debug sessions (free-rdp-debug.py rig)

- Rig: `free-rdp-debug.py` (ring capture + client.log + TLS secrets +
  session.txt per run) + `freeze-snapshot.sh` (sample/CPU/load/GPU/sockets).
- Decrypted-TLS proof: 4,337 fastpath Mouse-Move PDUs match 4,357 arrival log
  lines; keyboard/sync present. Input pipeline healthy end to end. Retracts the
  earlier "zero input on wire" verdict (artifact of UDP-only captures).
- Sample of frozen PID: main thread ~60% `SDL_UpdateTexture` (Metal upload) +
  ~40% `RenderPresent`; RDP thread healthy in recv/ACK. Render-bound, network
  innocent. Self-recovery on burst end confirmed by user.
- UNKNOWN (open): left-half screen blur with razor edge at x~1912 in one
  screen recording during a drag; possibly compositor/capture tile under GPU
  saturation, not RDP content. Cursor invisible in those frames (no
  `-capture_cursor`), so no offset measured.
- Display IDs renumber between sessions (lid/topology changes); scale
  overrides keyed by SDL ID can silently target wrong monitors. Override
  application now logged at INFO with display names.
- Unrelated server-side kill (DavidPC event log 23:54:55, 0x80090330 + TCP
  1236 local abort, both ends agree to the second): path flap, not our code.
- `+async-update` is a no-op here (`FORCE_ASYNC_UPDATE_OFF` hardcoded in
  update.c:49, upstream #10153 + CVE cluster behind it). Do NOT enable.

## Connection failures — see bugs/connection-failures.md

Transport/server-side kills and black-screen-at-connect are tracked
separately; this file stays render-path only. (Moved 2026-09-10.)

## Video playback recurrence and redraw scheduling fix (2026-09-18)

See [the retained incident report](../VIDEO-PLAYBACK-FINDINGS-20260918.md) for
profiles, packet statistics, and the recovery comparison. During the freeze all
170 main-thread samples were in drawing; after recovery 181/182 were waiting
normally for UI events. Video continued while mouse and keyboard control failed.

The previous `SDL_EVENT_USER_UPDATE` handler drained a FIFO of damage batches
until empty. A continuous video producer could keep that loop running without
returning to input handling. Every completed frame also posted a separate SDL
event, letting stale redraw notifications accumulate.

The implementation now:

- Keeps at most one pending redraw notification in normal operation.
- Merges overlapping damage against the latest framebuffer instead of retaining
  old frame batches; sparse regions remain separate.
- Caps the pending region list at 32 rectangles, falling back to a bounding box
  when necessary. This preserves all damage while bounding uploads per UI turn.
- Draws one snapshot per redraw event, pumps native input, then returns to the
  event loop. Damage arriving during drawing posts another event behind already
  queued input.
- Clears pending damage when the connection ends.

Compressed frames are still decoded in order. This is presentation scheduling;
it does not change the transport, H.264 decoder, or server codec selection.

`TestSDLUpdateQueue` covers large bursts, bounded damage coverage, sparse monitor
updates, queued keyboard input during continued production, failed wakeups,
session reset, wakeups consumed by dialogs, and a concurrent producer/consumer. The standalone test passed
normally and with AddressSanitizer/UndefinedBehaviorSanitizer.

Live acceptance check after restarting with the new build: reopen the same video
tab and leave it playing while moving the pointer, typing, and operating the
connection bar. Repeat across the two displays and while dragging windows. Input
must remain responsive, and stopping video must leave no stale regions. This
check remains pending; video block artifacts and YUV444 worker contention are
separate unresolved questions.

Validation completed: the full SDL client built successfully in
`build/video-responsive`; `TestSDLUpdateQueue`, `TestSDLInputMapping`, and
`TestSDLMonitorScale` all passed through CTest. The updated queue test also passed
with AddressSanitizer and UndefinedBehaviorSanitizer. The executable passed its
`/version` startup check before and after installation.

The usual debug-launcher executable at
`build/videotoolbox/client/SDL/SDL3/sdl-freerdp` was replaced atomically with this
build on 2026-09-18 at 18:54 CEST. The running session was not restarted. The new
executable resolves its FreeRDP libraries from `build/video-responsive`, so retain
that build directory. The prior executable is preserved locally at
`diagnostics/video-playback-20260918/sdl-freerdp-before-redraw-fix`; build hashes and
test output are in `redraw-fix-build.json` and `redraw-fix-tests.log` beside it.
