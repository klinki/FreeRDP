# UI freeze under rapid motion — render-thread saturation (not network)

Status: diagnosed 2026-09-09, unfixed. Self-recovering performance cliff, not a
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

- `+async-update`: decouples decode from the render thread; measures the true
  upload/present ceiling.
- `/size:1920x1080`: quarters upload bandwidth; expected ~4x sustainable fps
  if upload-bound (predicted, not yet measured).

## Proposed fix

Frame pacing under burst, not faster uploads: when the main thread falls
behind, coalesce superseded dirty regions and present the latest instead of
grinding through every queued rect (what mstsc does). Validate with the repro
recipe above: drag Terminal, no beachball, input stays live.
