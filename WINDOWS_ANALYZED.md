# Windows reference capture analysis

Source: `~/Downloads/rdp-udp-win-win.pcapng` (pktmon, Windows), 5956 raw /
2978 deduplicated datagrams, ~40 s healthy session. Cleaned IP-level copy
recipe: parse pcapng EPBs, take packets starting with `0x45`, drop consecutive
identical datagrams (pktmon per-layer duplicates), write classic pcap
`LINKTYPE_RAW`. Endpoints: MS RDP client `100.102.53.29` (DavidPC) egress
`62321` → server `100.116.46.21` (SNI `david-t14-amd`) port `3389`.
Wireshark native `rdpudp` dissector used throughout.

## Handshake (all bulk sizes in UDP payload bytes)

- C→S SYN, 1232 B: `snSourceAck=0xFFFFFFFF`, window **64**, flags `0x1801`
  (SYN|CORRELATIONID|SYNEX), random `initialSeq`, MTU 1232/1232, 16 B
  correlation ID + 16 B zero reserved, SYNEX v3 (`0x0101`) + 32 B cookie hash,
  zero-padded to 1232.
- S→C SYN+ACK, 1232 B: acks client seq, window 64, flags `0x1005`
  (SYN|ACK|SYNEX), **no correlation ID echoed**, SYNEX v3, no cookie hash.
- No standalone 12 B v1 final ACK observed: first DATA follows SYN+ACK
  directly (~5 ms later).

Ours for comparison: window 128 (vs 64), SYN+ACK echoes correlation ID
(MS server does not), 12 B final ACK (accepted per 81 s of no server
retransmit, but evidently not required by all servers).

## First DATA (C→S, 452 B payload), fully decoded

Wire: `00 14 f3 01 14 00 64 e0 | 64 00 01 00 | 16 03 01 ...`
After byte-7 swap: prefix **`0xE0`**, header `0xF314` → flags `0x314`
(DATA + AOA + DELAYACK + reserved bit `0x200`, meaning unknown),
`logWindow` **15**, DelayAck max=1 timeout=20 ms, AOA=100,
**DataSeq=100**, **ChannelSeq=1**, then 440 B TLS ClientHello
(record len 435, TLS 1.2 with 20 suites, SNI, `supported_versions`).

Follow-up C→S ACK-only packets (10–22 B) also carry prefix `0xE0`.

## The prefix-byte finding (root cause of our server silence)

- MS prefix for normal packets is always `0xE0` = `1110_0000`.
  Layout MSB-first: C(3)=shortLen | B(4)=type | A(1)=0 →
  shortLen=7, type=0 (Data), A=0.
- We emitted `0x07` (`(type<<3)|shortLen`, nibbles transposed).
  Type field per both dissectors (mask `0x1E`, ours in
  `tools/wireshark/rdp-udp.lua` included) sits in bits[4:1]:
  MS `0xE0` → type 0 (Data); ours `0x07` → type **3 (reserved)**.
- Consequence: Windows silently drops every UDP2 datagram while the v1
  handshake (separate codec) succeeds — matching all observations (zero
  server replies across flag/seq variants, no SYN+ACK retransmits, green
  self-tests: our parser leniently accepts anything and our encoder
  disagreed with our own dissector).
- Fixed in `6eab8283f`: `rdpeudp2_make_prefix` emits
  `(shortLen<<5)|(type<<1)` (DATA `0xE0`, dummy `0xF0`); `unprotect`
  reads type from `(prefix>>1)&0xF`. `TestRdpeUdp` pins absolute
  `0xE0`/`0xF0` plus an MS frame-3 interop fixture. Live verification
  against DavidPC pending at time of writing.

## Secondary observations (not blocking)

- MS `DataSeq` starts at **100** (either start value is accepted server-side;
  consistent with our fixed-base receiver, V1).
- MS `ChannelSeq` starts at **1** (matches our fix; original 0-start was wrong).
- MS ClientHello flight fits one datagram (~440 B); ours is 1538 B over two
  UDP2 chunks. UDP-layer ACKs must not depend on TLS record completeness,
  but if silence persists after the prefix fix this is the next suspect to
  isolate (force a smaller hello vs chunked flight).
- Hypothesis only: the S→C ~1007 B flood with `00 14 f2 ...` framing looks
  like the lossy (DTLS/FEC) channel, which we do not implement; reliable
  channel behavior is independent of it.
- `wlog` DEBUG delivery still unresolved (correct full-name filters verified
  working in isolation via `/tmp/wlogtest`, yet silent in `sdl-freerdp`;
  diagnosis used temporary ungated traces, since reverted). Parked.

## Adopted into code (and deliberately not)

- Adopted: DelayAck hint max=1, timeout=20 ms in both constructors (was 2/50);
  matches the observed values and our prompt-ACK behavior.
- Adopted: send-window additionally capped by `sent[]` capacity — MS peers
  advertise logWindow 15 (2^15) while `sent[]` holds 64; the old
  `MAX_LOGWINDOW`-only cap could admit more in flight than slots exist.
- Not adopted: DataSeq start 100 (product quirk; reference SDK uses 1 and
  servers accept any start — our fixed-base receiver already handles both).
- Not adopted: first-DATA AOA=100 (no semantics before any ACK is received;
  reference SDK omits AOA until there is something to acknowledge).
- Not adopted: reserved flag bit `0x200` (unknown meaning; MUST-ignore on
  receipt — setting unknown bits is never correct).
- Not adopted: SYN window 64 (ours 128 is honest for our buffers; both valid).
- Not adopted: advertising logWindow 15 (would promise 2^15 buffering we do
  not have; our 5 stays, MS's 15 is ignored for flow control as before).
- Not adopted: skipping the standalone final ACK (ours is accepted per the
  81 s no-retransmit evidence; MS's omission is their quirk).
