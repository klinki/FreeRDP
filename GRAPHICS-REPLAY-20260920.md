# Graphics replay from the existing packet capture — 2026-09-20

The saved baseline packet recording can be used for a real offline graphics
replay. It now runs through FreeRDP's production graphics decoder and SDL
per-monitor rendering code. No new server recording is required.

## Recording and transport validation

Source: `diagnostics/render-baseline-20260920-162716-59611/20260920-162716-59611/capture_00001_20260920162716.pcapng`,
with the matching private TLS key log beside it.

| Property | Result |
|---|---:|
| Unique server UDP channel chunks | 50,976 |
| Identical retransmissions removed | 21 |
| Missing channel labels | 0 |
| Authenticated encrypted TLS records | 40,049 |
| Complete graphics messages | 5,192 |
| Compressed graphics payload | 57,770,038 bytes |
| Recorded graphics span | 133.470230 seconds |

Capture SHA-256:
`5d877095c300c46629324eaf2efa8559cdea5cf117083f3fe7b42e43bccb4244`.

Extracted replay SHA-256:
`42a6db71cb0541d866e3809bad4348c8cb0ba0d67939158a3239c1e31eac5345`.

The extractor orders and deduplicates UDP chunks, authenticates/decrypts TLS,
reassembles tunnel and dynamic-channel fragments, and retains only complete
Graphics-channel messages. Cursor, audio, input and clipboard channels are
excluded. The resulting file still contains the original compressed ZGFX/AVC444
graphics, rather than screenshots or synthetic rectangles.

Private recording, images, build manifests, detailed logs and counter files:
`diagnostics/graphics-replay-20260920/`. This directory is ignored by Git.

## Correctness verification

Both runners use the same harness and link their respective existing production
SDL objects and FreeRDP libraries. The baseline is the counters-only build at
`f6d34cd3f`; the optimized SDL code adds `dc9f13f80` (per-monitor clipping) and
`e297cbd0f` (uploads before draws). Both already include input scheduling changes.
The tests do not change or reconnect the live RDP session.

Both builds completed the full recording with native Metal rendering and
VideoToolbox decoding:

| Check | Baseline | Optimized |
|---|---:|---:|
| Complete input messages | 5,192 | 5,192 |
| AVC444v2 surface updates / decoded paints | 4,965 | 4,965 |
| Protocol frames | 4,300 | 4,300 |
| Decoder errors or ignored codec updates | 0 | 0 |
| Full-image verification checkpoints | 199 | 199 |
| Pixel mismatches | 0 | 0 |

Each checkpoint compares every RGB pixel of both rendered monitor targets
against their corresponding crops of the decoded GDI framebuffer. Alpha is
excluded. Checks occur every 25 decoded paints and at the end, so this is
sampled temporal verification, not a claim that every intermediate paint was
read back. Both monitor sizes and offsets come from the captured ResetGraphics:

- Replay monitor 1 (M27UP): 3840×2160, desktop offset (1920, 0).
- Replay monitor 2 (Dell): 1920×1080, desktop offset (0, 1080).

These replay monitor numbers are synthetic identifiers; they differ from the
physical SDL display IDs used by the live launcher.

Final BMPs are byte-identical across builds, with these SHA-256 hashes:

- Monitor 1: `98c739fcb0a146d3547de1853ed0422141ff9b54c518269d41e5ffea93766f28`.
- Monitor 2: `9502ae0308ab6ce5ab3c78baed6b1d57596d3234bc91069833a44115611e4185`.

The final main-monitor image was also inspected visually and contains the
captured browser/video desktop. A short software-renderer replay passed the
same pixel checks on both builds.

The initial sandboxed VideoToolbox attempt could not allocate video buffers.
After granting the offline process local GPU access, decoding succeeded. The
runner explicitly rejects decoder errors, ignored updates, and recordings that
produce no measured paints, avoiding a false pass from a black framebuffer.

## Measurement scope

Performance trials are separate from correctness runs: texture readback and
image saving are disabled. Recorded seconds 20–80 are measured, while the
prefix is decoded first to reconstruct codec references, surfaces and textures.
Two trials per build run sequentially in baseline/optimized/optimized/baseline
order. Both receive identical compressed data and changed rectangles.

## Identical-input results

Every trial measured 2,487 complete graphics messages and 2,351 decoded paints.
Graphics-command counts, paint counts, input hashes and interval boundaries
matched. Upload totals were exactly repeatable within each build.

| Measurement, median across two trials | Baseline | Optimized | Reduction |
|---|---:|---:|---:|
| Decode + rendering wall time | 55.178 s | 49.283 s | 10.7% |
| Replay process CPU time | 66.563 s | 54.453 s | 18.2% |
| Rendering wall time, both monitors | 24.789 s | 13.899 s | 43.9% |
| Rendering p95 per paint, both monitors | 20.968 ms | 13.382 ms | 36.2% |
| Total local texture upload | 66.600 GB | 33.300 GB | 50.0% |
| M27UP local texture upload | 33.300 GB | 31.657 GB | 4.9% |
| Dell local texture upload | 33.300 GB | 1.643 GB | 95.1% |

GB values use decimal units. Raw rendering wall times were 26.088/23.489 seconds
for baseline and 13.733/14.066 seconds for optimized. Two trials establish an
initial comparison, not a statistical confidence interval. The p95 metric here
times a complete paint across both monitors; it is not directly interchangeable
with the live report's separate per-window redraw percentiles.

The earlier manual comparison suggested about 96% less upload volume on the
Dell. With identical recorded input, the corresponding reduction is 95.1%, and
the total reduction is exactly 50%. This confirms that the mostly static monitor
benefits substantially from clipping. The active video monitor still receives
most of its own updates. Both builds decode the same AVC444v2 data; the rendering
savings therefore produce a smaller reduction in overall replay CPU and elapsed
decode/render time. This comparison measures clipping and batching together and
does not separate their individual contributions.

Detailed machine-readable results and executable/object/library hashes are in
`diagnostics/graphics-replay-20260920/ab-metal/results.json`; its adjacent
`comparison.md` is the automatically generated table. The two executable
manifests record identical harness source hash
`740e417d3aec46e9308bc78e2ba21fa4ce6b818356ab2bf87cff81635c8ec04c`
and different production renderer-object hashes, confirming the intended A/B
pair was used.

## Interpretation limits

Rendering wall time covers the production `drawToWindows` call over both
monitors. Process CPU covers the replay process during the selected interval;
it excludes GPU execution and VideoToolbox helper processes. Upload bytes are
local SDL texture updates, not network data. CPU and wall durations are distinct.

The runner paints synchronously at GDI paint boundaries. It does not reproduce
the live UI queue's redraw coalescing, input contention, network behavior,
connection recovery, or visible display refresh. Hidden Metal windows exercise
the renderer but do not establish onscreen presentation latency. These results
evaluate rendering changes; they cannot quantify the earlier scheduling fix's
effect on input responsiveness. Other Mac activity can affect timing.

## Running it again

```sh
./run-graphics-replay.sh
```

The launcher prints a new private results directory with `comparison.md`,
`results.json`, logs, and per-monitor counters. It reuses the extracted baseline
recording and links the existing compiled client objects. After changing
production code, rebuild the client first. Build manifests identify the actual
objects and directly linked libraries by hash; the checkout revision alone
does not prove which source revision produced an existing build.

See [the harness instructions](tools/graphics-replay/README.md) for extraction,
other builds/captures, full pixel checking and benchmark overrides.

Validation also includes eight extractor tests and four replay-input tests,
covering reordering, retransmissions, missing fragments, sequence wrapping,
AES-GCM authentication failure, corrupt replay headers, truncated payloads,
oversized messages and invalid timestamps/options. All passed.
