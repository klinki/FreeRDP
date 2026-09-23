# SDL render metrics

Set `FREERDP_SDL_RENDER_METRICS` to a writable JSONL path before starting the
SDL3 client to enable the recorder:

```sh
FREERDP_SDL_RENDER_METRICS=/tmp/freerdp-render.jsonl \
  ./client/freerdp-client --version
```

The path is diagnostic output only. An unset, empty, or unopenable path leaves
the client unchanged. The recorder is owned by the UI/render thread and writes
one bounded JSON object per window approximately once per second. It does not
write one line per dirty rectangle. A line is safe to append to a shared path
when several monitor windows are active.

The records use schema `freerdp.sdl_render_metrics`, version `1`, and contain
numeric `window_id` and `monitor_id` values. `attempted_dirty_pixels` is the
sum of the source areas requested for redraw attempts; it retains rectangle
multiplicity and is not a union-area calculation. `uploaded_pixels` is the sum
of the areas submitted to texture upload calls, and `uploaded_bytes` is the
actual byte count supplied for those calls. These values let a run compare
source work with clipped upload work without inferring either from a redraw
count.

`upload_wall_ns`, `draw_wall_ns`, and `present_wall_ns` are summed elapsed
durations. They are measured with a monotonic wall clock and are not CPU time.
`redraw_wall_ns_samples` contains bounded reservoir samples of complete redraw
durations. `frame_interval_wall_ns_samples` contains bounded samples of the
time between completed presents; the first present has no interval sample.
`*_sample_count` is the number seen before reservoir sampling, while the JSON
array is the retained sample set. The recorder keeps at most 256 samples per
interval.

Use the analyzer to compare runs. Warmup intervals are removed independently
for each window/monitor group, and percentiles are calculated over the
combined retained samples rather than by averaging interval percentiles:

```sh
python3 tools/render-benchmark/analyze.py baseline.jsonl candidate.jsonl \
  --warmup-intervals 2
python3 tools/render-benchmark/analyze.py candidate.jsonl \
  --warmup-intervals 2 --format json
```

The analyzer reports uploaded bytes per second and p50/p95/p99 wall-duration
percentiles. Since each interval has a bounded reservoir, those percentiles
are approximate for long runs. Redraw counts are reported as counts and are
never presented as a frame-rate measurement.

## Schema version 2 (render records)

Version 1 keys are unchanged. Version 2 adds per-window counters, all summed
by the analyzer:

* `present_skips`: windows skipped because clipping removed all damage
  (per-monitor clipping working as intended when the Dell skips video damage).
* `target_recreates` / `gdi_recreates`: texture recreations (resize/monitor
  churn shows up here, not in upload bytes).
* `topbar_draws`: overlay draws composited inside presents.
* `stalled_presents`: reconnecting-overlay presents, excluded from
  `present_calls` so identical-input A/B comparisons of `present_calls`
  stay stable. Total onscreen presents are `present_calls + stalled_presents`.

## Queue records (`freerdp.sdl_queue_metrics`, version 1)

Process-global (window 0, monitor 0), written about once per second while the
event queue is active: `pushes`, `attempted_rects`, `merged_rects`,
`collapsed_events`, `pops`, `empty_pops`, `pop_rects`, `queue_wait_ns`
(push-to-pop delay; average wait is `queue_wait_ns / pops`),
`update_events_received`, `update_events_acted` (received but empty means a
dialog consumed the wakeup), and `motions_coalesced`.

`yuv_tiles`, `yuv_work_created` and `yuv_work_reused` are process-wide YUV
threadpool deltas polled from the codec layer. Steady video should show
`yuv_work_created` near zero after warmup (slots reused across frames);
a recreate storm points at callback/binding churn or repeated
`yuv_context_reset`.

These make input-scheduling claims falsifiable: one snapshot per update shows
up as acted ≈ received with low average wait even under video load; motion
coalescing shows up as `motions_coalesced` without lost damage. Offline
graphics replay drives rendering without the event queue, so replay files
normally contain no queue records — use live `run-with-counters.sh` sessions
for responsiveness and replay A/B for decode/render throughput. The
comparison tool ignores non-render records.
