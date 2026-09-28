# Branch review

Range: `master~1` (`cf09c1aec`) through `feat/performance-macos-native`
(`8d91ba1bd`), 84 commits including master's per-monitor scale overrides.

## Multimonitor and startup

Per-monitor scale overrides, mixed-DPI input mapping and topbar interactions: OK.

1. **P2: Monitor probing confuses content scale with pixel density.**
   `sdl_window.cpp:1086–1088` multiplies display bounds by display scale. Windows
   and X11 already express these bounds in pixels: a 4K screen at 200% is
   reported as 7680x4320. Keep UI scale separate from pixel dimensions.

## UDP

2. **P1: Retrying an ACK timeout duplicates stream bytes.**
   `rdpeudp.c:2804–2825` discards pending chunks and returns a retryable BIO error.
   Retrying the write uses fresh channel sequences, including for bytes already
   delivered. The reproduction receives `ABCDEFGHABCDEFGH` for one eight-byte
   write. Preserve write progress or fail the transport terminally.

3. **P1: Wrap heuristics discard genuine delayed channel zero.**
   `rdpeudp.c:1892–1905` treats 250 ms or 64 buffered chunks as proof of omission.
   A genuine zero arriving afterward is discarded, corrupting the TLS stream.
   Time and buffer pressure cannot establish that a channel label is unused.

4. **P2: Wrap-watch timeout depends on another packet arriving.**
   `rdpeudp.c:1930–1932` evaluates the watch from packet delivery only. With one
   buffered post-wrap packet followed by silence, it makes no progress after
   251 ms and fails the ten-second reassembly watchdog. Recovery decisions
   must remain loss-safe and cannot depend on further incoming traffic.

5. **P2: Empty channel list becomes migrate-all.**
   `multitransport.c:820–823` leaves `mappingActive` false for an explicit empty
   UDP DVC list; that value routes every channel over UDP. Distinguish absent
   CHANNELLIST from a present list containing zero IDs.

## Performance

Motion coalescing: OK.
Bounded redraw scheduling and damage merging: OK.
Per-monitor clipping and unchanged-window skipping: OK.
Texture upload batching: OK.
Asynchronous update launcher settings: OK.
Graphics extraction, replay and comparison tools: OK.

6. **P2: Reusable YUV slots wait on historical work.**
   `yuv.c:548` waits on every cached work object, including ones not submitted
   by this batch. Following a full 4K frame, each 16x16 update does 40 waits
   instead of one (default 512-pixel tiles). Local timing rises from about
   43 to 59 microseconds. Drain the current batch only, in all three paths.

7. **P2: Skipped-window metrics do not advance intervals.**
   `sdl_render_metrics.hpp:313–318` only starts an interval on a skipped present.
   Its expiry is handled by `beginFrame`, which an unchanged monitor never calls.
   Sixty seconds of skips produce one record; the recommended two warmup
   intervals remove the entire measurement. Stalled presents are also affected.

## Reconnect overlay and diagnostics

8. **P3: Reconnect textures outlive their renderer.**
   `sdl_window.cpp:122–130` destroys the renderer before `_stalled` is destroyed.
   Its texture deleters subsequently receive stale handles. Reproduced SDL's
   invalid-texture error; no crash observed. Reset `_stalled` before the renderer.

9. **P2: Copytruncate rotation is incompatible with the active tee writer.**
   `free-rdp-office-debug.py:92–94` truncates a file while non-append `tee` keeps
   its old offset. Subsequent writes create NUL-filled holes and logical file
   size keeps growing. The general debug launcher has the same pattern.

## Validation

All six SDL tests, the core UDP test, the YUV codec test, and twelve graphics
replay tests passed. The focused reproductions expose the failures above.
Windows/X11 scaling was modeled from SDL's documented semantics. No live RDP
session or Metal presentation session was run. See README.md for preserved
harnesses, exact commands, isolation details, and baseline observations.

## Follow-up fixes

The findings above describe the original target. See [FIXES.md](FIXES.md) for
individual commits and validation on `codex/macos-performance-review-fixes`.
A fresh build also exposed a macOS replay-tool link failure without LTO; that
follow-up issue has a separate fix and passed all twelve replay tests.
