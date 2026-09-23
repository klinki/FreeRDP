# Office reconnect investigation — 2026-09-23

Two distinct findings from today's live office session. No code changes;
all evidence is read-only (packet captures, client log, one `sample` thread
dump). The live session was never disturbed.

## Session

* Launcher: `free-rdp-office-debug.py` (office copy of `free-rdp-debug.py`)
* Evidence: `~/rdp-office-debug/20260923-173146-92276/`
  (`capture_0000{1,2,3}.pcapng`, `tls-secrets.txt`, `client.log`,
  `freeze-181746/`)
* Server `100.102.53.29` (davidpc) via Tailscale (`utun10`), office monitors
  PHL 288P6L (ID 2, RDP primary) + 231PQPY (ID 3), 5760x2160, AVC444
* Binary: home-repo `build/videotoolbox` at `d5a349249`
  (same base as `feat/performance-macos-native`)
* Forensics script (private, not committed): `/tmp/overflow-forensics.py`
  — parses cleartext RDPEUDP2 headers, no TLS keys needed for sequence work

## Finding 1: forced reconnect at every channel-sequence wrap (proven)

**Timeline.** Three identical events — 17:34:13, 17:35:44, 17:46:21:

```
[..] [ERROR][com.freerdp.core.rdpeudp] - [rdpeudp_test_feed]:
       UDP channel receive window overflow; reconnect required
[..] [ERROR][com.freerdp.core] - [rdp_check_fds]:
       ERRCONNECT_CONNECT_TRANSPORT_FAILED [0x0002000D]
... Attempting reconnect (0 of 5) -> ERRINFO_RPC_INITIATED_DISCONNECT ...
... Attempting reconnect (1 of 5) -> Logon Extended Info (success, ~+24s)
```

Each costs ~24s: attempt 0 always dies to the server racing the dying
session (`RPC_INITIATED_DISCONNECT` is the server swatting our own
reconnect, not an admin action); attempt 1 lands ~19s later.

**Packet proof.** Both wraps reconstructed datagram-by-datagram:

* Event #1: session ran channelSeq 6…65535 over ~130s (~500 chunks/s),
  then stalled. Exactly **one** cseq-0 datagram exists in the whole
  65,530-datagram session (at session start, handshake artifact) — the
  server **omits 0 at wrap**, standard MS convention (our own replay
  extractor already special-cases it as `omitted_zero_labels`).
* Event #2: session reached 65535 at 17:35:44 with **zero** cseq-0
  datagrams anywhere in the 2-minute window.
* Both sessions show ordinary WiFi loss, fully recovered: 13 DataSeq gaps
  (e.g. 3359→3366, 12967→12974) event #1, ~9 event #2.

**Mechanism** (`libfreerdp/core/rdpeudp.c`):

1. Channel reassembly is a fixed 256-slot ring (`RDPEUDP2_RECV_MAP`, :945),
   strictly in-order. The wrap lands `expectedChannelSeq` on 0, which never
   arrives; 256 later chunks buffer up and the ring laps itself → forced
   reconnect (:2061).
2. A zero-skip exists (`rdpeudp_can_skip_channel_zero_locked`, :1821) but
   fires only with pristine all-time DataSeq history. Every recovered loss
   permanently sets `recvSequenceHistoryLost`
   (`rdpeudp_slide_recv_data_locked`, :1761/:1764) → skip refused. The
   "omitted channel sequence 0" WARN **never appears** in the log.
3. Chicken-and-egg: `peerSkipsChannelZero` (the trust flag) is per-session
   and can only be learned through a successful skip — so every session's
   first wrap fails. At video rates that is roughly every 2 minutes of
   heavy traffic; at office activity it was every ~10–17 minutes.

**Fix directions** (not implemented): accept DataSeq continuity *across the
wrap boundary* as proof of omission instead of demanding all-time pristine
history; or remember the zero-omission convention per host across
reconnects. The comment on the gate already argues channel-1-buffered is
sufficient proof — the extra history conditions are what misfire on lossy
WiFi.

## Finding 2: frozen "Reconnecting…" — UI thread wedged in join (diagnosed)

**Evidence** (`freeze-181746/`, 5s `sample` of PID 92285 at 18:17:46):

* Main thread, all 4053 samples:
  `main → freerdp_client_stop → sdl_client_stop → std::thread::join →
  __ulock_wait`. It is not rendering and not pumping events.
* RDP thread (`SdlContext::rdpThreadRun`), 3957/4053 samples:
  `WaitForMultipleObjectsEx → poll`. Alive, parked, never exiting —
  so the join never completes.
* Everything else idle (channel threads in `MessageQueue_Wait`,
  workqueues, VTDecoder). Process CPU 11:44 over 46:06 elapsed (~25%).

**Fingerprint.** The "Reconnecting…" dots normally animate via ~1s overlay
repaints from the event loop. A static "..." means the UI thread is not in
its loop — it distinguishes this join stall from a saturated renderer
(where the dots would still dance between frames).

**Context.** This coincided with the day's 6th reconnect (18:17:13, itself
a plain TCP `rdp_check_fds() - -1` drop, not the wrap bug). Attempt 1
started (decoder inits, UDP tunnel thread at 18:17:30–31) but produced no
`Logon Extended Info`, then 2.5+ minutes of total log silence with both
threads parked — wedged with high confidence. Residual chance: the RDP
thread sits in `poll()`, so a stray socket event could still wake it,
release the join, and unfreeze the UI. Clearing the handshake
(`sdl_client_stop` vs `rdpThreadRun` lifecycle) is the follow-up.

## Secondary observation: plain TCP drops (not the wrap bug)

17:54:44, 18:00:22, 18:17:13: `rdp_check_fds() - -1` → "Network
disconnect!" → 15s retry dialog → reconnect. No overflow error, no
channelSeq anomaly — the TCP path itself dying (office WiFi/Tailscale
hiccup or server-side kill). Distinct cause, same user-visible pain;
worth correlating against Tailscale status if it recurs.

## What is proven vs open

* Proven: wrap + omitted-zero + dirty-history → refused skip → overflow
  (packet-level, twice); join stall + static-dots fingerprint (dump-level).
* Open: the exact loss source on the office path (WiFi vs Tailscale);
  whether the 18:17 join stall self-resolves; the three fix directions
  above are proposals, not patches.
