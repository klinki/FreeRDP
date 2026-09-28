# Office capture: UDP wrap recovery

Input: `~/rdp-office-debug/20260923-173146-92276/`, all six rotated captures,
merged chronologically. Analysis performed 2026-09-26. Baseline fix branch:
`bcc863bf0` (before this follow-up).

## Finding

The session contains six first-channel-wrap stalls. The server sends
`ffff -> 0001` in each affected UDP connection. There is no channel-zero payload
in these streams. Omitting the zero label during independent reassembly produces
streams whose TLS 1.3 authentication succeeds through the end: 317,111 encrypted
records across 441,190,044 stream bytes. This proves absence of missing bytes in
these six recorded wraps, rather than inferring absence from elapsed time.

Replaying the original datagrams through `bcc863bf0` reproduces all six stalls.
The permanent `recvSequenceHistoryLost` flag treats any old discarded DataSeq as
potentially containing channel zero, even when that loss was recovered thousands
of channel packets earlier. That conservative fix prevented corruption but did
not resolve the office-session failures.

The final recorded wrap occurs at 18:17:03.197; the client log reports an expected
channel of `0000` and a watchdog failure at 18:17:13.672, before the freeze snapshot.

## Fix and evidence

The receiver now records the channel label beside each DataSeq receipt and
processes those labels in DataSeq order. It keeps an upper bound on the original
channel number the sender could have introduced before the first wrap:

- A known data packet raises the bound to at least its channel label.
- A known dummy packet does not raise it.
- An unseen datagram could introduce at most one next channel, so it raises the
  bound by one. If that could cross zero, zero remains uncertain.

Retransmission does not introduce a new channel label. Therefore older recovered
loss can be ruled out as the original transmission of zero. A missing zero near
the wrap remains protected. The check requires a contiguous current DataSeq
window, and it does not use a time or buffered-packet threshold.

This follows the distinct DataSeq/ChannelSeq behavior documented in Microsoft's
[DataBody payload specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/ee2c60fe-3869-416b-a7f1-a23741588c71):
retransmissions use a new DataSeq while retaining the same channel label. Channel
ordering and wrap arithmetic are described in
[Sequence Numbers](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/ea4b6ac5-f574-4631-89a0-42a98ef78551).

The fixed native receiver matches the independently reconstructed, authenticated
stream byte-for-byte for all six affected connections:

| UDP flow | Wrap time (Europe/Prague) | First post-wrap frame | Recovery delay from that frame | Stream bytes | Authenticated TLS records |
|---|---|---|---|---|---|
| 0 | 17:34:13.715 | 143148 | 0.000 ms | 77,626,071 | 51,105 |
| 2 | 17:35:44.695 | 279188 | 0.000 ms | 78,523,372 | 51,367 |
| 4 | 17:46:20.816 | 456461 | 0.000 ms | 72,694,506 | 53,294 |
| 6 | 17:54:33.712 | 625698 | 0.000 ms | 72,877,972 | 52,795 |
| 8 | 18:00:11.855 | 778689 | 17.408 ms | 75,806,708 | 51,776 |
| 10 | 18:17:03.196 | 977342 | 0.000 ms | 63,661,415 | 56,774 |

Five wraps recover on the first post-wrap packet. Flow 8 recovers 17.408 ms later,
when the receive window advances past older lost DataSeqs and the channel bound
rules them out. No reconnect or speculative timeout is needed in these replays.

All thirteen captured flows match their independently reassembled encrypted
bytes with no native transport failure. Eleven authenticate to the end; two short
handshake-only flows (3 and 9) do not authenticate using the available key log.
They are excluded from the six-wrap authentication claim and regression assertion.

## Regression checks

- Added a first-wrap test after recovered loss: progress occurs on channel 1,
  without another packet or timer, and remains healthy after ten seconds of silence.
- Added a case losing both original `ffff` and zero, then retransmitting `ffff`:
  zero must remain pending even after those DataSeq holes leave the ACK window.
- Existing cases still preserve zero after 251 ms, 64 later chunks, reordering,
  retransmission, and late AOA; receive-window overflow remains terminal.
- The genuine missing-zero silence fixture now explicitly loses the original
  zero's DataSeq. It still tests watchdog wakeup and reconnect cleanup.
- The complete UDP test, eight selected CTest checks, the SDL client build, and
  the preserved review reproduction runner pass.
- `analyze_office_udp.py --expect-wraps 6` passes against the actual capture.

No new live RDP session was run. The fix is verified against saved real traffic
and synthetic loss cases; genuinely ambiguous lost bytes still fail safely.

## Reproduce and retained artifacts

The script reads secrets only to authenticate the offline reconstruction. It
never prints or saves keys or decrypted desktop data. Its packet cache is private
and contains the original encrypted datagrams. None of that data is committed.

```sh
REVIEW_SOURCE=/path/to/fixes-worktree
REVIEW_BUILD=/path/to/matching/cmake-build
CAPTURE_DIR="$HOME/rdp-office-debug/20260923-173146-92276"
ANALYSIS_DIR="$REVIEW_BUILD/office-capture"
mkdir -p "$ANALYSIS_DIR"
(umask 077; mergecap -w "$ANALYSIS_DIR/merged.pcapng" "$CAPTURE_DIR"/capture_*.pcapng)
python3 -B "$REVIEW_SOURCE/tools/branch-review-20260926/analyze_office_udp.py" \
  --capture "$ANALYSIS_DIR/merged.pcapng" \
  --keys "$CAPTURE_DIR/tls-secrets.txt" \
  --library "$REVIEW_BUILD/libfreerdp/libfreerdp3.dylib" \
  --output "$ANALYSIS_DIR/analysis" --expect-wraps 6
```

Add `--reuse-packets` on subsequent runs to reuse the tshark packet export.
The key file and native library must match the capture and source respectively.
The assertion deliberately fails on the baseline branch.

Local retained artifacts: `/Users/david/projects/FreeRDP/build/review-fixes/office-capture/`.
`baseline-report.json` and `baseline-analysis.log` preserve the failing run;
`analysis/report.json` and `fixed-analysis.log` preserve the passing run.
Build, CTest, and focused reproduction logs are in the same directory.
