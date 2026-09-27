# SDL render workload

`workload.html` is a deterministic, offline browser workload for checking the
FreeRDP SDL3 client while a remote Windows desktop is being rendered. Copy the
single HTML file to the test machine or serve the repository directory with a
static file server. It has no package, font, image, or network dependency.

The page draws integer aligned canvas pixels with a stable grid, edge markers,
color bars, labels, and animated content. Those fixed features make a clipped
rectangle, stale tile, seam, or one-pixel offset easy to spot in a screenshot.
The animated area represents the changing pixels in a remote desktop; the
page redraws at a configurable logical frame clock, so a run does not depend on the
browser's instantaneous frame rate.

Use a URL like this for the complete three-phase run:

```
workload.html?mode=cycle&duration=30&seed=20260919&width=3840&height=2160
```

`cycle` runs one region, sparse tiles, and a large rectangle crossing the
center seam. It ends by drawing a static checkpoint. The checkpoint reports a
pixel hash in the status panel and is available to automation as
`window.renderBenchmark.checkpoint`. It can be run directly with:

```
workload.html?mode=cycle&checkpoint=1&seed=20260919&width=3840&height=2160&hud=0
```

The same seed, dimensions, mode, and checkpoint parameters produce the same
canvas pixels within the same browser and platform. Font rasterization can
vary between operating systems, so compare the fixed geometric markers and
content regions when captures come from different systems. Use `hud=0` for
screenshot or pixel capture. `duration=0` is equivalent to `checkpoint=1`.

The individual workloads are useful when isolating a regression:

```
workload.html?mode=one&duration=20&seed=7
workload.html?mode=sparse&duration=20&seed=7
workload.html?mode=cross&duration=20&seed=7
```

The query parameters are:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `mode` | `cycle` | `one`, `sparse`, `cross`, or all three as `cycle` |
| `duration` | `30` | Animated seconds; `0` ends at the checkpoint |
| `seed` | `20260919` | Deterministic content seed |
| `width`, `height` | `1920`, `1080` | Canvas pixel dimensions, useful for a negotiated desktop size |
| `fps` | `60` | Logical animation frame rate (the browser still schedules actual paints) |
| `phase` | `duration / 3` | Seconds per cycle phase |
| `checkpoint` | off | Render the final static frame immediately |
| `hud` | on | Set `hud=0` to hide status text |

For an RDP test, open the page in the remote browser, let the animated run
finish, and compare the checkpoint with a local browser run using the same URL.
For a sequence test, capture the three mode URLs separately so a failure can
be tied to a sparse update, a single moving region, or a rectangle crossing a
monitor seam. The page's frame counter and elapsed time are workload metrics;
they do not measure input latency or network throughput.

The page redraws its canvas and does not control the rectangles Windows sends
over RDP. In particular, sparse canvas tiles do not guarantee sparse RDP updates.
Record the negotiated codec and client upload counters when comparing runs.

## Physical multi-monitor baseline comparison

The repository-root launchers use the same `free-rdp-debug.py` and
`free-rdp-multi-screen.sh` settings, including AVC444, monitor order/scaling,
logging, and packet capture:

```sh
./run-with-counters.sh           # optimized build/video-responsive client
./run-with-counters-baseline.sh  # counters-only worktree client
```

Run one client at a time. Close the current client before switching. The
baseline is commit `f6d34cd3fd034e0803c9089f5dbcb9ffdcc4eaa8`: it includes the
input-scheduling fix and counters, but precedes both per-monitor clipping and
upload batching. Its separate worktree is prepared locally at
`/Users/david/.codex/worktrees/render-counters-baseline/FreeRDP`, with the binary
under `build/counters/client/SDL/SDL3/sdl-freerdp`. Set
`FREERDP_BASELINE_WORKTREE` if that checkout moves. The launcher checks the
checkout commit and selects its binary explicitly, overriding `FREERDP_BIN`.
It expects the baseline to have been built already.

Each launcher prints the metrics path and analyzer command. Baseline captures
use `diagnostics/render-baseline-<timestamp>-<pid>/`; `baseline.txt` records
the source commit and binary SHA-256. The nested `session.txt` revision belongs
to the shared launcher repository, not the baseline source checkout.

For a useful comparison, wait until compilation finishes, then record separate
optimized/baseline/optimized sessions with the same 60–90-second video segment,
playback size, quality, and monitor placement. Start with video on M27UP while
the Dell remains static. Keep startup and idle time similar, and repeat with
video on the Dell or across the monitor boundary as separate runs. Do not
mix several workloads into one aggregate. Supply the two actual metrics paths:

```sh
python3 tools/render-benchmark/analyze.py \
  /path/to/optimized/render.jsonl /path/to/baseline/render.jsonl \
  --warmup-intervals 2
```

Compare uploaded bytes per second for both monitors and whole-redraw p50/p95/p99.
The optimized attempted-pixel count is taken after clipping, so equality with
uploaded pixels does not measure clipping savings. Do not compare
`average_draw_wall_ms` directly: the baseline timer includes per-rectangle
target setup while the optimized version sets the target once outside that
timer. Present intervals include idle periods and are not a video-FPS counter.
Windows may send different dirty rectangles in each live run; these comparisons
are observational, not identical-input replays.
