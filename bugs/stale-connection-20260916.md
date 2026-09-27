# Frozen desktop with responsive topbar: UDP channel sequence wrap

Diagnosed on 2026-09-16, times below in CEST. Live session PID 5742,
`20260916-200524-5735`, DavidPC, repository revision `3b6f9b67b`.
The running session was sampled and inspected without disconnecting it.

## Finding

The capture and an offline replay through the same built FreeRDP library
reproduce a receive-stream stall at **20:24:46.797002**. The server's UDP
channel sequence advances from `0xffff` to `0x0001`; the client's plain
`expectedChannelSeq++` wraps to `0x0000`. No channel sequence `0x0000`
appears in the captured session. Subsequent channel data remains buffered
behind that missing sequence, while transport acknowledgments and heartbeats
continue. This is a concrete client receive-path failure, not sufficient
evidence of a Windows compositor/session hang.

Relevant source: `libfreerdp/core/rdpeudp.c:1942–1963` (in-order delivery and
sequence increment). The SDL reconnect overlay is enabled only after
`freerdp_check_event_handles` reports failure
(`client/SDL/SDL3/sdl_context.cpp:753–760`). This receive-stream stall does
not produce that transport failure, so the UI continues to appear connected.

## Live evidence

- Five-second `sample` at 20:27:48: main thread normally waiting in
  `SDL_WaitEventTimeoutNS`; RDP thread 4141/4317 samples in the event wait,
  with most remaining samples polling UDP through the TLS receive path.
  Channel workers are waiting on queues. No deadlock or render spin is
  visible in this sample.
- TCP remains ESTABLISHED with zero receive and send queue bytes.
- Input events report `connected=true, suspended=false`. Despite its name,
  the `input_skip` logging function prints accepted input as well; those
  flags mean it returns false. Client TCP payloads continue and are ACKed
  by the server, including at 20:28:35.
- Decrypted server TCP frame 319713 at 20:28:35.011521 is an RDP Heartbeat
  PDU, not a graphics update. Server UDP frame 319413 at 20:28:32.585194
  is a dummy probe; client ACKs keep advancing.
- The server retransmits channel data after the stall. The final captured
  real UDP channel data is frame 316801 at 20:25:17.115904, channel `0x00d9`.
  Later UDP traffic in the snapshot is dummy/probe traffic.
- Logs contain clipboard conversion errors earlier in the session, but no
  corresponding transport-disconnect error. Piped log output is buffered,
  so the log snapshot ends before the packet snapshot.

## Reproduction

`tools/udp-review-tests/logs/stale-20260916/replay.py` feeds all 68,166
captured inbound UDP packets carrying a DataSeq through the production
`rdpeudp_test_feed` API in
`build/videotoolbox/libfreerdp/libfreerdp3.3.31.2.dylib`. This is an isolated
offline receive-state replay, not inspection or modification of live memory.
The ctypes structure uses macOS's one-byte BOOL layout.

| Frame | Channel sequence | Next expected | Delivered TLS stream bytes |
| --- | --- | --- | --- |
| 313335 | `0xfffe` | `0xffff` | 61,856,235 |
| 313336 | `0xffff` | `0x0000` | 61,857,465 |
| 313337 | `0x0001` | `0x0000` | 61,857,465 |
| 316801 | `0x00d9` | `0x0000` | 61,857,465 |
| 319413 | dummy | `0x0000` | 61,857,465 |

The final DataSeq receive base is `0x0aaa`: UDP receive/ACK accounting
continues advancing while channel-stream delivery is permanently stalled.

### Cryptographic confirmation of the sequence gap

Further offline analysis reassembled the captured server channel bodies,
deduplicating retransmissions and joining the observed `0xffff -> 0x0001`
transition. This yielded 65,752 unique channel packets, with the sole absent
extended channel number 65,536, and a 62,116,830-byte TLS stream.
All 62,883 TLS records have valid framing with no leftover bytes.

Using the saved session secrets, an isolated Python/OpenSSL verification
authenticated **all 173 TLS 1.3 AES-256-GCM records spanning and following
the sequence gap**, including the record that straddles the last delivery
offset, 61,857,465. No payload byte was inserted, removed, or changed.
No decrypted desktop content or secrets were printed or saved by this check.

This establishes that the apparent missing channel `0x0000` does not
represent missing stream bytes in this session. The observed peer numbering
and the client's expectation disagree at the wrap. The existing client
waits indefinitely while complete, authenticatable data is buffered.

Important qualification: Microsoft's published
[sequence-number rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/ea4b6ac5-f574-4631-89a0-42a98ef78551)
describe transmitting the lower 16 bits of a wider sequence number. The
[DataBody rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/ee2c60fe-3869-416b-a7f1-a23741588c71)
refer channel-number reconstruction to that procedure. These references do
not establish a universal rule that zero is reserved. Microsoft's public
test-suite handler also uses plain `ushort` increments. A fix should
therefore address the demonstrated peer behavior with regression coverage,
without assuming that every peer always skips zero or discarding a real
zero-numbered payload.

Saved checks: `check-channel-stream.py`, `verify-tls-continuity.py`, and
`tls-continuity-result.txt` in the evidence directory. The extracted encrypted
payload TSV and reconstructed TLS stream are temporary files under
`/tmp/freerdp-stale-20260916/`. Production source was unchanged during diagnosis;
the subsequent fix is described below.

## Artifacts

Repository-local evidence directory:
`tools/udp-review-tests/logs/stale-20260916/`

- `sample-5742.txt`: complete live thread sample.
- `client-at-sample.log`: client log snapshot.
- `replay.py` and `replay-result.txt`: reproduction and observed state.
- `final-server-packets.txt`: decoded heartbeat and dummy probe.

Full capture snapshot and packet metadata:
`/tmp/freerdp-stale-20260916/` (temporary storage).

Original ongoing session, logs, capture, and TLS secrets:
`/Users/david/rdp-debug/20260916-200524-5735/`.
TLS secrets were not copied into the repository.

## Follow-up

Correct channel-sequence wrap handling for the observed Windows behavior
and add a regression that crosses `0xffff` with reordered/retransmitted
packets. Separately, track receive-stream progress so a transport that
still exchanges heartbeats cannot hide a stuck reassembly stream. A simple
"no graphics for N seconds" timer would also flag legitimately idle desktops.
No production source change or session restart was performed in this
diagnostic task.

## Follow-up: requested forced reconnect

The user subsequently requested a forced transport interruption. TCP fd 42
was verified as the DavidPC connection, and LLDB was invoked to call
`shutdown(42, 2)` and detach. Attachment waited for macOS developer-tools
authorization. It ultimately failed with:

> attached to process, but could not pause execution; attach failed

The expression was never executed. By the next check PID 5742 and its
launcher had exited; the capture wrapper recorded stop time
20:40:43.221102. No automatic reconnect was observed. The client log has no
final disconnect explanation. Subsequent macOS log inspection strongly
implicates the failed debugger attachment, as detailed below; the exact
terminating signal remains unconfirmed.
No firewall or system network setting was changed. Restoring the connection
now requires relaunching `free-rdp-debug.py` and entering the RDP password
at its terminal prompt.

### macOS exit evidence inspected at 21:10

- 20:40:42.832345: the capture still shows a client TCP ACK in response to
  the server's 44-byte TLS record. No preceding TCP FIN/RST appears in the
  inspected final capture window.
- 20:40:42.950: debugserver successfully acquires the task for PID 5742.
- 20:40:42.951: `ptrace(PT_ATTACHEXC, 5742)` returns success (`0`), followed
  immediately by the "could not pause execution" error and debugserver exit.
- 20:40:42.953: launchd marks FreeRDP's service inactive and its IPC
  connections start reporting process death, about two milliseconds later.
- 20:40:42.991–42.992: the kernel reports the socket closing locally with
  `so_error: 0`, and runningboard records FreeRDP's process exit.
- 20:40:42.996: launchd explicitly SIGKILLs the video decoder helper
  **PID 5756** during teardown after its host exited. This is not a recorded
  SIGKILL of FreeRDP PID 5742 and must not be cited as its exit signal.

The close timing strongly supports the failed debugger attachment/teardown
as the trigger for the client exit. It does not support a preceding remote
disconnect. No FreeRDP crash report or explicit exit signal/status was found
in the inspected diagnostic reports and unified logs. The intended
`shutdown` expression never ran. The earlier UDP receive-stream stall is a
separate, independently reproduced failure.

Saved `macos-exit-excerpt.log` and `exit-tcp.tsv` alongside the original
thread sample and replay evidence. Full queried logs remain under
`/tmp/freerdp-stale-20260916/`.

## Sequencing fix and regression

The receive path now handles an omitted channel zero **only when the entire
received DataSeq history is accounted for**. At the wrap, if channel zero is
absent, channel one is buffered, and there are no outstanding or discarded
DataSeq gaps, reassembly continues at channel one and logs the compatibility
decision. A normal zero-numbered body still delivers in order. DataSeq and
outgoing channel counters keep their existing modulo-65536 behavior.

The proof depends on original channel sends being ordered: if a real channel
zero existed, its original DataSeq would precede channel one's original send.
Consecutive DataSeqs on the two boundary packets alone are insufficient because
both could be retransmissions. A permanent history flag records missing
datagrams discarded by receive-window sliding or a late first AOA, as well as
discarded channel buffers. Such ambiguity disables this inference. Initial
probe-epoch adoption remains supported. Dummy and duplicate arrivals also
retry delivery because either can close the final DataSeq gap.

The captured session meets these conditions: all 68,166 inbound DataSeqs are
accounted for, none were discarded by window sliding, and the largest reorder
was eight packets.

`TestRdpeUdp` now exercises twelve wrap scenarios through the production
receiver, including the captured boundary, simultaneous DataSeq wrap, a real
zero, reordered packets, lost zero with retransmission, consecutive boundary
retransmissions, dummy/duplicate gap closure, ordinary channel gaps, window
sliding, and late AOA. Checks include exact delivered bytes, not just lengths.
The new captured-wrap regression failed before the fix at channel `0001`
with `next=0000 len=65535`, and passes afterward.

Offline capture replay with the fix produces:

| Frame | Channel sequence | Next expected | Delivered TLS stream bytes |
| --- | --- | --- | --- |
| 313336 | `0xffff` | `0x0000` | 61,857,465 |
| 313337 | `0x0001` | `0x0002` | 61,858,695 |
| 316801 | `0x00d9` | `0x00da` | 62,116,830 |

All 62,116,830 delivered bytes exactly match the independently reassembled
stream whose TLS records around and after the gap were authenticated above.
SHA-256: `a64c232cd22183ace7fe3a2763fcd6f97ab520071cd8b5fa64f2d7a68b6a649e`.
The same replay passed with the normal VideoToolbox client library. The SDL
client was rebuilt for the next launch; no live session was restarted.

Validation commands:

```sh
cmake --build /tmp/freerdp-udp-sequence-build --target TestCore --parallel 8
ctest --test-dir /tmp/freerdp-udp-sequence-build/libfreerdp/core/test --output-on-failure
cmake --build build/videotoolbox --target sdl3-freerdp --parallel 8
python3 tools/udp-review-tests/logs/stale-20260916/replay.py \
  --expected-stream /tmp/freerdp-stale-20260916/channel-stream-skip-gap.bin
```

The four core tests pass. An earlier top-level CTest name filter also selected
WinPR's separate `TestVersion`, whose executable had not been built; the final
run targets the core test directory explicitly. Saved results are
`regression-before.txt`, `regression-after.txt`, and `replay-fixed.txt` in the
evidence directory.

Limitation: if receive history has already lost a datagram, a skipped zero
cannot be distinguished safely from a lost real zero, so the receiver keeps
waiting. This fixes the demonstrated stall; a general stalled-stream watchdog
is still separate follow-up work.

## Recurrence in an already-running old process, 21:47:42

The next freeze was inspected at 21:55–21:58. PID 21738 started at
**21:33:45**, before the fixed client/library rebuild at **21:44:47–49**.
Its sampled loaded library UUID is
`77BABB68-766E-389D-9E6B-E8211E70F0DB`, identical to the library in the
original frozen PID 5742. The fixed library on disk has UUID
`7D13AB7E-0195-3241-9006-855EC279A31F`. Loaded versus on-disk inode numbers
also differ. This session therefore never loaded the fix.

The new capture independently repeats the same numbering failure:

| Frame | Time (CEST) | DataSeq | ChannelSeq |
| --- | --- | --- | --- |
| 311286 | 21:47:42.234327 | `0x05b4` | `0xffff` |
| 311291 | 21:47:42.281336 | `0x05b5` | `0x0001` |
| 311293 | 21:47:42.327545 | `0x05b6` | `0x0002` |

No channel zero appears. All 67,093 received DataSeqs are accounted for;
the largest reorder is twelve packets, with no window-discarded gaps.
TCP remains established. A three-second read-only stack sample shows the
UI waiting normally and the RDP thread mostly waiting for events, with the
remaining time polling UDP/TLS. There is no sampled deadlock and no logged
transport-disconnect error; input continues to report connected/unsuspended.

An offline model of the original channel-ordering logic stops at channel zero
after 67,362,193 bytes, buffering eleven later channels. Replaying the same
capture through the **actual fixed VideoToolbox library** reaches expected
channel `0x000c` and delivers 67,363,478 bytes, matching independent channel
reassembly byte for byte (SHA-256:
`5297541d167a56363c3b579849cd1069ee4e961ab4899e2e67a6eb61ca68e46b`).
This recurrence is covered by the existing fix.

Session files: `/Users/david/rdp-debug/20260916-213345-21717/`.
Snapshot and replay script: `/tmp/freerdp-refreeze-20260916-2155/`.
Stack/replay results are preserved as `recurrence-213345-sample.txt` and
`recurrence-213345-replay.txt` in the evidence directory. No debugger was
attached, no signals were sent, and the live session was left running.
A full client relaunch is needed to load the rebuilt library; reconnecting
inside the existing process cannot load it.

## Follow-up on September 17

The next day's session loaded the fix and crossed two wraps successfully,
then reproduced its documented limitation after a recovered packet loss.
The third wrap stalled at 09:18:05; Windows continued heartbeats until a
provider-initiated disconnect at 09:22:37. See
[the September 17 investigation](sleep-report-20260917.md) for the production
receiver replay and the limits of what this establishes about reported sleep.
