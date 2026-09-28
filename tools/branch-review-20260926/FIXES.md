# Review fixes and validation

Branch: `codex/macos-performance-review-fixes`, based directly on
`feat/performance-macos-native` at `8d91ba1bdf736a0c17c030926cb617a1ea6778ca`.
The review and original reproduction scripts were committed first as `e6fcc79df`.
The original working directories were not switched or modified for these fixes.

## Office capture follow-up

The initial wrap fix below still stalled on all six real wraps from the office
capture. A follow-up replaces the permanent lost-history flag with DataSeq-ordered
channel bounds. All six now recover and match authenticated TLS streams exactly.
See [OFFICE-CAPTURE.md](OFFICE-CAPTURE.md) for evidence, the fix, and replay commands.

## Issue commits

Numbers correspond to [REVIEW.md](REVIEW.md).

| Issue | Commit | Result |
|---|---|---|
| 1. Monitor dimensions | `336501b57` | Use pixel density for pixel dimensions; retain display scale for UI sizing. |
| 2. UDP write retries | `b8135f2d8` | Latch ambiguous send failures, return a terminal BIO error, and prevent replay over UDP or TCP. |
| 3. Delayed channel zero | `587fc862d` | Remove timeout, chunk-count, and ring-pressure guesses. Preserve delayed zero; fail safely on receive-window overflow. |
| 4. Silent wrap gaps | `455f38312` | Test maintenance-timer wakeup and reconnect cleanup for unresolved wrap gaps, twice across reconnects. |
| 5. Empty migration list | `c0d2d57e0` | Preserve an explicitly empty DVC list instead of migrating every DVC. |
| 6. Reused YUV work slots | `307e23b2d` | Wait only for work submitted in the current decode, combine, or encode batch. |
| 7. Skipped/stalled metrics | `9ecb9778c` | Flush expired intervals even when a window does not render a frame. |
| 8. Reconnect textures | `3bab63db9` | Release stalled-overlay textures before their renderer. |
| 9. Log rotation | `c7692b28f` | Start with a fresh log and keep tee in append mode across copytruncate. |
| Follow-up: replay linking | `55fc47504` | Strip unused object code on macOS so the replay runner links with LTO disabled. |

Issues 3 and 4 share the unsafe wrap-watch mechanism. Issue 3 removes that
mechanism; issue 4's separate commit verifies the resulting silence behavior.
An unresolved wrap no longer guesses after 250 ms. It waits for the missing bytes
or the existing ten-second watchdog, which triggers the normal transport-failure
reconnect path. Overflow can fail sooner. The proven zero-skip fast path remains.
This trades speculative recovery of an ambiguous omitted zero for stream integrity.
An ACK timeout similarly requires reconnect because the peer may already have
received the failed write. No new recovery abstractions were added.

## Validation on the fixed branch

- Built `sdl3-freerdp`, core/codec tests, and six SDL test binaries from this worktree.
- Eight selected CTest checks passed: UDP, YUV, and all six SDL checks listed below.
- All preserved correctness reproductions passed. A genuine late zero remains in
  order after both 251 ms and 64 later chunks. Failed-write retries deliver exactly
  eight bytes, and an explicit empty migration list does not route DVC 42.
- YUV instrumentation reports one wait per small update before and after a 4K frame,
  instead of increasing to forty waits. Timing remains informational.
- Both modeled monitor density cases report 3840x2160 at 200% scale. Overlay teardown
  leaves no SDL error. Skipped/stalled-only metrics have dedicated interval tests.
- Both real log rotators, exercised with their corresponding launchers' tee mode,
  produce exactly `next\n` after truncation. Both launcher shell syntax checks pass.
- Twelve graphics replay tests passed against a newly linked runner using this
  worktree's production objects and libraries, with LTO disabled.

The local build is `/Users/david/projects/FreeRDP/build/review-fixes`; retained
reproduction binaries, logs, and `commands.json` are in its `review-results/`
subdirectory. CTest and replay logs are retained there too.

The build uses Release, SDL3, FFmpeg and VideoToolbox, with testing enabled and
Opus disabled. `WINPR_HAVE_PIPE2=OFF` was necessary because this host's macOS 26.7
runtime does not supply the symbol detected by Xcode's macOS 27 SDK. This is a
local CMake setting, not a source change.

## Re-run

Use a matching Unix Makefiles build with the required dependencies configured:

```sh
REVIEW_SOURCE=/path/to/fixes-worktree
REVIEW_BUILD=/path/to/matching/cmake-build
cmake --build "$REVIEW_BUILD" --parallel 8 --target \
  sdl3-freerdp TestCore TestFreeRDPCodec TestSDLRenderWindow \
  TestSDLRenderMetrics TestSDLRenderGeometry TestSDLUpdateQueue \
  TestSDLInputMapping TestSDLMonitorScale
ctest --test-dir "$REVIEW_BUILD" --output-on-failure \
  -R '^(TestRdpeUdp|TestFreeRDPCodecYUV|TestSDL(RenderWindow|RenderMetrics|RenderGeometry|UpdateQueue|InputMapping|MonitorScale))$'
python3 -B "$REVIEW_SOURCE/tools/branch-review-20260926/run.py" \
  --source "$REVIEW_SOURCE" --build "$REVIEW_BUILD" \
  --output "$REVIEW_BUILD/review-results"
python3 -B "$REVIEW_SOURCE/tools/graphics-replay/build.py" \
  --source "$REVIEW_SOURCE" --build "$REVIEW_BUILD" \
  --output "$REVIEW_BUILD/graphics-replay/optimized"
REPLAY_BINARY="$REVIEW_BUILD/graphics-replay/optimized" \
  python3 -B -m unittest discover \
  -s "$REVIEW_SOURCE/tools/graphics-replay" -p 'test_*.py'
```

No live RDP session, Windows/X11 desktop, or Metal presentation session was run.
The monitor checks use mocked SDL queries; the UDP tests use synthetic packets
and deterministic clocks, with a real maintenance timer for the wakeup test.
