# Sleep report: third UDP channel wrap after recovered packet loss

Session: `/Users/david/rdp-debug/20260917-080921-93335/`, PID 93341,
08:09:21–09:22:38 CEST on 2026-09-17. Investigated offline after exit.

## Findings

The session used the sequencing fix: it logged successful channel-zero
omission handling at 08:15:10.040 and 08:45:29.848. A third boundary at
09:18:05 was not handled. This exposes the previously documented limitation
of the fix after receive history has lost a datagram.

| Time (CEST) | Evidence |
| --- | --- |
| 09:09:18.018089 | Frame 782560: DataSeq `c0a6`, channel `b922`. |
| 09:09:18.072181 | Frame 782565: DataSeq `c0a8`, channel `b924`; original DataSeq `c0a7` is absent. |
| 09:09:18.155104 | Frame 782568: missing channel `b923` recovered with fresh DataSeq `c0ab`. |
| 09:09:21.810088 | Frame 783467: DataSeq `c127` pushes the missing original `c0a7` out of the 128-packet ACK window. |
| 09:18:05.785535 | Frame 867658: DataSeq `0796`, channel `ffff`. |
| 09:18:05.802162 | Frame 867663: DataSeq `0797`, channel `0001`; channel zero is absent. |
| 09:22:36.489156 | Frame 868208: decrypted server RDP Heartbeat. |
| 09:22:37.698742 | Frame 868210: decrypted T.125 `disconnectProviderUltimatum`, reason `rn-provider-initiated (1)`. |

`rdpeudp_slide_recv_data_locked` permanently sets `recvSequenceHistoryLost`
when the original DataSeq leaves the window. It does so even though the
channel data was successfully recovered roughly 0.14 seconds after the last
preceding packet. `rdpeudp_can_skip_channel_zero_locked` subsequently refuses
to skip the omitted zero. The first two wraps had complete receive history;
the third did not. The fix therefore needs follow-up for subsequent wraps
after recovered loss.

Across the merged capture, 198,590 inbound packets have a DataSeq. Exactly
one original DataSeq is missing from its epoch; no channel zero is present
at any of the three wraps. Dumpcap reports 868,217 packets captured and zero
capture/interface drops.

Replaying all 198,590 captured inbound DataSeq packets through the actual
fixed VideoToolbox library reproduces both successful wraps and the third
stall. Delivery stops at frame 867658 with 127,664,092 bytes delivered and
`expectedChannelSeq == 0`. Frames carrying channels `0001` through `000e`
do not advance the stream, while DataSeq ACK accounting reaches `0823`.
This is a reproduction using the production receiver, not only an inference
from packet numbering.

## Deeper investigation: intact graphics, delayed ACKs, and no recovery

An independent reassembly deduplicated all 1,397 repeated channel chunks,
asserting identical bytes for each retransmission. The only missing extended
channel labels are 65,536, 131,072 and 196,608, exactly the three omitted
zeros. The resulting stream contains 127,669,048 bytes and 204,178 complete
TLS records, with no trailing partial record.

Using the original saved session keys, all **17 TLS records spanning or
following the third wrap authenticate** without adding or removing any
payload bytes. They contain 17 Tunnel DATA PDUs, including 14 DVC DATA PDUs
on channel 7, identified from the earlier CREATE request as
`Microsoft::Windows::RDS::Graphics`. A later CLOSE concerns channel 10,
`AUDIO_PLAYBACK_DVC`. The receiver withholds 4,956 encrypted stream bytes
after its last delivery offset. This establishes that the stalled suffix is
intact; it is not waiting for missing TLS payload. No session keys or
decrypted application payloads were printed or saved.

The ACK timing makes the failure worse:

- Last normal ACK: **09:18:05.786302**, DataSeq `0796`, frame 867660.
- First ACK after the blocked `0001` arrives: **09:18:10.829427**, DataSeq
  `07de`, frame 867743 — **5.027265 seconds** later.
- In between, the server sends channels `0001`–`000d` and retransmits them
  **58 times**. Each gets five or six transmission attempts.
- The first five-second keepalive ACK covers all of those received DataSeqs,
  even though none of their channel bytes have reached TLS. Subsequent probes
  and ACKs continue. Channel `000e` arrives during final session teardown.

In `rdpeudp2_recv_reliable`, normal ACKs are emitted when stream bytes are
consumed or are ready to consume. Out-of-order data stranded in `recvMap`
does not satisfy that condition. Consequently the wrap stall also suppresses
prompt ACKs, leaving the five-second keepalive to acknowledge the packets.
This ACK scheduling issue amplifies retransmissions; it does not account for
the original decision to wait for zero.

The state cannot recover by waiting: the omission check continues rejecting
the wrap because `recvSequenceHistoryLost` stays set for the transport's
lifetime. Receive polls return retry/no-data, not a connection failure.
`lastRecvTs` is updated but is not used as a receive-progress deadline.
There is no deadline on buffered-but-undelivered channel data. Also,
`rdp_check_fds` only logs negative multitransport results while keeping the
TCP connection alive; a future watchdog needs an explicit recovery path,
not just another log entry. The session remains stalled for **271.896580
seconds**, until the independent provider-initiated disconnect.

### Isolated reproduction

`reproduce-recovered-loss.py` feeds tiny synthetic datagrams through the
actual production receiver. Both cases traverse two channel wraps and
include the same retransmission. The only difference is whether one original
datagram was received. Both have identical delivered channel data after
recovery; at the next wrap the control advances to channel `0002`, while the
case with the recovered loss remains at `0000`. This reproduces the failure
without a Windows server, TLS content, idle timers, or any live network.

Saved scripts/results in `tools/udp-review-tests/logs/stale-20260917/`:
`reproduce-recovered-loss`, `verify-blocked-stream`, and `analyze-stall-acks`
(each has `.py` and `.txt` files). Production code was not changed during
this investigation.

## What the logs establish about sleep

The server kept sending TCP RDP heartbeats and UDP probes during the apparent
stall. In the final capture segment (09:15:25–09:22:37), the largest gap between
server packets is 2.092 seconds. This does not show the host becoming
unreachable during the stall. It does not identify the cause or timing of
an actual Windows power-state transition, including one after the final
disconnect.

The logged `ERRINFO_RPC_INITIATED_DISCONNECT` message is generated locally
from that generic provider-initiated MCS disconnect by `rdp_read_header`
in `libfreerdp/core/rdp.c`. The final packet does not carry a separate RDP
Set Error Info code identifying an administrative tool. The log wording
alone must not be used to conclude that someone manually disconnected the
session. The client then sends TLS close-notify and closes TCP normally;
this is not a crash or a network-timeout reconnect.

Windows' own power-event log is needed to determine whether and why it slept.
The user was asked for the approximate sleep time and whether a local wake or
sign-in preceded 09:22:37; no answer was available during the initial analysis.
The user subsequently clarified that they remained logged in but had not
worked with the Windows machine for a while. This makes an idle power timeout
plausible, but does not establish its exact timing or cause, and does not
change the independently reproduced receive-stream stall.

Temporary merged capture, packet metadata, decrypted final-protocol summary,
and replay script/results: `/tmp/freerdp-sleep-20260917/`.
Compact replay and disconnect evidence is also preserved in
`tools/udp-review-tests/logs/stale-20260917/`.
TLS secrets remain in the original session folder. No client or server
settings were changed and no live debugging or network disruption was used.

## Follow-up fix and regression verification — 2026-09-17

Implemented the three fixes identified above, plus the cleanup required to make
reconnect work with the new failure signal:

- The first omitted channel zero is still inferred only from complete DataSeq
  history. Once proven, the receiver remembers that peer's convention for the
  lifetime of the UDP transport. A recovered datagram loss no longer disables
  subsequent wraps. Real channel zero remains valid and, when delivered,
  disables the skip convention. DataSeq zero is never skipped. An ambiguous
  first wrap is left for recovery/reconnect rather than discarding possible data.
- Every received DATA datagram is ACKed promptly after buffering, including
  duplicate channel data and out-of-order channel data. ACK versus ACKVEC still
  follows DataSeq reception. ACK-only packets do not provoke another ACK;
  dummy probes before the first real DATA remain exempt. TLS consumption no
  longer controls ACK timing. Buffer overflow/allocation failure becomes a
  terminal failure instead of ACKing bytes the receiver cannot retain.
- A missing channel with later channels buffered starts a 10-second progress
  deadline. Advancing the expected channel refreshes it; duplicates, probes,
  and heartbeats do not. Empty receive queues and ordinary idle desktops do
  not start this timer. A one-second mainloop timer enforces the deadline even
  with no socket traffic (normally about 10–11 seconds until detection).
- A stalled/failed established reliable tunnel propagates a transport error
  through `freerdp_check_fds`, enabling the client's existing auto-reconnect
  behavior when enabled. Continuing on TCP alone cannot restore the peer's
  already-migrated incoming channels. Reconnect frees the old UDP transport,
  migration state and timer before starting a new session.

The watchdog logs the expected channel, buffered channel count, DataSeq base,
and elapsed gap duration. It does not infer Windows sleep from an idle desktop.
Learning the wrap convention assumes that a peer uses it consistently within
one UDP transport; it is not carried across reconnects.

Validation:

- Added `test_rx_recovered_loss_wrap`, demonstrated it failing on the previous
  implementation, then passing with this fix. It covers recovered loss across
  repeated wraps and an ordinary peer whose real channel zero is lost and
  retransmitted. All previous wrap/reordering tests still pass.
- New ACK tests decode actual production ACK/ACKVEC wire output immediately
  after receipt, including a channel gap with no DataSeq gap, duplicate DATA,
  probes, and the absence of ACK loops.
- New deterministic-clock tests cover expiry, just-in-time recovery, recovery
  after expiry, duplicate/probe traffic, changing gaps, idle connections and
  receive-map overflow without falsely acknowledging dropped bytes.
- A socketless integration test uses a real FreeRDP context, timer service and
  mainloop, with a healthy idle TCP read callback. With socket events cleared,
  the timer wakes the event loop; `freerdp_check_fds` fails with
  `FREERDP_ERROR_CONNECT_TRANSPORT_FAILED` and `ERRINFO_SUCCESS`. The test runs
  the real reconnect cleanup, verifies a healthy state, then repeats with a
  fresh UDP transport and watchdog.
- All four core CTest tests passed (Version, Settings, Utils, RdpeUdp). The
  final focused RdpeUdp run after formatting/additional ACK coverage also passed.
- Full session replay feeds all 198,590 server DataSeq datagrams into the native
  receiver using original capture timestamps. All three wraps advance, no
  watchdog expires, and all 127,669,048 TLS bytes match the independent channel
  reconstruction, including the previously stranded 4,956 bytes. SHA-256:
  `362775d852643ece44d620db1e1f3ee28cabc91a254f76a399b743de5bdea4bb`.
  TLS authentication of the records around the previously blocked boundary is
  documented in the earlier investigation; this replay compares encrypted bytes.

Replay tooling/results are in `tools/udp-review-tests/logs/stale-20260917/`:
`replay-fixed.py`, `replay-fixed.txt`, and `reproduce-recovered-loss-fixed.txt`.
The original reproduction output is preserved; its script accepts `--expect-stall`
to check an older library, or defaults to verifying the fix.

Rebuilt the usual SDL target successfully and repeated the full capture replay
against that final build’s dylib with the same passing byte comparison. Final
build identifiers:

- `build/videotoolbox/client/SDL/SDL3/sdl-freerdp`: September 17, 10:09:25 CEST;
  UUID `2C0ABA8B-9466-3DF2-8E1E-F5DB2F4EEA63` (arm64).
- `build/videotoolbox/libfreerdp/libfreerdp3.3.31.2.dylib`: 10:09:22 CEST;
  UUID `B375DE28-A7B1-3C8D-A915-E3D69966A185` (arm64).

An already-running client must be restarted to load this build. These checks
used offline captures and socketless fixtures; no live session was attached,
paused, restarted, or subjected to packet loss during this follow-up.
