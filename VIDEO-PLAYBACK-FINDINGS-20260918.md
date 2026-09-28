# Video-playback beachball — 2026-09-18

## Conclusion

The sampled Mac UI thread is saturated in rendering. This is strong evidence for the previously documented rapid-motion rendering problem, distinct from the earlier Windows TermService crash. The reported blocky picture/artifacts are not yet explained: this profile establishes a responsiveness bottleneck, not the origin of visual corruption or compression artifacts.

## Live session and observations

- Session: `/Users/david/rdp-debug/20260918-162103-39007`, client PID 39013, revision 9caf7000a; desktop negotiated at 5760×2160 across two displays.
- Profile: macOS `sample 39013 2 10`, collected at 17:43:27 CEST. No LLDB attachment, packet injection, session restart, or settings change was performed.
- All 170 main-thread observations are in `SdlContext::drawToWindow` / `SdlWindow::drawRects`. 168/170 are in `SDL_UpdateTexture`: 87 in command flushing, 81 in the Metal texture-update path. This is sampled wall time, not a CPU-utilization measurement.
- The RDP thread is polling/receiving TLS-over-UDP and sending acknowledgements; it is not stuck at a sequence gap in this sample.
- The graphics-channel worker is actively processing AVC444. A large part of its sampled time is in YUV444 thread-pool coordination: 95/170 observations are in `CountdownEvent_AddCount` critical-section waits. Worker threads are also contending in completion signalling. This is a second client-side performance lead, not proof of deadlock.
- VideoToolbox H.264 decoding is active. There are no new codec failures or transport disconnects in the inspected log around the incident. The last logged decoder initialization is 17:03:58.
- Seven successful omitted-zero wrap recoveries are logged in this session through 17:44:52; the previous wrap was 17:40:24.

## Packet snapshot

The retained slice covers 17:40:16.942–17:42:37.863 CEST, before the profile. Analysis includes 500 initial handshake packets for native RDP-UDP dissection; those packets are excluded from the slice statistics.

- 114,198 UDP packets, including 57,088 server packets.
- 57,085 server channel chunks; 56,923 unique channel sequence positions; 162 duplicates.
- The only absent position between the first and last observed channel sequence is the reserved zero at the wrap. No other channel holes appear in this slice.
- Approximately 3.97 Mbit/s server UDP traffic including captured packet headers. This does not measure encoder bitrate or delivered frame rate.
- Exact individual-ACK matching found 55,256 matches: median 2.16 ms, p99 127 ms, maximum 6.71 s. Individual ACKs are not a complete acknowledgement audit because ACK vectors were not expanded. These delays and duplicates mean this is not evidence of a perfectly lossless/latency-free path, although the channel sequence is complete apart from the known skipped zero.

## Source findings

- `client/SDL/SDL3/sdl_freerdp.cpp`, `SDL_EVENT_USER_UPDATE`: drains the rectangle queue until empty before returning to event handling. A continuing producer can starve input handling.
- `client/SDL/SDL3/sdl_context.cpp`, `push` / `pop`: FIFO queue, with no bounding or coalescing at this point.
- `client/SDL/SDL3/sdl_window.cpp`, `blit`: alternates updating and drawing the same texture for each dirty rectangle. The profile shows command flushing within these texture updates.
- `SdlContext::drawToWindow` also holds `_critical` across drawing/presentation.
- This matches the performance problem already recorded in `bugs/rapid-pace-freeze.md`.

## Next work

Bound work per UI event and coalesce pending dirty regions against the latest framebuffer while preserving all invalidated areas. Batch texture uploads before drawing where possible, then present at a controlled cadence. Verify that input stays responsive during sustained video and window dragging. Investigate YUV444 task granularity/coordination separately.

Do not drop compressed RDP/H.264 updates to discard old visual work: decoder state still needs those updates. Coalescing should happen at the presentation stage.

For the block artifacts, a controlled comparison is still needed to distinguish server encoding/quality adaptation, client decode/conversion, and presentation problems. The earlier NVIDIA encoder crash is relevant background but does not establish that NVIDIA caused this incident. Pausing the video is a useful non-destructive diagnostic: responsiveness should recover as rendering pressure subsides, if this is the same performance cliff. The user subsequently confirmed spontaneous recovery, but did not specify whether video had stopped or its workload changed.

`+async-update` is forced off by this build; the startup log confirms it is not an available workaround.

## Artifacts

The supporting evidence is preserved in [`diagnostics/video-playback-20260918/`](diagnostics/video-playback-20260918/) within this project. These copies do not depend on `/tmp`. The evidence directory is locally ignored by Git; this report can be committed separately.

- [sample.txt](diagnostics/video-playback-20260918/sample.txt): original profile
- [client.log](diagnostics/video-playback-20260918/client.log): log snapshot through 17:40:24; later live log checked through 17:44:52
- [session.txt](diagnostics/video-playback-20260918/session.txt): session metadata
- [handshake.pcapng](diagnostics/video-playback-20260918/handshake.pcapng), [capture_00006_20260918174017.pcapng](diagnostics/video-playback-20260918/capture_00006_20260918174017.pcapng): retained packet evidence
- [udp.tsv](diagnostics/video-playback-20260918/udp.tsv), [packet-summary.json](diagnostics/video-playback-20260918/packet-summary.json): extracted packet metadata and statistics

## Recovery comparison

The user confirmed that video continued during the loss of control, then the session recovered. A second two-second profile of the same PID at 17:48:59 CEST shows 181/182 main-thread observations (99.45%) in SDL/Cocoa event waiting, with just one in drawing. During the incident all 170 observations were in drawing. This before/after comparison strongly supports UI event starvation under rendering load.

The retained post-recovery log ([client-recovered.log](diagnostics/video-playback-20260918/client-recovered.log)) has no input-event entries between 17:40:07.581 and 17:45:34.796, then input entries resume and continue through 17:49:28. A log gap alone cannot distinguish inactivity from blocked event handling; combined with the user's attempted interaction, live profile, and continued playback, it corroborates the diagnosis. The name `input_skip` is misleading: these entries report input arrival and show `connected=true, suspended=false`.

No reconnect was needed. No live-session mutation was performed. The second profile is saved as [sample-recovered.txt](diagnostics/video-playback-20260918/sample-recovered.txt).
