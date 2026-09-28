# Physical multi-monitor rendering comparison — 2026-09-20

The two optimized runs show substantially less total rendering work than the
counters-only baseline. In a common elapsed-time window, the latest optimized run
submitted **41.9% fewer texture bytes** and spent **44.2% less wall time inside
redraws** across both windows. The first optimized run showed reductions of
50.0% and 54.4%, respectively. The clearest benefit is avoiding duplicate work
on the Dell; the M27UP video window's median redraw duration stays around
4.6–4.9 ms.

These are observations from live sessions, not a controlled replay of identical
RDP updates. They establish neither a process CPU reduction of the same size
nor an increase in video FPS. The comparison measures clipping and batching
together and does not isolate the benefit of batching. Playback, pauses and
typing were manually repeated without matching their timing across runs, so
the percentages cannot be attributed entirely to the code changes.

## Recordings and snapshot

Measurements were frozen at **2026-09-20 16:32:44 Europe/Prague**, while the
third session continued running. Its later data is not included here. The
snapshot copies only newline-terminated metrics records; all three files had
zero incomplete trailing bytes and zero parse errors.

| Run | Source directory under `diagnostics/` | Launcher start / stop | Records |
| --- | --- | --- | ---: |
| Initial optimized | `render-20260920-161040-52688` | 16:10:40 / 16:14:07 | 347 |
| Baseline | `render-baseline-20260920-162716-59611` | 16:27:16 / 16:29:35 | 258 |
| Latest optimized | `render-20260920-162936-59749` | 16:29:36 / still running at snapshot | 300 |

The baseline worktree is at `f6d34cd3fd034e0803c9089f5dbcb9ffdcc4eaa8`,
including counters and the earlier input-scheduling fix, before per-monitor
clipping (`dc9f13f80`) and upload batching (`e297cbd0f`). The baseline binary
SHA-256 recorded by its launcher is
`dd1d4f684359db09f902cd45eebc2e79a39d8a6b54f5a3013c945644c48113fa`.
Both optimized sessions used `build/video-responsive/client/SDL/SDL3/sdl-freerdp`.
The baseline used its worktree's `build/counters` binary and libraries. Build
settings were matched when preparing the baseline. The shared launcher's Git
revision in the baseline `session.txt` is not the baseline source revision.

The user confirmed that the baseline and latest optimized runs used the same
video and size on M27UP, keeping the Dell mostly static. They also clarified
that each recording mixed playback, typing during playback, pausing, and typing
while paused. Similar actions were repeated manually; neither their timing nor
the exact video position was synchronized. These are useful interaction trials,
but equal recording lengths do not make their workloads identical. Logs confirm
the same desktop geometry and scaling in all three sessions:

- Monitor 3: M27UP, 3840 × 2160 remote pixels, desktop scale 175%.
- Monitor 2: Dell U2419HC, 1920 × 1080, desktop scale 100%.
- Combined desktop: 5760 × 2160; the Dell is to the left, bottom aligned.
- VideoToolbox accelerated H.264 decoding initialized in all three.
- All three migrated UDP reception on the first DVC PDU after the initial
  post-ACTIVE TCP safeguard.

The same launcher requests AVC444 in each run. These client logs do not contain
per-frame codec IDs, so this check confirms the configured request and H.264
decoder, not independently the negotiated AVC444 variant from packet contents.

## Common elapsed-time window

The latest run's M27UP upload rate falls from hundreds of MB/s to approximately
32 MB/s around 90 seconds after its first metrics interval. Whole-session
averages therefore mix different proportions of activity. To reduce this
effect, the principal comparison selects complete intervals contained within
**20–80 seconds after each run's first interval starts**. This excludes startup
and the later low-activity period. The selection was made after examining the
time series; it is an exploratory common elapsed-time window, not synchronized
video frames or a marked playback interval. It does not guarantee uninterrupted
playback or equivalent amounts of typing within the selected window.

Actual retained durations are approximately 59 seconds per window, because
intervals crossing either boundary are excluded. Rates use each window's
retained duration; combined rates add those two per-window rates.

| Measurement | Initial optimized | Baseline | Latest optimized |
| --- | ---: | ---: | ---: |
| M27UP retained duration | 59.382 s | 58.726 s | 59.657 s |
| Dell retained duration | 58.680 s | 58.733 s | 59.200 s |
| M27UP texture uploads | 544.1 MB/s | 552.1 MB/s | 619.6 MB/s |
| Dell texture uploads | 7.5 MB/s | 552.1 MB/s | 22.1 MB/s |
| **Combined texture uploads** | **551.6 MB/s** | **1104.2 MB/s** | **641.8 MB/s** |
| **Combined redraw wall time per second** | **131.1 ms/s** | **287.3 ms/s** | **160.2 ms/s** |
| M27UP redraw p50 | 4.830 ms | 4.571 ms | 4.931 ms |
| M27UP redraw p95 | 8.058 ms | 8.527 ms | 8.114 ms |
| M27UP redraw p99 | 9.403 ms | 9.981 ms | 9.224 ms |
| Dell redraw p50 | 0.557 ms | 4.864 ms | 0.786 ms |
| Dell redraw p95 | 2.654 ms | 8.326 ms | 2.702 ms |
| Dell redraw p99 | 3.154 ms | 9.668 ms | 2.875 ms |
| M27UP redraw attempts | 1608 | 2128 | 1953 |
| Dell redraw attempts | 178 | 2128 | 426 |

MB means 1,000,000 bytes. These bytes are local SDL texture submissions, not
network traffic or allocated memory. Redraw wall time is the sum of the recorded
redraw durations divided by observation duration, summed across the two
windows. Every redraw-duration sample was retained in these selected periods,
so no reservoir expansion or percentile averaging is used for those totals.
It includes time waiting inside rendering calls; it is not CPU time. It also
does not include the entire network/decode/input pipeline.

### Interpretation

1. **The baseline duplicates pixel uploads across monitors.** It submits about
   552 MB/s to each window and redraws both 2128 times in the selected period.
   That agrees with the old implementation sending the desktop's dirty regions
   to both windows even when they are outside one window's visible area.
2. **Clipping removes most of the Dell's unnecessary work.** Latest-run Dell
   traffic is 96.0% lower than baseline. Its redraw time falls from 142.7 to
   8.0 ms per observed second. The first optimized run shows the same pattern.
3. **The main video window is not dramatically faster per redraw.** Its median
   is slightly higher in both optimized runs, while p95/p99 are slightly lower
   in this selected period. The overall saving is principally less work for
   the second window, not a large reduction in each M27UP redraw.
4. **The workloads are not identical at the renderer boundary.** Latest-run
   M27UP uploads are 12.2% higher than baseline despite the same user setup.
   Server dirty regions, coalescing and timing can differ. Total uploads still
   fall 41.9%. The repeated optimized observations support the direction of
   the benefit, but these percentages are observed effect sizes, not universal
   performance guarantees.
5. **This does not establish higher video FPS.** Present intervals include idle
   time, and redraw attempts are not distinct decoded video frames. We also
   did not measure end-to-end input latency or whole-process CPU in these runs.

## Whole-recording summaries

For transparency, these are the ordinary analyzer results after removing the
first two emitted intervals independently for each window. They include idle,
interaction, and playback periods and should not be treated as the primary
estimate of optimization benefit.

| Measurement | Initial optimized | Baseline | Latest optimized snapshot |
| --- | ---: | ---: | ---: |
| M27UP observed duration | 192.616 s | 131.247 s | 169.853 s |
| Dell observed duration | 193.209 s | 131.286 s | 172.187 s |
| M27UP uploads | 442.1 MB/s | 531.6 MB/s | 333.8 MB/s |
| Dell uploads | 16.9 MB/s | 532.0 MB/s | 18.9 MB/s |
| Combined uploads | 459.1 MB/s | 1063.6 MB/s | 352.6 MB/s |
| Combined redraw wall time | 117.4 ms/s | 276.4 ms/s | 98.2 ms/s |
| M27UP redraw p50 / p95 / p99 | 4.837 / 8.943 / 12.740 ms | 4.478 / 8.806 / 10.432 ms | 4.637 / 9.366 / 14.715 ms |
| Dell redraw p50 / p95 / p99 | 0.725 / 3.242 / 4.557 ms | 4.751 / 8.683 / 10.689 ms | 0.915 / 3.190 / 4.640 ms |

The full-recording M27UP tails are worse in the optimized runs, unlike the
selected active period. That is a reason to avoid claiming a general latency
or smoothness improvement from these aggregates.

`attempted_dirty_pixels == uploaded_pixels` does not mean clipping saved
nothing: the optimized attempted-pixel counter is recorded after clipping.
Do not compare `average_draw_wall_ms` directly: baseline times per-rectangle
target setup inside that scope, while batching moves target setup outside it.

## Logs

No runtime renderer failure, UDP buffer overflow, watchdog-triggered failure,
or unexpected reconnect was found in the saved log snapshots. The initial run
logged one handled skipped channel-sequence zero at 16:12:50, then an explicit
user cancellation at 16:14:07. An SDL `handleShow` error appears during that
shutdown, not during the selected measurement period. Baseline and latest
snapshots contain the familiar startup warnings but no corresponding runtime
transport failure. Log inspection is not proof that no brief visible stutter
occurred.

## Preserved evidence and reproduction

The ignored, private directory
`diagnostics/render-comparison-20260920-163244/` contains frozen JSONL files,
client logs, session metadata, `manifest.json` with metrics hashes,
`comparison.json`, and `reproduce.py`. No TLS secret file was copied into this
comparison directory. Original packet captures and TLS secrets remain with
their respective original recordings.

```sh
python3 diagnostics/render-comparison-20260920-163244/reproduce.py
python3 tools/render-benchmark/analyze.py \
  diagnostics/render-comparison-20260920-163244/optimized-initial.jsonl \
  diagnostics/render-comparison-20260920-163244/baseline.jsonl \
  diagnostics/render-comparison-20260920-163244/optimized-current.jsonl \
  --warmup-intervals 2
```

The current session was left running. No thread dump, fault injection, remote
interaction, or packet decryption was needed for this comparison.
