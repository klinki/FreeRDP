# Video playback after the redraw fix — 2026-09-18

## Status update — 2026-09-28

This document records the September 18 diagnostic run and its tested build.
The recovered-loss first-wrap stall was subsequently addressed by the UDP
sequence fixes now integrated on `master`, including the DataSeq-ordered
recovery described in [office replay validation](tools/branch-review-20260926/OFFICE-CAPTURE.md).
The failure and proposed transport work below are historical findings, not a
new defect report against current master.

The measured observations and capture identity below are preserved unchanged.
No new live playback or visual-quality acceptance test was run for this
preservation work. The referenced packet captures, TLS keys, and packet-level
evidence remain local.

## Findings

The new UI profiles support the responsiveness improvement: the main thread returns to event waiting while graphics decoding and presentation continue. Neither profile reproduces the earlier all-rendering main-thread starvation. These are short observations, not a controlled playback benchmark or proof that every workload is fixed.

The session also exposed a remaining UDP sequencing case. At 19:11:08 CEST the client detected a receive-window overflow and automatically reconnected. Offline replay of the captured packets against the production receive implementation reproduces the exact failure: the first channel wrap remains blocked waiting for `0000` after Windows sends `ffff → 0001`. Earlier missing datagrams prevented learning the peer's omitted-zero convention, even though all earlier channel bytes had subsequently arrived.

The reconnect safeguard worked: a new UDP tunnel was established about 850 ms later. The subsequent tunnel successfully handled its first omitted-zero wrap at 19:12:39.073. This incident is separate from both the earlier UI starvation and the earlier Windows NVIDIA encoder crash.

The user subsequently confirmed that they noticed **no problems during this test**. The reconnect is a logged internal event, not a reported visible freeze; the tunnel timing alone does not measure how long any reconnect label was visible.

## Build and capture

- Session directory: `/Users/david/rdp-debug/20260918-190435-67824`.
- Client PID 67830, started 19:04:35; session metadata records commit `8a3702904`, `[client,sdl] Keep input responsive during continuous redraws`.
- Executable: `build/videotoolbox/client/SDL/SDL3/sdl-freerdp`, installed at 18:54, before this process started. Its SHA-256 matches the tested executable in `build/video-responsive`: `4c53701b496993185033cf0b5873ea2c073570d55b42c5af28ac1f552365234c`.
- This executable links the FreeRDP libraries from `build/video-responsive`; retain that build directory.
- Packet capture was already active on `utun10`, PID 67827, with a 10 × 100 MB ring. The client and capture were still running at the final process check, 19:18:36.
- TLS secrets are retained locally for possible decryption; the sequence analysis below only requires transport metadata.
- Diagnostics consisted of two 2-second macOS `sample` profiles, short CPU sampling, log reads, capture snapshots, and a separate socketless replay. No packet loss was injected, no debugger was attached, and no live session settings were changed.

## UI and CPU observations

| Observation | Before fix, during loss of control | After fix, sample 1 | After fix, sample 2 |
|---|---:|---:|---:|
| Time, CEST | 17:43:27 | 19:11:14.966 | 19:12:18.207 |
| Main-thread samples | 170 | 178 | 176 |
| Drawing | 170 (100%) | 54 (30.3%) | 37 (21.0%) |
| SDL/Cocoa event-wait path | 0 | 119 (66.9%) | 138 (78.4%) |

The remaining observations are event pumping and other loop work. These percentages describe sampled wall time, not CPU utilization. Both new profiles are after the reconnect; neither records the failure itself. The video content and amount of changing screen area were not held constant across the old and new observations.

Texture upload remains the main sampled rendering cost: 52 observations in sample 1 and 36 in sample 2 are inside `SDL_UpdateTexture`. VideoToolbox and AVC444 processing are active. Sample 2 shows only one observation in `CountdownEvent_AddCount`, versus 95 in the earlier incident; this workload difference is not evidence that the unchanged YUV conversion code has been optimized.

The RDP thread spends substantial sampled time polling for UDP acknowledgements inside `rdpeudp_bio_write`. That is elapsed waiting time, not equivalent CPU consumption or proof of a stuck send.

The three usable short `top` intervals report 38.1%, 96.1%, and 83.1% client CPU, with approximately 1.16–1.20 GB memory. On macOS, 100% is one CPU core. System memory was heavily used/compressed, with swap activity during this measurement, so whole-machine load is another confounder. No visual-quality conclusion can be drawn from these profiles.

## Packet sequence and failure

The first connection used local UDP port 53534. The saved extract covers 19:04:51.196704–19:11:08.213085:

- 132,053 UDP packets in both directions.
- 65,877 server channel chunks; 65,828 unique unwrapped positions and 49 duplicates.
- Between positions 1 and 65,829, the only missing channel position is 65,536: channel `0000` at the first wrap.
- 30 original server DataSeq values are absent, all between 1493 and 1530, around 19:04:54. Their channel data was recovered: there is no earlier remaining channel hole. A client-side capture does not establish where datagrams were lost, or independently rule out capture loss.

Replay of that observed server datagram stream uses `rdpeudp_test_new` and `rdpeudp_test_feed` in the same `build/video-responsive` library. The fixture has no socket or send callback. Capture timestamps drive its test clock. This is the same receive implementation used by the live transport; the function name in the live error log does not imply a debugging injection.

| Capture frame | Time, CEST | Received channel | Next expected channel | Delivered stream bytes | Health |
|---:|---|---|---|---:|---|
| 159107 | 19:11:07.768174 | `ffff` | `0000` | 73,841,852 | Healthy |
| 159109 | 19:11:07.768190 | `0001` | `0000` | 73,841,852 | Healthy, no progress |
| 159630 | 19:11:08.000085 | `0101` | `0000` | 73,841,852 | Receive-window overflow |

The replay processes 66,027 datagrams before failure. The receive bitmap has no outstanding bits at all three checkpoints, but the historical-loss flag prevents the first-wrap inference. Channels `0001` through `0100` occupy the 256-slot receive map; `0101` collides with buffered `0001`. The stream stops advancing for 232 ms before the explicit failure. This matches the live error timestamp exactly.

The source explanation is in `libfreerdp/core/rdpeudp.c`, specifically `rdpeudp_slide_recv_data_locked`, `rdpeudp_can_skip_channel_zero_locked`, and `rdpeudp_deliver_recv_data_locked`. Missing original DataSeq history is retained conservatively even after replacement datagrams deliver the missing channel bytes. Commit `c8fde9472` allows an already-confirmed omitted-zero convention to survive later losses. Here the loss happened **before the first wrap**, so there was no confirmed convention yet.

The live log reports the transport failure and reconnect at 19:11:08.001, the new UDP tunnel at 19:11:08.851, and session logon information at 19:11:11.423. A server TCP reset at 19:11:08.005397 follows the local overflow; it does not explain the preceding receive failure.

The replacement flow, local UDP port 50517, has 40,866 UDP packets in the retained metadata slice through 19:11:41.066022, with 19,409 unique channel positions, 7 duplicate chunks, and no channel or DataSeq gaps. The later log records a successful first wrap at 19:12:39.073 using complete DataSeq history. No additional transport failure appears in the log snapshot taken at 19:18:12.

## Next transport work (at the time of this run)

A compact independent reproducer, `first-wrap-repro.py`, reduces the problem to synthetic one-byte channel payloads, with no packet capture, TLS keys, decoder, UI, or network involved. It confirms all three cases against the current production library:

| Synthetic case | Result |
|---|---|
| Complete DataSeq history, peer skips zero | First wrap succeeds; channel stream continues. |
| One early datagram lost, its channel bytes retransmitted successfully, peer skips zero | First wrap remains at zero and overflows on channel `0101`. |
| Same early recovered loss, peer actually sends zero but its first transmission is lost | Receiver correctly waits; retransmitted zero delivers `Z` before buffered channel-one byte `A`. |

These checks reproduce the current behavior, including the unwanted overflow; they are a regression seed rather than a passing test of a new fix. The existing `test_rx_recovered_loss_wrap` in `libfreerdp/core/test/TestRdpeUdp.c` covers loss **after** a first wrap has established the peer convention. It does not cover the newly reproduced ordering.

Add a regression covering loss and retransmission before the first omitted-zero wrap, using this replay as supporting evidence. Then improve the first-wrap inference without treating a genuinely lost channel zero as an omitted zero. Simply ignoring the historical-loss flag or skipping zero for every peer could corrupt the TLS byte stream. The current behavior safely reconnects, but still interrupts an otherwise recoverable session.

No production code was changed during this diagnostic run. The visual block artifacts remain unexplained; these findings do not establish a new NVIDIA or H.264 failure.

## Preserved evidence

All artifacts are in [`diagnostics/video-playback-20260918-after-fix/`](diagnostics/video-playback-20260918-after-fix/), within this project, independent of `/tmp`. The directory is locally ignored by Git and access-restricted; packet captures and TLS secrets should remain local.

- `sample-1.txt`, `sample-2.txt`, `top-1.txt`: thread profiles and CPU intervals.
- `session.txt`, `test-start.json`: session and collection identity.
- `client.log`, `client-latest.log`, `capture.log`, `capture-latest.log`: snapshots; latest snapshot taken at 19:18:12.
- `capture_00001_20260918190435.pcapng`: complete first capture, including the failed wrap and reconnect.
- `capture_00002_20260918191123.pcapng`: partial second capture through 19:11:41 used for `packets.tsv`.
- `capture_00002_full.pcapng`: later complete copy of the second capture, preserving the subsequent successful wrap as well.
- `tls-secrets.txt`: local session decryption material.
- `packets.tsv`, `packet-summary.json`: transport metadata and counts for the first capture plus the partial second capture.
- `replay-input.tsv`, `replay.py`, `replay-result.txt`, `replay-result.json`: reproducible offline receive failure, with capture frame references.
- `first-wrap-repro.py`, `first-wrap-repro-result.txt`, `first-wrap-repro-result.json`: compact synthetic reproducer and control cases.
- `evidence-manifest.json`: artifact byte sizes and SHA-256 hashes.

For the earlier incident and evidence, see [`VIDEO-PLAYBACK-FINDINGS-20260918.md`](VIDEO-PLAYBACK-FINDINGS-20260918.md).
