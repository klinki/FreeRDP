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
