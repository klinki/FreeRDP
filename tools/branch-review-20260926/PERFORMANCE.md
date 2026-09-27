# Performance impact of the review fixes

Measured locally on macOS on 2026-09-26. No live session or end-to-end FPS/latency
benchmark was run. The latest UDP receiver fix shows similar processing cost in
this microbenchmark; the YUV fix removes unnecessary waits.

## UDP receiver measurement

Compare `bcc863bf0` (before the office-capture wrap fix) with `63e07afe5` (after).
Both versions include the earlier review fixes. This comparison isolates the
latest UDP fix, rather than measuring the entire review branch against its base.

`benchmark_udp_receive.py` compiles both versions of `rdpeudp.c` with the same
`-O3 -DNDEBUG` options and matching build dependencies, then runs them sequentially
in alternating order. Each run excludes one warmup and measures eight rounds;
five before/after pairs were collected.

Each round replays 394,173 real datagrams from the six affected office flows,
delivering 439,631,285 bytes. Both versions stop before the first post-wrap
channel 1: the older version stalls there, so timing beyond it would compare
different amounts of successful work. All runs delivered the same byte count.
This totals 31,533,840 measured packet operations.

| Measurement | Before | After |
|---|---:|---:|
| Median receiver CPU time per packet | 733.113 ns | 735.599 ns |
| CPU time range across runs | 720.235–748.781 ns | 730.380–798.889 ns |
| Median wall time per packet | 734.459 ns | 736.256 ns |
| Transport structure size | 9,232 bytes | 9,744 bytes |

The difference between CPU medians is +2.486 ns per packet (+0.34%), within the
observed run variation. This does not establish a precise slowdown or zero cost.
The last after-run was slower (798.889 ns CPU, 913.273 ns wall); all runs,
including that outlier, are retained in [udp-performance-results.json](udp-performance-results.json).

The wrap fix adds 512 bytes per UDP connection, channel-label bookkeeping, and
an extra small array move when the receive window advances. It adds no thread,
lock, or per-packet heap allocation. The capture correctness replay separately
confirms recovery of all six observed wrap stalls; see [OFFICE-CAPTURE.md](OFFICE-CAPTURE.md).

Packet loading is outside timing. The harness uses the production receiver via
its socketless test entry point, drains delivered bytes, and includes transport
creation and cleanup. It excludes network I/O, ACK serialization, TLS decryption,
and rendering. These numbers are receiver CPU cost, not total application CPU
or frame rate. The earlier reliable-send fix is also outside this comparison.

## Other fixes

| Fix | Expected performance impact |
|---|---|
| YUV batch waits | Reduces work: after a full 4K frame, a small update waits on one submitted work item instead of all 40 cached items. Verified by the preserved wait-count harness. |
| Reliable UDP send failure | Adds a locked health check per write. Not separately benchmarked. On exhausted retries, retires the stream instead of replaying bytes that may already have arrived. |
| Metrics intervals | When metrics are enabled, skip-only or stalled-only windows now produce an expired interval log entry, normally once per second. Disabled metrics still return immediately. |
| Monitor pixel density | Display probing change; no added per-frame rendering work. |
| Reconnect textures | Changes destruction order during cleanup; no added per-frame work. |
| Empty DVC migration list | Corrects channel routing for an explicit empty list; no extra rendering work. Affected channels remain on TCP as requested by the peer. |
| Append-mode diagnostic logs | Changes how diagnostic writers open their log files; does not add writes. |
| Graphics replay linking | Offline replay build change; does not affect the live client. |

## Reproduce

Requires the private packet caches produced by `analyze_office_udp.py` and a
matching macOS Unix Makefiles build. Raw captures, TLS secrets, and packet caches
are not included in this directory.

```sh
python3 -B tools/branch-review-20260926/benchmark_udp_receive.py \
  --source /path/to/review-worktree \
  --build /path/to/matching/cmake-build \
  --packets /path/to/office-capture/analysis \
  --output /path/to/udp-performance
```

The output directory retains source snapshots, compile commands, binaries, logs,
and results. Timings are informational and should not become fixed regression
thresholds; correctness and equal-workload checks remain deterministic.
