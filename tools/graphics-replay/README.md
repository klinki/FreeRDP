# Offline graphics replay from packet captures

This tool extracts the **existing captured server graphics stream**, rather
than requiring a new desktop recording. Extraction opens no network socket.
The output contains the captured desktop's compressed graphics data; keep it
with the private, ignored diagnostics, not in Git.

## Extract

Requires Python 3, `tshark`, and OpenSSL `libcrypto`. On this Mac:

```sh
python3 tools/graphics-replay/extract.py /path/to/capture.pcapng \
  --keys /path/to/tls-secrets.txt --output /path/to/private/session.gfx \
  --libcrypto /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib
```

Use one complete session including the UDP ClientHello, ServerHello, and
graphics-channel creation. For a capture split across ring files, merge the
consecutive files first with Wireshark's `mergecap`. Files overwritten by the
ring cannot be recovered. This initial extractor supports one RDPEUDP2 tunnel,
TLS 1.3 AES-GCM, and ordinary DVC DATA_FIRST/DATA framing; it fails on unsupported
framing or incomplete input rather than silently dropping graphics messages.
It does not merge graphics that migrate back and forth between TCP and UDP.

The extractor deduplicates retransmissions, restores channel ordering,
authenticates every encrypted server TLS record, reconstructs tunnel and DVC
messages across fragments, and retains only the Graphics channel. An absent
channel sequence zero at a wrap is provisionally omitted only when all
remaining TLS authentication succeeds. Other gaps and conflicting retransmits
are errors. Cursor, audio, input and clipboard channels are excluded. Output
files use restrictive permissions; neither keys nor desktop payloads are logged.

`session.gfx.json` records hashes, transport validation counts, message counts,
and duration. The replay format is eight bytes `FRGFXR01`, then repeated
little-endian `uint64 timestamp_us`, `uint32 length`, and `length` bytes containing
one complete, still-ZGFX-compressed graphics message. Timestamps are relative
to the first graphics message, based on capture arrival and ordered delivery.
They are not timestamps of the original client's decoder or UI thread.

The 2026-09-20 baseline capture has been extracted into the ignored directory
`diagnostics/graphics-replay-20260920/`: 50,976 unique UDP chunks, 21 identical
retransmissions, no channel gaps, 40,049 authenticated encrypted TLS records,
and 5,192 complete graphics messages spanning 133.470 seconds.

## Extractor checks

```sh
REPLAY_LIBCRYPTO=/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib \
  python3 tools/graphics-replay/test_extract.py
```

The tests exercise reordering, duplicates, missing data, wrap labels, fragment
boundaries, channel-ID reuse, truncated input, and authenticated-decryption
failure using a public AES-GCM test vector. They contain no capture data.

## Build the production-code runner

The runner links the **existing compiled SDL client objects** and their matching
FreeRDP libraries. It replaces only the executable's entry point. A generated
header overlay adds friend declarations for offline setup and texture readback;
it does not change production objects or the running client. The build adapter
currently requires an SDL3 build generated with CMake's Unix Makefiles generator.
Rebuild the client first after changing its implementation, then relink the runner.
Keep the source tree and compiled build consistent; this adapter does not rebuild
the client or certify that existing objects match the checkout revision.

```sh
python3 tools/graphics-replay/build.py --source . \
  --build build/video-responsive --output build/graphics-replay/optimized
```

The adjacent `.build.json` records the harness, executable, production-object,
and directly linked library hashes. Its source revision describes the checkout, which
may be newer than the existing compiled objects. This is especially useful when
comparing the old counters-only worktree against a newer renderer.

The runner initializes the actual FreeRDP rdpgfx plugin, ZGFX decompressor,
AVC444 decoder, GDI surfaces and production `SdlContext::drawToWindows` path.
The only substituted channel behavior is an offline DVC endpoint: server data
comes from the replay file and local replies are discarded. No RDP connection,
authentication, remote input, or network packet injection occurs.

Recorded ResetGraphics messages establish the monitor sizes and offsets.
Windows remain hidden. `--renderer metal` uses the native GPU rendering path;
`--renderer software` uses SDL's dummy video driver and software renderer.
**Both use the existing build's decoder**, including VideoToolbox when enabled.
On macOS, an agent sandbox may block VideoToolbox buffer allocation; normal
Terminal execution or approved local GPU access is needed. Decode errors or
ignored codec updates invalidate the run instead of producing misleading metrics.

## Correctness replay

```sh
mkdir -p diagnostics/my-replay
build/graphics-replay/optimized \
  --input diagnostics/graphics-replay-20260920/baseline.gfx \
  --renderer metal --verify-every 25 \
  --metrics diagnostics/my-replay/verify.jsonl \
  --save-final diagnostics/my-replay/final \
  > diagnostics/my-replay/verify.log 2>&1
```

Every 25 decoded paints, read back both production render targets and compare
every RGB pixel against the corresponding crop of the decoded desktop.
Alpha is excluded because it is not part of the desktop image comparison.
The final image is also checked. Use `--verify-every 1` to check every paint,
or `--max-messages 100` for a quick smoke test. `--save-final` is optional and
writes private BMP files; an error saves a mismatch image when this option is set.
Metrics filenames must be new. Recordings, keys and decoded images stay outside Git.

Run this separately for both builds. Verification adds CPU work and GPU readback
synchronization, so **do not use correctness-run timing as benchmark results**.
The JSON result flags verification runs explicitly. Corrupt/truncated replay
inputs can be checked without private data:

```sh
python3 tools/graphics-replay/test_replay.py
```

## Repeatable A/B measurement

For the current workspace and saved baseline capture:

```sh
./run-graphics-replay.sh
```

The comparison script requires Python 3.11 or newer.

This links separate runners for the current `build/video-responsive` build and
the counters-only baseline worktree, then runs two sequential trials per build
with alternating order. It measures captured seconds 20–80, decoding the prefix
first to reconstruct all H.264 references and graphics surfaces. Timed runs
disable pixel verification and image saving. The script prints its private
results directory, individual progress, and a comparison table. It writes
`comparison.md`, `results.json`, complete logs, and per-monitor JSONL counters.

Useful overrides:

```sh
./run-graphics-replay.sh --repeats 3 --start-seconds 20 --stop-seconds 80
REPLAY_INPUT=/absolute/path/session.gfx ./run-graphics-replay.sh --renderer software
```

Environment overrides: `REPLAY_BASELINE_SOURCE`, `REPLAY_BASELINE_BUILD`,
`REPLAY_CURRENT_BUILD`, `REPLAY_INPUT`, and `REPLAY_OUTPUT` (must be a new directory).
For other executable pairs, invoke `compare.py --help`. The comparison refuses
runs with differing message counts, graphics-command counts, paint counts,
recorded intervals or renderers. It records binary/replay hashes and rejects
files changed during the comparison. Monotonic measurement boundaries select
complete counter intervals after warmup. Upload totals are bytes passed to SDL
texture updates, **not network traffic**.

## What these results can tell us

The same captured compressed graphics and changed rectangles reach both builds.
This supports reproducible clipping, texture upload, batching, pixel correctness,
decode cost and rendering comparisons without asking the server to reproduce
mouse movements, typing, or video frames.

Painting is synchronous at each GDI paint boundary. This deliberately excludes
the live UI event queue, its redraw coalescing, concurrent input handling,
reconnects, and network timing. Hidden Metal windows do not measure onscreen
presentation latency or display refresh behavior. Wall timers measure API-call
duration, not GPU execution time; process CPU excludes VideoToolbox helper-process
and GPU work. Treat timing differences as this replay's results, not a promised
FPS or input-latency improvement. Other activity on the Mac can affect them.

`--paced` on an individual runner preserves capture delivery spacing for timing
experiments, but still cannot reconstruct the original decoder/UI scheduling.
The default runs as quickly as decoding and rendering allow. AVC444 remains
unchanged; this harness exercises the recorded codec, not a codec substitute.
