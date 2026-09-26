# Review reproductions: master~1 to feat/performance-macos-native

Reviewed 2026-09-26. Base: `cf09c1aece03019273be96635983e6f036dcf58c`
(`master~1`). Target: `8d91ba1bdf736a0c17c030926cb617a1ea6778ca`.
The range contains 84 commits, including master's per-monitor scale override commit.

These exploratory harnesses are preserved for conversion into regression tests.
The correctness cases now assert the fixed behavior and fail on regressions;
timings and metrics analysis remain informational. The synthetic harnesses do not
connect to an RDP server, use private packet captures, or change production source
files. Capture-dependent follow-up tools are documented separately below. The
original observation-only versions remain in commit `e6fcc79df`.

See [FIXES.md](FIXES.md) for issue commits, current behavior, and validation.
The [office capture follow-up](OFFICE-CAPTURE.md) documents the actual six-wrap
failure and its fix, with a retained native replay and TLS-authentication script.
See [PERFORMANCE.md](PERFORMANCE.md) for fix overhead, the retained UDP receiver
benchmark, and its measured results.

## Run

Requires macOS, Python 3, clang, and an existing matching FreeRDP CMake **Unix
Makefiles** build with SDL3 and testing configured. The runner reads that build's
compiler/linker flags and reuses its libraries; it does not rebuild those libraries.
Rebuild the libraries after changing their source before comparing results.

From the repository containing this directory:

```sh
python3 -B tools/branch-review-20260926/run.py \
  --source "$(pwd)" \
  --build /path/to/matching/cmake-build
```

Use `--output /path/to/new/results` to keep separate runs. Default `results/`
retains binaries, build logs, output logs, and `commands.json`; it is gitignored.
The source tree may be a separate worktree. The six SDL tests are freshly compiled
from that source into the output directory. No source checkout is switched.

## Harnesses and observations

| Harness | Exercise | Observed at the reviewed target | Regression expectation |
|---|---|---|---|
| `udp_cases.c` | Genuine channel zero arrives after 251 ms or 64 later chunks at wrap | Missing `Z`; prefix `FAB` instead of `FZA`, one byte lost, transport still healthy | Preserve all ordered channel bytes; elapsed time or buffered count cannot prove a missing label is unused |
| `udp_cases.c` | Omitted channel zero after old recovered loss, then silence | No progress at 251 ms; reassembly watchdog fails at 10 s | Recover immediately when sequence accounting rules out zero; a separate fixture with a genuinely missing DataSeq still verifies the watchdog |
| `analyze_office_udp.py` | Replay all saved office UDP flows and authenticate their TLS streams | Baseline fix branch stalls on all six wraps | Native output must match all six authenticated streams without transport failure |
| `send_cases.c` | Deliver two four-byte chunks, lose ACK of second, then retry | Receiver gets `ABCDEFGHABCDEFGH` for one eight-byte write | Deliver each byte once, or fail the transport terminally; don't retry accepted ciphertext under fresh channel sequences |
| `send_cases.c` | Soft-Sync has CHANNELLIST set, UDP tunnel entry, zero DVC IDs | Unrelated DVC 42 routes over UDP | Keep an explicit empty list empty, or reject it; distinguish it from absent CHANNELLIST |
| `probe_case.cpp` | Mock SDL Windows/X11 bounds 3840x2160, display scale 2 | Probe reports 7680x4320 | Physical dimensions remain 3840x2160; separate pixel density from UI content scale |
| `yuv_wait_cases.c` | Decode a 16x16 region before and after a full 3840x2160 frame, default 512 tile size | Wait calls increase from 1 to 40 for every subsequent small update | Wait only on work submitted in the current batch |
| `yuv_bench.c` | Same small-update workload against public YUV APIs | Approximately 42–43 microseconds before full-frame warmup, 58–61 afterward | Informational timing; use the deterministic wait count as the future regression assertion |
| `skip_case.cpp` | One initial frame, then one skipped present per second for 60 seconds | One 60-second record; recommended `--warmup-intervals 2` discards all 60 skips | Emit expired intervals even for windows whose redraws are always skipped |
| `log_rotation_case.py` | Invoke real copytruncate rotator while real `tee` keeps writing | Nine NUL bytes precede `next\n`; logical file size 14 instead of 5 | Writer resumes at byte zero after truncation, e.g. with append mode |
| `overlay_case.cpp` | Render reconnect overlay, then destroy the window | SDL error `Parameter 'texture' is invalid` | Destroy `_stalled` textures before their renderer; no stale texture destruction |

### Isolation details

- `udp_cases.c` includes the existing core UDP test fixture to use its deterministic
  receive clock and wrap setup, and exercises the built production receiver.
- `send_cases.c` includes production `rdpeudp.c` to access the internal sender. It
  reduces the send timeout to 30 ms, uses the existing send callback to deliver
  packets and selectively ACK them, and stubs the unused socket/TLS cleanup paths.
  It proves byte-stream duplication below TLS; it is not a TLS handshake test.
  The Soft-Sync fixture calls the built production mapping implementation.
- `yuv_wait_cases.c` includes production `yuv.c`, wrapping only the wait call to
  count it. The wrapper delegates to the actual WinPR wait. No codec algorithm is
  replaced. `yuv_bench.c` calls the shared library without instrumentation.
- `probe_case.cpp` interposes SDL query functions to model Windows/X11 and
  macOS Retina scale and pixel-density semantics. This is not a test on a Windows desktop. See
  [SDL high-DPI documentation](https://wiki.libsdl.org/SDL3/README-highdpi).
- `overlay_case.cpp` uses SDL's dummy video driver and software renderer. SDL
  rejects the stale handles in this build; the reproduction does **not** show a crash.
- `log_rotation_case.py` lowers the rotation threshold to eight bytes and runs
  one rotation iteration. The launcher/server/capture entrypoint is not invoked.
- `verify_sdl.py` is the preserved build-and-test helper used for the six SDL tests.

## Baseline comparison and existing checks

The UDP late-zero cases preserve the byte on the earlier
`feat/further-performance-improvements` build (`d5a3492498952f6e96204dfe651e132a0c7fd83e`).
The quiet-watch case still stalls there. The older YUV implementation measured
about 42 microseconds per small update both before and after full-frame warmup.
These are local microbenchmarks, not end-to-end performance claims.

To rerun the public YUV benchmark against a different existing library:

```sh
DYLD_LIBRARY_PATH=/path/to/baseline/build/libfreerdp \
  tools/branch-review-20260926/results/yuv_bench
```

Validation completed during review:

- Freshly compiled: TestSDLInputMapping, TestSDLMonitorScale,
  TestSDLRenderGeometry, TestSDLRenderWindow, TestSDLRenderMetrics,
  TestSDLUpdateQueue — all passed.
- Existing target build: `TestCore TestRdpeUdp` and
  `TestFreeRDPCodec TestFreeRDPCodecYUV` — passed.
- `python3 -B -m unittest discover -s tools/graphics-replay -p 'test_*.py'`, run
  from the target worktree with its existing `build/graphics-replay/optimized`
  executable — 12 passed.

No live remote session, Windows/X11 runtime, or Metal presentation session was
run for this review. Existing tests passing does not cover the failure cases above.
