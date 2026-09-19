# Commit-to-feature map

Prepared on 2026-09-17 to support future feature extraction, task organization, and pull-request splitting.

## Scope

- Source branch at the time of review: `fix/multimon-layout`.
- Oldest included commit: `24dda4b815a8cd47f06398a0175c7d85c6106640` — the first user-authored commit and the local `master` tip at the time of review.
- Newest included commit: `c8fde9472e517d33ce046b87b5b569cf6e0d0282`.
- Both endpoints are included: **64 commits**, ordered oldest to newest.
- Earlier history, commits exclusive to other branches, and uncommitted changes are outside this report.

The fixed range can be listed with:

```sh
git log --reverse --format="%h %ad %s" --date=short 24dda4b815a8cd47f06398a0175c7d85c6106640^..c8fde9472e517d33ce046b87b5b569cf6e0d0282
```

## Classification

Classification is based on commit messages and changed content. Reviews, tests, scripts, and investigation notes are assigned to the feature they support. A feature label does not imply that the commit implements a production fix or can be cherry-picked independently. The Kind column distinguishes these cases; semicolons in Feature indicate a mixed commit.

## All commits

| # | Commit | Date | Feature | Kind | Contribution |
|---:|---|---|---|---|---|
| 1 | `24dda4b81` | 2026-09-02 | Per-monitor scaling | Implementation | Add per-monitor desktop/device scale overrides and tests. |
| 2 | `f9da435df` | 2026-09-05 | UDP transport | Implementation | Initial reliable UDP transport, handshake, TLS, tunnels and channel routing. |
| 3 | `a9eb02727` | 2026-09-05 | UDP transport | Fix + review | Fix initial review findings: receive buffering, framing, lifecycle, events and migration. |
| 4 | `ec430af14` | 2026-09-05 | UDP transport | Review | Record UDP review findings. |
| 5 | `02ec5a114` | 2026-09-05 | UDP transport | Fix + review | Fix packet encoding, Soft-Sync migration and partial tunnel-PDU reassembly. |
| 6 | `444b698b3` | 2026-09-05 | UDP transport | Fix + review | Fix nonblocking reads, per-channel routing and strict validation. |
| 7 | `1354c6e2a` | 2026-09-05 | UDP transport | Fix + review | Harden migration-map installation and fragmented Soft-Sync requests. |
| 8 | `e2e2d0e40` | 2026-09-05 | UDP transport | Review | Update UDP review findings. |
| 9 | `6fefaf21e` | 2026-09-05 | UDP transport | Fix + review | Share strict Soft-Sync parsing across client, server and transport. |
| 10 | `ed931403e` | 2026-09-05 | UDP transport | Interop fix | Correct SYN cookie-hash byte order. |
| 11 | `11408bc5e` | 2026-09-05 | UDP transport | Interop fix | Start sequences at one and defer ACKs until receiving data. |
| 12 | `8ddd53a5f` | 2026-09-05 | UDP transport | Fix + review | Preserve the receive sequence base and acknowledge contiguous data. |
| 13 | `6eab8283f` | 2026-09-06 | UDP transport | Interop fix + test | Correct the UDP2 prefix byte and add a Windows-capture regression. |
| 14 | `da614a872` | 2026-09-06 | UDP transport | Interop fix | Match Windows delayed-ACK parameters and bound the send window. |
| 15 | `8223cf9ac` | 2026-09-06 | UDP transport | Test tooling | Add UDP review harnesses, tests and logs. |
| 16 | `a1ce296da` | 2026-09-06 | UDP transport | Review / investigation | Preserve UDP reviews and packet-capture comparisons. |
| 17 | `7abff736c` | 2026-09-06 | UDP transport | Interop fix | Fix handshake ACK behavior, BIO readiness and receive epoch. |
| 18 | `965365678` | 2026-09-06 | UDP transport | Diagnostics | Add runtime-gated UDP tracing. |
| 19 | `0de3c95e5` | 2026-09-06 | UDP transport | Interop fix + tooling | Fix tunnel-subheader framing; add a VM fixture and loop launcher. |
| 20 | `db0f808c0` | 2026-09-06 | UDP transport | Fix + tooling | Track all DATA sequences, adjust AOA epoch and increase send timeout. |
| 21 | `e8e4e268a` | 2026-09-06 | UDP transport | Interop fix | Gate ACK emission on real DATA; the actual diff only changes the receiver. |
| 22 | `d549ba018` | 2026-09-06 | UDP transport | Implementation / interop | Receive and dispatch raw dynamic-channel PDUs over UDP. |
| 23 | `6dc844d3a` | 2026-09-06 | UDP transport | Test tooling | Enable channel debug logging in the VM loop. |
| 24 | `33e410c56` | 2026-09-06 | UDP transport | Cleanup + test | Remove dead code and test fragmented DATA_FIRST handling. |
| 25 | `dfa15584e` | 2026-09-06 | UDP transport | Review | Record another UDP review. |
| 26 | `47459d93e` | 2026-09-06 | UDP transport | Fix + review | Fix server accept peeking, AOA advancement and CREATE direction. |
| 27 | `9fbe4fb10` | 2026-09-06 | UDP transport | Test tooling | Add review harnesses for AOA and framing. |
| 28 | `4c1ec178e` | 2026-09-06 | UDP transport | Review | Record UDP follow-up review findings. |
| 29 | `11c4f822b` | 2026-09-06 | UDP transport | Fix + review | Fix raw-DVC sending, accept draining and AOA gating. |
| 30 | `dcbfeed14` | 2026-09-06 | UDP transport | Review | Record further UDP review findings. |
| 31 | `fcd082c29` | 2026-09-06 | UDP transport | Fix + review | Send whole channel PDUs over UDP; update review conclusions. |
| 32 | `84a15593f` | 2026-09-06 | UDP transport | Review | Update UDP review status and disputes. |
| 33 | `c08569f65` | 2026-09-06 | UDP transport | Test tooling | Extend accept, AOA and channel-fragmentation harnesses. |
| 34 | `a67cdfe05` | 2026-09-06 | UDP transport | Refactor + test | Share the production receive path with transport integration tests. |
| 35 | `026e5a022` | 2026-09-06 | UDP transport | Review | Record integration-test evidence and remaining exceptions. |
| 36 | `bbb2a1692` | 2026-09-06 | UDP transport | Test coverage | Add allocation-failure tests for Soft-Sync migration. |
| 37 | `8a700aebb` | 2026-09-06 | UDP transport | Review | Update UDP review conclusions. |
| 38 | `764326c61` | 2026-09-08 | SDL lifecycle | Fix | Prevent teardown exceptions/hangs and wake the RDP thread on termination. |
| 39 | `52c66884c` | 2026-09-08 | Build/debug tooling | Build tooling | Add the SDL3/VideoToolbox build script. |
| 40 | `d6e9fbfe6` | 2026-09-08 | UDP transport | Build fix | Declare UDP deallocators before allocator attributes reference them. |
| 41 | `343a09420` | 2026-09-08 | Connection topbar | Implementation + tooling | Add topbar controls, input ownership, review notes and a multimon launcher. |
| 42 | `36e162bdb` | 2026-09-09 | Build/debug tooling | Diagnostics | Add a debug launcher with rolling packet capture and session logs. |
| 43 | `b7350b786` | 2026-09-09 | Rendering performance | Investigation only | Document rapid-motion rendering freezes; no rendering fix. |
| 44 | `9e7e5c9dd` | 2026-09-09 | Build/debug tooling | Diagnostics | Add session metadata, log rotation and freeze snapshots. |
| 45 | `a71f06cf3` | 2026-09-09 | Rendering performance | Launcher configuration | Request async-update in launchers; not a renderer implementation change. |
| 46 | `9ac0af952` | 2026-09-09 | Build/debug tooling | Launcher configuration | Track the AVC444/4K UDP test launcher. |
| 47 | `9f2002424` | 2026-09-09 | SDL lifecycle | Fix | Ignore cursor refresh after GDI teardown. |
| 48 | `ab3152267` | 2026-09-10 | Cross-monitor input | Investigation + tooling | Document drag-mapping problems and add screen recording tooling. |
| 49 | `4a3992897` | 2026-09-10 | Connection topbar; DPI diagnostics | Mixed implementation | Make the topbar compact, draggable and resizable; add monitor-scale diagnostics. |
| 50 | `15179d3f9` | 2026-09-10 | Connection topbar | Fix / enhancement | Add compact-mode toggle and fix topbar drag handling. |
| 51 | `72a6c0d04` | 2026-09-10 | Startup windows | Fix | Keep windows hidden until final placement/fullscreen state. |
| 52 | `fabad8d5e` | 2026-09-11 | Cross-monitor input | Fix | Apply monitor offsets to focus-generated pointer events. |
| 53 | `a8ace5fb9` | 2026-09-11 | Rendering performance; connection diagnostics | Mixed investigation | Document render-freeze benchmarks, connection failures and startup black screens. |
| 54 | `a15a3ea85` | 2026-09-11 | Per-monitor scaling | Specification | Add the detailed per-monitor scaling task specification. |
| 55 | `8ce66d814` | 2026-09-11 | Cross-monitor input | Investigation only | Update focus-offset, signed-delta and motion-coalescing analysis. |
| 56 | `cedcfb174` | 2026-09-11 | Cross-monitor input | Input handling | Coalesce mouse motion across windows; a separate change from coordinate mapping. |
| 57 | `47ed1eff7` | 2026-09-11 | Startup windows | Fix | Make monitor probing hidden/windowless to eliminate startup flashing. |
| 58 | `b9d88ab52` | 2026-09-11 | Cross-monitor input | Fix + investigation | Preserve event order during motion coalescing; qualify earlier diagnosis. |
| 59 | `84733797d` | 2026-09-11 | Reconnect feedback | Specification / investigation | Document silent stalls and propose dimming/reconnect feedback. |
| 60 | `0e02c1dd9` | 2026-09-11 | Cross-monitor input | Investigation only | Record wire evidence against stale-event replay as the slow-drag cause. |
| 61 | `ec95186e4` | 2026-09-11 | Reconnect feedback | Implementation | Dim the last frame and show Reconnecting during reconnect attempts. |
| 62 | `399e8d454` | 2026-09-11 | Cross-monitor input | Investigation only | Record historical DPI-resize diagnosis, later corrected by the captured-drag fix. |
| 63 | `3b6f9b67b` | 2026-09-16 | Cross-monitor input | Fix + tests / evidence | Map captured drags through the destination monitor scale and origin. |
| 64 | `c8fde9472` | 2026-09-17 | UDP stall recovery | Fix + tests / evidence | Fix sequence wraps after loss, immediate ACKs, watchdog failure and reconnect cleanup. |

## Notes for future splitting

- **Per-monitor scaling and coordinate mapping:** `24dda4b81` adds explicit per-monitor scaling; `fabad8d5e` fixes focus-generated pointer offsets; `3b6f9b67b` fixes captured pointer mapping across mixed-DPI monitors. `a15a3ea85` is the scaling specification. This identifies the original implementation commits, not a claim that they can be cherry-picked unchanged onto another base.
- **Input coalescing:** `cedcfb174` and its ordering correction `b9d88ab52` concern event processing. They are distinct from coordinate mapping, even though both were investigated during the drag work.
- **Connection topbar:** `343a09420`, `4a3992897`, and `15179d3f9` form the topbar work. The initial commit also contains a launcher and review notes; `4a3992897` additionally changes DPI diagnostics.
- **Startup windows:** `72a6c0d04` and `47ed1eff7` address initial window visibility and monitor probing. They are separate from scaling, input mapping, and the topbar.
- **UDP transport:** the implementation, interoperability fixes, reviews, and transport-specific tooling form a large related series. `c8fde9472` adds later stall recovery and depends on that transport implementation.
- **Reconnect feedback versus recovery:** `ec95186e4` displays reconnect activity; `c8fde9472` detects persistent UDP reassembly gaps and propagates failures into reconnect. These are distinct features. `84733797d` documents the feedback proposal.
- **SDL lifecycle:** `764326c61` and `9f2002424` harden termination and teardown. They are not monitor-layout features.
- **Rendering performance:** `b7350b786` is investigation, and `a71f06cf3` changes launcher options only. Neither should be described as a completed rendering-performance fix.
- **Mixed diagnostic documentation:** `a8ace5fb9` covers rendering performance, connection failures, and startup black screens. Split its content by subject if preparing focused patches.
- **Historical conclusions:** investigation commits preserve hypotheses from that point in time. In particular, the diagnosis in `399e8d454` was subsequently corrected by the captured-drag work in `3b6f9b67b`. Consult later evidence before presenting historical conclusions as current findings.
- **Tooling and evidence:** review archives, debug scripts, launcher configuration, and captured logs may need different treatment from production code in an upstream PR. This report categorizes them; it does not establish dependency order or upstream readiness.

## Original commit subjects

The original subjects are retained below for unambiguous lookup, including generic documentation/review titles.

| Commit | Original subject |
|---|---|
| `24dda4b81` | SDL3: add per-monitor scale overrides |
| `f9da435df` | [core,multitransport] implement UDP protocol support (RDPEUDP/RDPEUDP2) |
| `a9eb02727` | [core,multitransport] fix UDP review P1-1..P1-8 |
| `ec430af14` | docs: Add new review |
| `02ec5a114` | [core,multitransport] fix re-review R1-R3 and P1-8 migration |
| `444b698b3` | [core,multitransport] fix re-review S1-S3: zero-timeout read, per-DVC routing, strict validation |
| `1354c6e2a` | [core,multitransport] fix re-review T1-T2: installed-map gate, reassembled requests, strict parity |
| `e2e2d0e40` | docs: Update review |
| `6fefaf21e` | [core,multitransport] fix U1: shared Soft-Sync parser, no-list trailing rejected |
| `ed931403e` | [core,rdpeudp] send SYN cookie hash words in network byte order |
| `11408bc5e` | [core,rdpeudp] fix UDP2 interop: 1-start seq, DATA-only until RX |
| `8ddd53a5f` | [core,rdpeudp] fix review V1-V2: fixed seq base, contiguous ACK |
| `6eab8283f` | [core,rdpeudp] fix mirrored UDP2 prefix byte (0x07->0xE0), add MS-capture interop test |
| `da614a872` | [core,rdpeudp] match MS-observed delay-ack params, cap send window by slots |
| `8223cf9ac` | chore: Add UDP review tests, harnessses and logs |
| `a1ce296da` | docs: preserve UDP reviews and packet capture comparisons |
| `7abff736c` | [core,rdpeudp] skip standalone final ACK, fix BIO event + receive-base epoch |
| `965365678` | [core] add temporary UDP_TRACE live-debug facility (runtime-gated) |
| `0de3c95e5` | [core,rdpeudp] overlaid tunnel-subheader model + live-VM fixture + vm loop rig |
| `db0f808c0` | [core,rdpeudp] record all DATA seqs for ACK state, AOA epoch snap, patient send timeout |
| `e8e4e268a` | [core,rdpeudp] gate ACK emission on real DATA; harden vm loop rig |
| `d549ba018` | [core,rdpeudp] raw DVC PDUs over UDP: splitter, observed-migration latch, dispatch |
| `6dc844d3a` | [scripts] vm loop: channel DEBUG filters |
| `33e410c56` | [core,rdpeudp] cleanup: drop dead code, fragment-tolerant DATA_FIRST test |
| `dfa15584e` | docs: Add new review |
| `47459d93e` | [core,rdpeudp] N2 peek-only accept, N3 AOA epoch advance, N4 direction-aware CREATE |
| `9fbe4fb10` | chore: Add review harness and tests |
| `4c1ec178e` | docs: Add new review |
| `11c4f822b` | [core,rdpeudp] N1 raw-DVC send, P1 accept drain, P2 AOA gate; close review findings |
| `dcbfeed14` | docs: Add new review |
| `fcd082c29` | [core] Q1 whole-PDU UDP send, P2 partial with Q2 dispute; update review |
| `84a15593f` | docs: Update review |
| `c08569f65` | chore: Update review harness |
| `a67cdfe05` | [core,rdpeudp] Q2 transport integration test with test driver; share v2 receive path |
| `026e5a022` | docs: close P3, record Q2 integration evidence, tag V1 fixed-with-exception |
| `bbb2a1692` | [core] Soft-Sync alloc-failure injection tests with mapping test driver |
| `8a700aebb` | doc: Update review |
| `764326c61` | [client,sdl] No-throw teardown drain, abort in term handler |
| `52c66884c` | build: add SDL client build script |
| `d6e9fbfe6` | fix(core): declare UDP deallocators before attributes |
| `343a09420` | Add SDL multi-screen connection topbar |
| `36e162bdb` | chore: Add debug script |
| `b7350b786` | chore: Add bug with rapid-window movement |
| `9e7e5c9dd` | [tools] Debug script metadata, log rotation, freeze snapshot helper |
| `a71f06cf3` | [tools] Enable async-update in launch scripts |
| `9ac0af952` | [tools] Track free-rdp-02 launcher script |
| `9f2002424` | [client,sdl] Graceful cursor refresh after GDI teardown |
| `ab3152267` | [docs] Drag mapping investigation notes |
| `4a3992897` | [client,sdl] Compact draggable resizable topbar |
| `15179d3f9` | [client,sdl] Compact topbar toggle and working drag |
| `72a6c0d04` | [client,sdl] Hidden-window reveal on final placement |
| `fabad8d5e` | [client,sdl] Route focus-synthesized input through monitor offsets |
| `a8ace5fb9` | docs: Add bug reports descriptions |
| `a15a3ea85` | docs: Add multimonitor.md |
| `8ce66d814` | [docs] Update drag-mapping with focus fix, signed-delta analysis, global coalescing |
| `cedcfb174` | [client,sdl] Coalesce motion globally across windows to kill stale replay jumps |
| `47ed1eff7` | [client,sdl] Kill startup slideshow: hidden windowless monitor probe |
| `b9d88ab52` | [client,sdl,docs] Order-preserving motion coalescing, soften drag-mapping claims |
| `84733797d` | [docs] Report silent connection stalls, propose dim plus reconnecting indicator |
| `0e02c1dd9` | [docs] File slow-drag wire verdict, falsify stale replay for slow drags |
| `ec95186e4` | [client,sdl] Dim plus Reconnecting overlay for stalled transport |
| `399e8d454` | [docs] Resolve drag mapping: DPI resize convicted, wire verdicts filed |
| `3b6f9b67b` | [client,sdl] Fix captured drags across mixed-DPI monitors |
| `c8fde9472` | [core] Recover reliable UDP sequence wraps and stalled streams |

## Additions after the original review — 2026-09-19

The 64-commit snapshot above remains the original reviewed range. These later
commits extend the feature map through `e297cbd0f`, oldest to newest.

| Commit | Date | Feature | Kind | Contribution |
| --- | --- | --- | --- | --- |
| `9caf7000a` | 2026-09-17 | Contribution organization | Documentation | Preserve the original commit-to-feature map. |
| `5ba31f4ea` | 2026-09-18 | Video playback responsiveness | Investigation | Record SDL event-loop starvation during continuous video updates. |
| `8a3702904` | 2026-09-18 | Video playback responsiveness | Fix + tests | Bound redraw processing so continuous updates leave time for input. |
| `f6d34cd3f` | 2026-09-19 | SDL rendering observability | Diagnostics + workload + tests | Add opt-in per-window upload/draw/present metrics and a repeatable offline workload. |
| `dc9f13f80` | 2026-09-19 | Per-monitor rendering efficiency | Implementation + tests | Clip dirty regions to visible window areas, skip unaffected windows, and preserve full initialization/resize redraws. |
| `e297cbd0f` | 2026-09-19 | Texture upload batching | Implementation + tests | Upload all dirty regions before drawing; validate scaled source bounds and test stale-pixel preservation. |

The three rendering commits are separate for review and future extraction.
Batching builds on the clipping/lifecycle change; the instrumentation is optional
at runtime. See [rendering validation](RENDER-OPTIMIZATION-VALIDATION-20260919.md)
for test coverage, AVC444 VM observations, measurements, and remaining limits.
