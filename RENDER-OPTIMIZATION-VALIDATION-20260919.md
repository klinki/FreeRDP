# SDL rendering optimization validation — 2026-09-19

## Scope and separate changes

| Commit | Change |
| --- | --- |
| `f6d34cd3f` | Opt-in per-window render metrics, JSONL analyzer, and repeatable offline browser workload. |
| `dc9f13f80` | Clip desktop damage to each unscaled window's visible source area; skip unaffected windows; initialize/recreate render targets safely. |
| `e297cbd0f` | Upload all dirty texture regions before drawing; preserve draw order and validate scaled source rectangles before reading pixels. |

All implementation changes are in the SDL3 client. Codec selection, AVC444
reconstruction, network transport, and the previous input scheduling fix are
unchanged. Full repaints remain explicit for initialization, resize, expose,
and local UI redraw paths. An empty result after clipping is a no-op; an
explicit empty input to `drawRects` remains a full repaint request.

## Review and automated validation

Luna xhigh agents implemented instrumentation, rendering, and tests; the
orchestrator reviewed and integrated them. Review tightened framebuffer locking,
render-target initialization/failure handling, scaled source bounds, and tests
that could otherwise pass by reusing old texture pixels.

The Release SDL3 client builds in `build/video-responsive`. All six selected
CTest cases pass: `TestSDLRenderGeometry`, `TestSDLRenderWindow`,
`TestSDLRenderMetrics`, `TestSDLUpdateQueue`, `TestSDLInputMapping`, and
`TestSDLMonitorScale`.

Geometry tests use an independent pixel oracle with negative origins, monitor
edges, odd sizes, offscreen regions, empty/full semantics, and integer limits.
The real `SdlWindow` test uses SDL's dummy/software renderer and two synthetic
windows. It checks exact pixels after overlapping updates, full repaints,
resize and source-size replacement, forced full initialization from a tiny dirty
rectangle, fractional scaling, and offsource no-ops. Geometry also passed an
ASan/UBSan run. The CTest renderer is software, not Metal.

## Live VM setup

The Windows 11 VM is accessed using the existing saved test credential; no
credential is included in this report. The dedicated client bundle and raw
artifacts are retained under `diagnostics/render-optimization-20260919/` in the
working directory. This directory is ignored by Git.

- RDP desktop: 1600 × 900; Mac Retina window: 800 × 450 content points.
- Edge workload: 1600 × 800 canvas, `mode=cycle`, `duration=30`, `fps=30`,
  `seed=20260919`; one moving area, sparse tiles, and a large moving area.
- A frozen checkpoint makes grid lines, text, color bars, and stale tiles visible.
- Client binaries and their SHA-256 values are recorded in `*-session.json`.
- Metrics are explicitly enabled with `FREERDP_SDL_RENDER_METRICS`.
- Both AVC444 sessions initially logged the existing post-ACTIVE TCP safeguard,
  then logged UDP receive migration on the first DVC PDU. No packet-loss or
  transport recovery fault injection was performed.

Initially the VM selected ClearCodec/progressive despite the client's AVC444
advertisement. For the AVC444 tests, `AVC444ModePreferred=1` was temporarily set
on the VM after saving its previous state (the value was absent). The trace then
confirmed `RDPGFX_CODECID_AVC444v2 [15]`, and the client reported VideoToolbox H.264
decoding. This is the documented Windows [AVC444 preference policy](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-admx-terminalserver#ts_server_avc444_mode_preferred).

## Measurement limits

Only one physical Mac display was connected. The live VM checks exercise native
rendering and AVC444, while synthetic tests cover multiple window offsets and
clipping. A physical mixed-DPI multi-monitor session remains to be checked.
There is no automated full `SdlContext` multi-window fan-out test.

The workload changes canvas pixels, but does not control the rectangles Windows
sends. It redraws the canvas, so “sparse tiles” does not guarantee sparse RDP
updates. Server encoding and dirty-region choices vary between runs. Reported
upload bytes are SDL texture submissions, not network bytes. Redraw timing is
wall time, not decode time or input latency; redraw counts are not video FPS.

Each local observation lasts about 35 seconds around a 30-second run. The first
emitted metrics interval is omitted in comparisons. These short runs are
exploratory rather than a statistically controlled benchmark. Whole-process CPU
includes decoding, rendering, and other client threads.

## Observed results

### Native AVC444 VM workload

| Metric | Instrumented baseline | Clipping + batching |
| --- | ---: | ---: |
| Retained active metric intervals | 29 | 26 |
| Retained interval duration | 29.559 s | 26.374 s |
| Redraw p50 | 1.624 ms | 1.309 ms |
| Redraw p95 | 3.204 ms | 2.134 ms |
| Redraw p99 | 4.008 ms | 2.599 ms |
| Average upload-call wall time | 0.081 ms | 0.055 ms |
| Present-interval p50 | 31.285 ms | 31.315 ms |
| Present-interval p95 | 63.649 ms | 63.262 ms |
| Submitted texture bytes/second | 26.24 MB/s | 27.33 MB/s |
| Whole-process CPU over ~35 s observation | 7.24 CPU s | 5.82 CPU s |

The renderer spent less time per redraw in this sample, while presentation
cadence stayed similar. That supports a reduction in client rendering overhead;
it does not establish higher video FPS. The captures retained different active
durations and server dirty-region sequences, so the CPU totals and percentile
differences are not a controlled effect-size estimate. The one-display run is
primarily evidence for batching, not multi-monitor upload savings.

The `draw_wall_ns` scopes also differ: baseline includes target setup for each
rectangle; batching sets the target once before timing individual texture draws.
Do not compare that sub-counter directly. Whole-redraw timing includes both.

### Deterministic two-window software-renderer workload

Both builds received the same 64 left-side rectangles for 120 frames from a
1600 × 900 framebuffer. Each synthetic window was 800 × 900; the right window
had source offset (-800, 0). Warmup uploads were excluded.

| Metric | Instrumented baseline | Clipping + batching |
| --- | ---: | ---: |
| Left-window uploaded bytes | 344,064,000 | 344,064,000 |
| Right-window uploaded bytes | 344,064,000 | 0 |
| Total uploaded bytes | 688,128,000 | 344,064,000 |
| Total upload calls | 15,360 | 7,680 |
| Total test wall duration | 177.850 ms | 116.776 ms |

This confirms **50% fewer texture-upload bytes** for this two-window layout,
with no uploads to the unaffected window. It calls `SdlWindow` directly and
still presents both windows, so it does not measure `SdlContext`'s skipped
presentation optimization. Its software-renderer timing is not a Metal result.

### Live behavior and cleanup

The AVC444 animation and final checkpoint displayed correctly. Keyboard
shortcuts and deliberate keystrokes worked; a saved disposable VM document was
read back as `ok`. Rapid synthetic text entry and focus selection were
unreliable through the automation tool, so this is not a comprehensive input
stress test. A two-second process sample during the check showed the main thread
waiting in `SDL_WaitEventTimeoutNS`, without a renderer busy loop.

Smart sizing was exercised in a separate session. The native window was shrunk
from roughly 800 × 450 content points to 650 × 370 and restored, with the remote
desktop repainting at both sizes. This complements the automated resize and
texture-recreation pixel checks.

The VM's original AVC444 policy state was restored and verified absent. The
manual test task was removed and verified absent. The test client was closed.
The ordinary installed client binary was not replaced. Raw logs, the brief
sample, benchmark source, binaries, and comparison JSON remain in the ignored
project diagnostics directory; the benchmark page and disposable input document
remain in the VM's dedicated test folder.

## Reproduction and upstream extraction

Use `tools/render-benchmark/workload.html` and
`tools/render-benchmark/analyze.py --warmup-intervals 1 <capture.jsonl>`.
Counter definitions are in `tools/render-benchmark/METRICS.md`.
The ignored `diagnostics/render-optimization-20260919/MICROBENCH.md` records the
standalone two-window build and run instructions.

The instrumentation commit is independently useful. The clipping commit adds
shared geometry and lifecycle tests; batching builds on that clipping/lifecycle
work and extends the scaled pixel tests. Extract them in the table's order,
then review dependencies on this fork's existing SDL multi-window renderer.
