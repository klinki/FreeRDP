# UDP implementation review

Date: 2026-09-05
Re-reviewed: 2026-09-05 (commit `a9eb02727`)
Latest review: 2026-09-06 (HEAD `11c4f822b`)
Review result: accept queue-blocking and legacy send envelope fixed; two remaining issues Q1/Q2 below. P3 harness API mismatch repaired by this review.
Implementor update 2026-09-06: N1, P1, P2 FIXED in the working tree (headers tagged,
status paragraphs below); P3 deferred to reviewer (owner decision, `tools/` untouched);
live VM loop re-ran healthy after the fixes.
Implementor update 2026-09-06 (Q-round): Q1 FIXED in the working tree (whole-PDU send,
uncommitted); P2 walked back to partial with a contested-remedy dispute recorded under
Q2; P3 direction correction accepted (reviewer's `isServer=TRUE` matches the codebase
convention — my suggested FALSE was wrong-axis).

Latest scope: `9fbe4fb10..11c4f822b` (two commits), plus a separate working-directory inspection. The interim progress report was not a code review. Earlier sections retain historical findings and line numbers.

The original review identified eight P1 correctness issues. Re-review of commit
`a9eb02727` found three blocking issues: an encoder/protector integration regression,
incomplete Soft-Sync handling, and loss of partial tunnel receive state.

Commit `1354c6e2a` fixes T1's map-capacity and fragmented-request failures.
Commit `6fefaf21e` fixes U1 by sharing request validation between the DVC
handler and multitransport layer. The server also uses the shared response
helper. The review harness confirms that the handler rejects both historical
trailing-byte fixtures before sending a response.

The original buffer-length, header-decoding, invalid-free, worker-join, and socket
readiness fixes are present. Earlier sections preserve the review history; the
latest findings and validation take precedence over earlier fix descriptions.

## 1. [P1] [FIXED] Initialize and maintain the receive stream's actual length

Location: `libfreerdp/core/rdpeudp.c:857`, with the same initialization in `rdpeudp_accept` and append logic around line 1293.

`Stream_New(nullptr, 65536)` sets both capacity and length to 65536. Consequently, `rdpeudp2_recv_reliable` initially delivers unwritten zero bytes to TLS. After the buffer is drained, its length becomes zero, but subsequent `Stream_Write` calls do not update that length, making received data invisible to the reader.

Initialize the length to zero in both constructors and seal or update it after appending received bytes.

**Status: FIXED.** Both constructors (`rdpeudp_new`, `rdpeudp_accept_ex`) now do
`Stream_SetPosition(...,0)` + `Stream_SetLength(...,0)` after `Stream_New`, and the
in-order delivery loop calls `Stream_SealLength(recvStream)` after each append batch
so `Stream_Length - recvPos` reflects published bytes.

## 2. [P1] [FIXED] Parse the header without requiring the unread payload

Location: `libfreerdp/core/rdpeudp.c:2211`; parser validation at lines 310–313.

The tunnel creation code passes only four header bytes to `rdpemt_parse_header`, but the parser rejects `len < headerLength + payloadLength`. A valid Create Response therefore always fails because `4 < 4 + 4`. The server accept path and every nonempty Tunnel DATA receive have the same mismatch.

Separate header decoding from complete-PDU validation so callers can decode lengths before reading the payload.

**Status: FIXED.** New `rdpemt_decode_header` decodes the 4-byte base header only
(action/plen/hlen, `hlen >= 4`); `rdpemt_parse_header` keeps full-PDU validation on
top of it. `rdpeudp_tunnel_create`, `rdpeudp_tunnel_recv_full`, and the accept path
use decode-then-read-payload. Covered by header-only decode assertions in
`TestRdpeUdp:test_autodetect_framing`.

## 3. [P1] [FIXED] Avoid freeing the stack-backed autodetect payload

Location: `libfreerdp/core/multitransport.c:738`, with release at line 758.

`pdu` points inside the local `buf` array. `Stream_New` marks the supplied buffer as owned, and `Stream_Release` subsequently calls `Stream_Free(s, TRUE)`, attempting to free that stack address. Any autodetect packet reaching this path can crash the process.

Use `Stream_StaticConstInit` or otherwise make the wrapper explicitly non-owning.

**Status: FIXED.** `multitransport_check_fds` autodetect dispatch now wraps subheader
bytes with `Stream_StaticConstInit` and performs no `Stream_Release` on the wrapper.

## 4. [P1] [FIXED] Join the worker before freeing its state

Location: `libfreerdp/core/multitransport.c:512–525`.

The five-second wait result is ignored, and `multitransport` is freed even when the worker remains alive. The accept worker can wait 30 seconds for a SYN, then access `multi->lock` and `multi->closing` after their destruction. Closing the thread handle does not terminate the worker.

Add cancellation to establishment and ensure worker termination before freeing shared state.

**Status: FIXED.** `multitransport` owns a manual-reset `abortEvent` (signaled in
`multitransport_free` before join) plus `closing` flag; free waits `INFINITE` on the
worker before destroying `lock`/transport/events. `rdpeudp_set_abort_event` hands the
event to the transport; SYN/handshake, ACK-wait, send-window, reliable send/recv,
TLS send/recv, and both accept waits poll `udp_aborted`/`accept_aborted` (and
`freerdp_shall_disconnect_context`) so the 30s SYN wait exits promptly.

## 5. [P1] [FIXED] Connect the receive event to socket readiness

Location: `libfreerdp/core/rdpeudp.c:849`; event reset at `libfreerdp/core/multitransport.c:772`.

`udpEvent` is an ordinary manual-reset event, with no socket association or background receiver. It is signaled only after `rdpeudp_recv_one` has already read a datagram. Once `multitransport_check_fds` resets it, subsequent UDP arrivals cannot wake the main loop. Incoming traffic therefore stalls until unrelated activity triggers polling.

Expose socket readiness and wake the main loop when the asynchronously created transport becomes available.

**Status: FIXED.** Transport owns a WSA `sockEvent` attached via
`WSAEventSelect(FD_READ|FD_CLOSE)` on connect and accept-connect, signaled on every
received datagram. `rdpeudp_get_event` prefers `sockEvent`; `multitransport_get_event`
falls back to `stateEvent` (signaled on async establishment completion) when no
transport exists yet. `rdpeudp_update_event` (called from `multitransport_check_fds`)
does reset + non-blocking `wait_readable(0)`/buffered-bytes recheck + re-set, so no
wakeup is lost between drain and reset.

## 6. [P1] [FIXED] Parse ACKVEC before the DATA body

Location: `libfreerdp/core/rdpeudp.c:1243–1250`; corresponding encoder in `rdpeudp2_build_packet_ex`.

When DATA and ACKVEC are both present, the decoder reads `channelSeq` immediately after `dataSeq` and copies all remaining bytes into the TLS stream. The specified layout places ACKVEC between DataHeader and DataBody; `channelSeq` belongs to DataBody. Valid combined packets therefore corrupt TLS input, and the later ACKVEC parser reads the wrong bytes.

Correct both the encoder and decoder field order.

Reference: [Microsoft: RDP-UDP2 packet layout](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/98f1d4e7-b9ee-4434-97c0-72c88fdfaf27).

**Status: Field order and integration fixed (see R1 below).** Pure `rdpeudp2_parse_layout`/`rdpeudp2_encode_layout` implement spec
order Header, ACK, OverheadSize, DelayAckInfo, AckOfAcks, DataHeader, ACKVEC, DataBody;
both the packet builder and `rdpeudp_recv_one` use them. New
`TestRdpeUdp:test_v2_data_ackvec_order` asserts exact wire bytes for a DATA+ACKVEC
fixture (`50 0C 11 11 22 22 01 01 33 33 41 42`) and rejects the old buggy order.

## 7. [P1] [FIXED] Encode UDP autodetect messages as tunnel subheaders

Location: `libfreerdp/core/rdpeudp.c:565–567`, with corresponding tunnel receive and autodetect dispatch paths.

The implementation introduces a private payload-type byte and security-flags prefix inside Tunnel DATA. UDP autodetect messages use `RDP_TUNNEL_SUBHEADER` framing, so a conforming server will not interpret these responses correctly. Conversely, the receive path skips subheaders and cannot dispatch the server's actual autodetect requests.

Implement the specified subheader encoding and dispatch.

Reference: [Microsoft: network characteristics detection rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/16ffa852-8aa7-481c-99a0-36c1a9a198f6).

**Status: FIXED.** Private `ptype/secFlags` framing removed
(`rdpeudp_build_autodetect_packet`, `rdpeudp_parse_autodetect_packet`,
`rdpeudp_parse_tunnel_ptype` deleted). New `RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ/RSP`,
`rdpemt_build_subheader` (`SubHeaderLength=2`), `rdpemt_build_tunnel_data`,
`rdpemt_autodetect_pdu_length`, `rdpemt_next_subheader`,
`rdpeudp_tunnel_send_autodetect`, `rdpeudp_tunnel_recv_full`. Send puts the autodetect
PDU in a subheader with empty HigherLayerData (TLS provides security, no RDP sec
header); recv splits subheaders (dispatched to `autodetect_recv_*` as `UDP_R`) from
HigherLayerData (channel). Legacy `rdpeudp_tunnel_recv` now fails if subheaders are
present so they cannot be silently dropped. Raw `subLen+payloadLen` counted via
`autodetect_account_udp_bytes` per MS-RDPBCGR 2.2.14. Covered by updated
`TestRdpeUdp:test_autodetect_framing`.

## 8. [P1] [FIXED] Negotiate channel migration before switching drdynvc

Location: `libfreerdp/core/channels.c:87–88` and the UDP routing block that follows.

Every `drdynvc` PDU switches to UDP as soon as the tunnel exists, without checking channel migration state or completing Soft-Sync. With a peer using Soft-Sync, tunnel establishment alone does not authorize this switch. Earlier TCP traffic can also remain in flight while later UDP traffic arrives.

Route channels according to negotiated migration state rather than tunnel connectivity alone.

Reference: [Microsoft: multitransport setup and Soft-Sync requirements](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpemt/02d833db-5c7c-4c15-a18e-179df6d8a34d).

**Status: FIXED (via S2 + T1/T2 below).** Migration state (`softSyncNegotiated/Complete`, `udpSend/RecvMigrated`)
with `is_udp_send/recv_migrated` gates (not mere connectivity). Without Soft-Sync,
migration latches only if the tunnel is ready pre-`ACTIVE` (post-`ACTIVE` completion
stays TCP-safe to avoid reordering in-flight TCP vs new UDP). With Soft-Sync,
send/recv enable via `on_soft_sync_request_sent/received`, `on_response_sent/received`.
Soft-Sync control PDUs (Cmd `0x08`/`0x09`) are forced TCP and snooped in `channels.c`
send/recv to drive the hooks; UDP recv pre-migration is ignored. Whole-PDU pinning
(no TCP/UDP split) and atomic-only `send_packet` UDP preserved.

## Re-review findings for `a9eb02727`

### R1. [P1] [FIXED] Restore the stream position before protecting the packet

Location: `libfreerdp/core/rdpeudp.c:1312–1316`.

`rdpeudp2_encode_layout` rewinds the stream to position zero, while
`rdpeudp2_protect` uses `Stream_GetPosition` as the packet length. Consequently,
the production send path replaces the encoded packet with padding, preventing
valid DATA and ACK packets from reaching the peer through this builder.

A standalone reproducer linked against the rebuilt libraries confirmed:

```text
Encoded: length=9 position=0
Protected: 00 00 00 00 00 00 00 00
```

Seek to the encoded length before protection, or make the encoder/protector
contract consistent. Add a test covering the complete encode/protect sequence;
the current layout fixture tests the encoder output without passing it through
the protector and therefore misses this regression.

**Status: FIXED.** `build_packet_ex` seeks to `Stream_Length` after `encode_layout`
(whose contract — position = length on protect input — is now documented in the
header) before `protect`. New `test_v2_encode_protect_roundtrip` covers the full
encode → protect → unprotect → parse path and asserts the wire bytes are not the
all-zeros regression output.

### R2. [P1] [FIXED] Implement Soft-Sync handling beyond command snooping

Location: `libfreerdp/core/channels.c:275–281`; downstream handler:
`channels/drdynvc/client/drdynvc_main.c:1637` (`drdynvc_order_recv`).

The new code detects a Soft-Sync request but still forwards it to
`drdynvc_order_recv`, which has no `SOFT_SYNC_REQUEST_PDU` case and returns
`ERROR_INTERNAL_ERROR`. No response is generated, so the response-sent migration
hook cannot run in the standard client. Merely adding migration booleans and
snooping the command nibble does not complete the handshake.

Implement request validation and response generation in the DVC layer, honoring
the requested channel/tunnel lists rather than switching the entire `drdynvc`
multiplex.

**Status: FIXED (see U1).** Client and server now handle Soft-Sync control PDUs.
However, the implementation skips DVC IDs instead of retaining them for routing,
and request validation is incomplete. The offer-helper tests do not establish
that complete handshake processing or per-channel selection works. See S2–S3.

Reference: [Microsoft: Soft-Sync request format](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/f82105dd-0abd-4126-a61b-41a7909e974f).

### R3. [P1] [FIXED] Preserve partially received tunnel PDUs across polling calls

Location: `libfreerdp/core/rdpeudp.c:2683–2688`, within
`rdpeudp_tunnel_recv_full`; partial-read behavior in `tls_recv_all` at line 2478.

`tls_recv_all` can consume a header or part of a payload and then return short on
timeout. `rdpeudp_tunnel_recv_full` discards that progress and returns `-1`;
`multitransport_check_fds` ignores the error and retries later with a fresh header.
If the remainder arrives after the polling deadline, payload bytes are interpreted
as the next header, desynchronizing the tunnel. Header-only decoding fixes the
original validation mismatch but does not solve incremental receive framing.

Retain header, subheader, and payload progress in transport state until the
complete PDU is available. Verify split headers and split payloads with timeouts
between fragments, followed by another complete PDU to check alignment.

**Status: FIXED (see S1 below for the zero-timeout completion).** `tunnelBuf`
retains incomplete PDUs. With zero timeout, the caller returns before the readiness
checks or TLS read and cannot populate that buffer. `test_tunnel_split` only calls
header/codec helpers with differently sized byte spans; it never calls
`rdpeudp_tunnel_recv_full` or exercises timeouts and persistent receive state.

## Review findings for `02ec5a114`

### S1. [P1] [FIXED] Allow a zero-timeout poll to read available TLS bytes

Location: `libfreerdp/core/rdpeudp.c:2857–2859`.

`deadline` is initialized to `udp_now_ms() + timeoutMs`. With `timeoutMs == 0`,
the deadline check always returns before the non-blocking readiness checks and
`tls_recv_some`. `multitransport_check_fds` calls this function with zero, and a
new transport starts with an empty `tunnelBuf`. The normal event loop therefore
never reads or dispatches incoming tunnel PDUs. An unread socket can remain ready,
causing repeated wakeups without progress.

An isolated harness compiled the current `rdpeudp_tunnel_recv_full` body unchanged,
with a readable-socket stub and a TLS stub ready to supply a complete DATA PDU.
The zero-timeout call returned:

```text
zero-timeout result=0 TLS_reads=0 payload_length=0
```

Treat zero as a non-blocking read attempt rather than an already-expired wait.
Allow available bytes to enter the reassembly buffer before returning, preserving
partial data when no more bytes are immediately available. Add a test invoking
the actual receive function with zero timeout and ready data, including fragments
arriving on successive calls.

**Status: FIXED.** The deadline-expiry check is skipped for `timeoutMs == 0`, so a
zero-timeout call checks the reassembly buffer, peeks TLS-buffered/socket-readable
(fast non-blocking return when neither), does one `tls_recv_some` attempt, and
rechecks before returning 0/1 with progress preserved. Framing extracted into pure
`rdpemt_tunnel_consume` (1 = complete, 0 = need more, −1 = corrupt, −2 = buffers
too small), which `recv_full` uses for all buffer decisions; new
`test_tunnel_consume` feeds two back-to-back PDUs in uneven fragments through it,
asserting order, alignment, and corrupt rejection. A full `recv_full`-with-TLS
harness still needs live transport and remains future work.

### S2. [P1] [FIXED] Retain DVC IDs and apply migration per channel

Location: `channels/drdynvc/client/drdynvc_main.c:1729–1733`; related helper at
`libfreerdp/core/rdpeudp.c:593–595` and routing in `multitransport.c`/`channels.c`.

The handler reads `NumberOfDVCs` but skips every ID with the comment that all DVCs
migrate. The shared helper returns only a boolean, and the migration hooks still
enable the whole `drdynvc` multiplex. A request listing only DVC 7 on reliable UDP
therefore also enables UDP routing for unlisted channels. On the server send path,
those channels may still have TCP traffic in flight: the request only establishes
a transport-switch barrier for the listed channels.

Keep the channel-ID-to-tunnel mapping and use it when routing each DVC. If the
implementation cannot support a requested mapping, do not silently broaden it to
all channels. Add a test with one migrated DVC and another remaining on TCP;
checking only whether the request mentions UDPFECR is insufficient.

**Status: FIXED.** `multitransport` stores the UDPFECR-listed DVC IDs
(`udpDvcIds`, `mappingActive`; reset on tunnel setup/teardown) populated by
strict-validating request hooks; new `multitransport_is_dvc_migrated` gates each
send on direction-migrated AND (no mapping OR ID listed), with `dvc_get_id`
parsing CREATE/DATA_FIRST/DATA/CLOSE IDs in both send paths (unlisted/ID-less
stay TCP). Receive still dispatches arrived UDP data (no loss). New
`rdpeudp_soft_sync_request_udp_dvcs` extraction helper backs the mapping and is
unit-tested with a 2-tunnel fixture (`{7,8}` migrate, `9` on lossy does not).

References: [Microsoft: Soft-Sync channel lists](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/bcc93699-6e47-49a5-aa88-e7fd09b1128a),
[Microsoft: sending the Soft-Sync request](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/b64bcbe9-569f-4cec-906d-b02659aee043).

### S3. [P2] [FIXED] Validate the entire Soft-Sync request before enabling migration

Location: `libfreerdp/core/rdpeudp.c:575–594`; duplicate validation in
`drdynvc_process_soft_sync_request`.

The helper checks only `Length >= 8`, ignores the mandatory `TCP_FLUSHED` flag,
and returns true on the first UDPFECR list without validating the remaining lists.
Core snooping uses that result to change migration state before the DVC handler
processes the request. The handler also merely warns when `TCP_FLUSHED` is absent.
A malformed or incomplete request can therefore authorize switching transports
without a valid TCP completion barrier.

Direct calls against the rebuilt library confirmed that the helper returns true for:

- A request with `TCP_FLUSHED` cleared.
- A request whose declared length exceeds the supplied bytes.
- A request declaring two tunnels but containing only the first UDPFECR list.

Use one complete parser for both the DVC handler and migration decision. Require
the mandatory flag, validate the declared length and all list entries, then change
state only after successful validation. Add negative fixtures for these cases.

**Status: FIXED (shared parser; see U1).** The separate client-handler parser is
gone: the handler calls the shared utils validator, so the negative fixtures are
rejected identically everywhere. The former `soft_sync_request_parse` requirements
(`TCP_FLUSHED`, exact lengths, complete list parsing) now live in one place.

Reference: [Microsoft: mandatory Soft-Sync flags and channel lists](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/b64bcbe9-569f-4cec-906d-b02659aee043).

### Validation of `02ec5a114`

- `cmake --build /tmp/freerdp-build --target TestCore --parallel 4` succeeded.
- `ctest --test-dir /tmp/freerdp-build/libfreerdp/core/test -R '^(TestRdpeUdp|TestVersion|TestUtils)$' --output-on-failure` passed all three selected tests.
- The isolated receive harness confirmed S1. Direct calls to the rebuilt offer
  helper confirmed the malformed-request cases in S3.
- The production seek-to-length fix for R1 is present, and the added
  encode/protect round-trip test passes.
- No live peer, ASAN run, or full application build was performed. Only this
  document was changed in the repository. S1–S3 line numbers refer to `02ec5a114`;
  earlier findings retain their historical locations.

## Re-review findings for `444b698b3`

### T1. [P1] [FIXED] Require successful map installation before accepting migration

Location: `libfreerdp/core/multitransport.c:766–777`; related response hook at
lines 833–843 and single-chunk request snooping in `libfreerdp/core/channels.c:303–317`.

The request hook extracts IDs into a fixed 256-entry array, while the DVC handler
accepts up to 1024 IDs and independently sends an accepting response. For a valid
257-ID request, extraction fails and no map is installed. The response-sent hook
nevertheless enables both directions. `multitransport_is_dvc_migrated` interprets
`mappingActive == FALSE` as permission to migrate every DVC, including unlisted IDs.

Fragmented requests expose the same failure: core snooping requires both FIRST
and LAST on one static-channel chunk, but the DVC handler processes the reassembled
request and responds successfully. Thus no map is installed before the response
enables migration. This can occur with smaller lists when VCChunkSize is reduced,
or with longer lists exceeding the usual chunk size.

An isolated harness compiled the current request-received, response-sent, and
per-DVC routing functions unchanged, using the rebuilt extraction helper and a
connected-transport stub. A request listing IDs 1 through 257 produced:

```text
strict offers=1
map before response: active=0 count=0
unlisted DVC 999 migrated=1
```

Install the mapping from the fully reassembled, validated request before sending
an accepting response. Make installation success an explicit prerequisite for
the response hook; failed allocation, capacity overflow, or a skipped hook must
never become migrate-all. Allocate for the actual validated ID count or reject
unsupported sizes consistently. Test 257 IDs and a fragmented request, asserting
that an unlisted DVC remains on TCP.

**Status: FIXED.** New `mappingInstalled` flag (reset on tunnel setup/teardown)
is set only by successful installs; both response hooks require it, so a failed,
skipped, or never-run install leaves the session TCP-safe. Install uses two-phase
extraction (count, then exact-size `malloc`) — no fixed stack cap, so 257 IDs
install fully. New `soft_sync_recv_feed` reassembles FIRST..LAST drdynvc chunks
passively (single-chunk requests complete immediately; resync/64 KiB cap on
abuse) and installs + enables recv on the validated whole; the send path already
sees whole PDUs. The added unit cases test extraction of 257 IDs, incomplete input, and trailing
bytes. The separate review harness for `1354c6e2a` verifies actual map installation,
fragment feeding, and exclusion of unlisted IDs with transport stubs (see below).

Reference: [Microsoft: Soft-Sync channel selection](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/bcc93699-6e47-49a5-aa88-e7fd09b1128a).

### T2. [P2] [FIXED] Use the strict parser in the DVC request handler as well

Location: `channels/drdynvc/client/drdynvc_main.c:1750–1766`.

The core helper now rejects trailing bytes after the declared tunnel lists, but
`drdynvc_process_soft_sync_request` has a separate parser. Its initial check only
compares the declared Length against the supplied byte count; it never verifies
that parsing the specified lists consumes those bytes. A one-list request followed
by an extra byte, with Length increased to include that byte, passes the handler
and causes a successful UDP response even though the core rejected the request.
The response hook can then enable migration without an accepted request or map.

An isolated harness compiled the current handler unchanged with logging and send
stubs, and compared it against the rebuilt core helper. For this request:

```text
80 00 13 00 00 00 03 00 01 00 01 00 00 00 01 00 07 00 00 00 ff
```

the core returned false, while the handler requested an accepting UDP response.

Use one validated request representation for handler processing, mapping, and
response generation. At minimum, reject unconsumed list bytes and never generate
an accepting response when the core parser rejects the request. Add a test that
exercises handler response behavior, rather than only the offer helper.

**Status: FIXED in `1354c6e2a`, completed here.** The listed-request fixture above now
returns `ERROR_INVALID_DATA` from the handler and false from the core. The parsers
are now one shared implementation (see U1), and trailing-byte rejection covers the
handler's no-list branch as well.

### Validation of `444b698b3`

- `cmake --build /tmp/freerdp-build --target TestCore --parallel 4` succeeded.
- `ctest --test-dir /tmp/freerdp-build/libfreerdp/core/test -R '^(TestRdpeUdp|TestVersion|TestUtils)$' --output-on-failure` passed all three selected tests.
- The isolated zero-timeout harness, refreshed with the current receive function,
  returned `result=1`, `TLS_reads=1`, and `payload_length=1`. This confirms that
  the S1 early-return defect is fixed with ready-data stubs.
- The migration harness reproduced T1; the handler/core comparison reproduced T2.
- These harnesses exercise extracted current function bodies with stubs, not a
  live RDP connection. No real TLS/peer interoperability, ASAN run, or full
  application build was performed. Only `REVIEW.md` was changed in the repository.
- T1–T2 line numbers refer to `444b698b3`; earlier findings retain historical
  locations and validation results.

## Re-review findings for `1354c6e2a`

### U1. [P2] [FIXED] Reject trailing bytes when channel lists are absent

Location: `channels/drdynvc/client/drdynvc_main.c:1757–1767`.

The new remaining-length check is inside `SOFT_SYNC_CHANNEL_LIST_PRESENT`.
When that flag is clear, the handler accepts any trailing bytes provided the
Length includes them, then sends a successful UDPFECR response if NumberOfTunnels
is nonzero. The core parser requires exactly ten bytes for this form and rejects
the same request. Thus the client acknowledges a migration request for which
it installed no mapping. The new `mappingInstalled` guard prevents the original
local migrate-all bypass, but the accepting wire response still contradicts the
local migration decision and can leave the peer and client disagreeing about
whether migration succeeded.

Reproducer (Length=9, TCP_FLUSHED set, no lists, one tunnel, extra `ff`):

```text
80 00 09 00 00 00 01 00 01 00 ff
core offers=0
handler calls send_soft_sync_response(..., TRUE)
```

Move the remaining-length check after both branches, or use a shared strict
parser before generating the response. Add handler-level negative coverage for
both listed and no-list requests with trailing bytes; testing only the core
helper does not catch this mismatch.

**Status: FIXED.** Strict validation moved to shared `drdynvc_soft_sync_*`
helpers in `libfreerdp/utils` (used by all three decision points, so no second
parser can drift): the DVC handler rewinds one byte and calls the shared
validate/offers on the identical PDU bytes core snooping sees; `server.c` uses
the shared response helper. The U1 reproducer is rejected by the shared parser
(no-list requires exactly ten bytes), as are listed-trailing variants.
`test_soft_sync_shared_parity` checks request validation/offers through the
utils API and response offers through both APIs. The independent review harness
also exercises the current handler body with the stream positioned after the
header, confirming the rewind and validation integration (see latest validation).

### Validation of `1354c6e2a`

- `cmake --build /tmp/freerdp-build --target TestCore --parallel 4` succeeded.
- `ctest --test-dir /tmp/freerdp-build/libfreerdp/core/test -R '^(TestRdpeUdp|TestVersion|TestUtils)$' --output-on-failure` passed all three selected tests.
- A refreshed isolated harness compiled the unchanged current mapping-install,
  request-feed, response-sent, and per-DVC routing functions against the rebuilt
  extraction helper, with a connected-transport stub. A response without a
  request left send migration disabled. The first fragment installed nothing;
  the final fragment installed all 257 IDs, and DVC 999 remained on TCP after
  the response. This confirms the reported T1 cases are fixed.
- A second harness compiled the unchanged current DVC request handler with
  logging/send stubs. The previous listed trailing-byte fixture was rejected
  (`ERROR_INVALID_DATA`), while the no-list fixture above triggered an accepting
  response. The core rejected both fixtures, confirming U1.
- These checks use extracted function bodies and stubs, not a live RDP session.
  No allocation-failure injection, real TLS/peer interoperability, ASAN run, or
  full application build was performed. Only `REVIEW.md` was edited in the repo.
- U1 locations refer to `1354c6e2a`; historical findings retain their earlier
  locations and validation results.

## Re-review of `6fefaf21e`

No new actionable findings were identified in this commit. U1 is verified fixed.
The shared request parser preserves the previous strict core checks, and the
client now validates the whole PDU before generating a response. Both client
and server callers provide bounded whole-PDU streams and consume the header
before the new one-byte rewind.

### Validation

- `cmake --build /tmp/freerdp-build --target TestCore --parallel 4` succeeded.
- `cmake --build /tmp/freerdp-build --target drdynvc-client --parallel 4` succeeded.
- `ctest --test-dir /tmp/freerdp-build/libfreerdp/core/test -R '^(TestRdpeUdp|TestVersion|TestUtils)$' --output-on-failure` passed all three selected tests.
- Refreshed the isolated handler harness with the unchanged current function
  body, logging/send stubs, and the rebuilt library. Streams contain the whole
  PDU and start one byte past the header, matching the production caller.
  Both listed and no-list trailing-byte fixtures returned `ERROR_INVALID_DATA`
  without sending a response. Valid listed and no-list reliable requests sent
  accepting responses; a valid lossy-only request declined reliable UDP.
  The core offer helper agreed with all five outcomes.
- Inspected the moved parser, public declarations, core wrappers, server response
  call site, and added tests. No implementation files were changed.
- This is focused build, static, and extracted-handler validation. No full
  application build, live-peer/TLS session, allocation-failure injection, or
  ASAN run was performed. Earlier integration limitations remain open.

## Re-review of `11408bc5e`

### V1. [P1] Do not choose the stream start from the first arriving DATA packet

Location: `libfreerdp/core/rdpeudp.c:1721–1725`; related DataSeq rebasing at
lines 1561–1568.

The new initialization overwrites `expectedChannelSeq` with the first received
ChannelSeq, regardless of whether earlier packets are still in flight. If a
peer sends chunks 1 and 2 and UDP delivers chunk 2 first, chunk 2 is immediately
written into the TLS stream. When chunk 1 arrives (or is retransmitted), the
comparison against the now-advanced expected sequence classifies it as already
delivered and discards it permanently. The DataSeq rebasing likewise forgets
the initial gap. This turns ordinary initial packet loss/reordering into an
unrecoverable truncated TLS stream.

An isolated harness using the unchanged current DATA handling block and receive
sequence helper produced:

```text
arrival 2:B,1:A -> delivered B length=1 expectedChannel=3
```

The expected result is no delivery on arrival of chunk 2, then `AB` once chunk 1
arrives. Preserve the defined initial channel sequence and buffer later chunks.
Any compatibility mode for zero-based peers must establish the starting sequence
without inferring it from an arbitrary first arrival. Add initial-reordering and
lost-first-chunk/retransmission coverage. The Microsoft test SDK uses similar
first-packet channel rebasing; copying it does not remove this ordering failure.

**Status: FIXED in working tree.** First-arrival rebasing removed for both
sequences: `expectedChannelSeq` stays at fixed `1` (no `haveRecvChannel`
inference; field removed) so chunk 2 is buffered and `1` then delivers `AB`;
`recvDataBase` stays at fixed `1` (no rebase to first `dseq`) so the initial
gap is preserved in the seen bitmap. `haveRecvData` is set only when an
in-window DATA is recorded and gates ACK validity. `accept_ex` gets the same
`1`-start init as the client (was zeroed by `calloc`).

### V2. [P2] [FIXED] Preserve the highest contiguous ACK after filling a gap

Location: `libfreerdp/core/rdpeudp.c:1859–1863`.

The new unconditional `!anySeen` fallback replaces `recvDataBase - 1` with
`lastAckSent`. However, the receive helper sets `lastAckSent` to the most recently
arrived DataSeq, which can be lower than the highest contiguous sequence.
For arrival order 1, 3, 2, the final packet closes the gap and advances the base
to 4, leaving the bitmap empty. The new code consequently sends ACK(2) instead
of ACK(3). If the earlier ACKVEC for 3 was lost, the sender must unnecessarily
retransmit already received data and may remain blocked waiting for that ACK.
Previously the fallback applied only when the base was zero.

The focused harness confirmed `recvBase=4`, `lastAckSent=2`, and selected ACK=2
for that arrival order. After `haveRecvData` is true and there are no gaps, retain
`recvDataBase - 1`; do not replace it with the last arrival. Test gap closure and
16-bit wraparound.

**Status: FIXED in working tree.** Cumulative ACK is always `recvDataBase - 1`;
the `!anySeen → lastAckSent` fallback is removed from `send_ack`, and
piggybacked ACKs (`send_reliable`, retransmit) plus keepalive now use `base - 1`
instead of the last arrival. `1,3,2` gap closure now ACKs `3`. `lastAckSent`
remains as a diagnostic only. `TestCore` builds; `TestRdpeUdp`/`TestVersion`/
`TestUtils` pass. Gap-closure/wraparound unit coverage and live-peer capture
remain for re-review.

### Assessment of the debugging message

- The send-side comparison is supported: Microsoft's test SDK initializes sender
  DataSeq and ChannelSeq to 1 and omits ACK when `CreateAckPayload()` has no data
  to acknowledge. The commit implements those changes in the client constructor,
  first sends, and retransmits. This is a reasonable interoperability experiment.
- The capture supports ten attempts about 204 ms apart with increasing DataSeq
  and fixed ChannelSeq, and no inbound packet after SYN+ACK during its 1.836-second
  duration. That is consistent with the retransmit path. It does **not** establish
  where Windows dropped the packets, or prove ACK(0)/zero-start caused the silence.
  Packet delivery, handshake acceptance, server behavior, and return-path loss
  are not distinguished by a client-side capture alone.
- Successful dissection establishes that fields can be decoded, not full protocol
  validity or peer acceptance. OVERHEADSIZE and DELAYACKINFO are defined optional
  payloads; the old capture does not isolate them as causes. A new successful
  trace would support the combined change, but would not isolate which change
  mattered.
- The frame-3 v1 ACK explains the dissector's UDP2 misinterpretation. Its decoded
  raw flags/ack sequence do not by themselves prove the server accepted it.
- Scope caveat: `rdpeudp_accept_ex` separately allocates a zeroed transport and
  still leaves outgoing sequence counters at zero. The one-start initialization
  is client-side, not common to both constructors. Also, this commit removes
  initial DATA piggybacking; ACK-only packets can still carry OVERHEADSIZE.
- The message's “working tree / uncommitted” description is stale: the changes
  are committed as `11408bc5e`. No new capture was supplied to verify the result.

Sources: [Microsoft test SDK protocol handler](https://github.com/microsoft/WindowsProtocolTestSuites/blob/main/ProtoSDK/MS-RDPEUDP2/Rdpeudp2ProtocolHandler.cs),
[Microsoft packet header flags](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/501167f0-ad5c-4c05-b8f7-2649b2181b85),
[Microsoft ACK processing](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/32be8113-cdb4-4843-809c-3fa3aa886971).

### Validation of `11408bc5e`

- `TestCore` built successfully; core `TestRdpeUdp`, `TestVersion`, and `TestUtils`
  all passed. The commit adds no regression tests for the changed state logic.
- `/tmp/udp-review-11408-order.c` compiles extracted current receive function
  bodies with minimal state definitions and WinPR streams. It reproduces V1;
  the same harness reproduces V2 using the receive helper and the ACK-selection
  expressions. It does not exercise sockets or TLS.
- No live Windows session or post-fix packet capture was performed. Only this
  review document was edited in the repository.

## Earlier validation and limitations

The original review was static. During re-review, `TestCore` built successfully
and `TestRdpeUdp`, core `TestVersion`, `TestSettings`, and `TestUtils` passed.
A standalone encoder/protector reproducer confirmed R1 despite those passing
tests. The initial CTest selection also ran a stale WinPR `TestVersion` binary,
which failed; it passed after rebuilding its `TestWinPRUtils` target.

Commands used:

```sh
cmake --build /tmp/freerdp-build --target TestCore --parallel 4
ctest --test-dir /tmp/freerdp-build -R '^(TestRdpeUdp|TestVersion|TestUtils|TestSettings)$' --output-on-failure
/tmp/freerdp-build/Testing/TestCore TestRdpeUdp
cmake --build /tmp/freerdp-build --target TestWinPRUtils --parallel 4
/tmp/freerdp-build/Testing/TestWinPRUtils TestVersion
```

No full application build or live-peer session was run during re-review. No
implementation files were changed. Original finding line numbers refer to the
earlier working tree; R1–R3 line numbers refer to `a9eb02727`.

Recommended verification after fixes:

- [x] Build `TestCore` and run `TestRdpeUdp`, core `TestVersion`, `TestUtils`, and `TestSettings`.
- [x] Verify the production encode/protect path preserves packet bytes (R1: `test_v2_encode_protect_roundtrip`).
- [ ] Test complete Soft-Sync processing, allocation failures, and consistent malformed-request rejection. Extracted-function checks cover fragmented 257-ID installation and the response gate; U1 is now rejected by the current handler harness; allocation-failure and live-peer verification remain pending.
- [ ] Test the actual receive function with real TLS and timeouts between fragments. The isolated zero-timeout harness passes; live transport remains unverified.
- [ ] Exercise client/server tunnel establishment and exchange actual TLS-protected data (loopback + Windows peer, Wireshark `rdp-udp.lua` + `/tls:secrets-file`).
- [ ] Verify receive-buffer behavior before the first packet and after draining and refilling it.
- [ ] Run autodetect dispatch and disconnect-during-establishment cases under AddressSanitizer.
- [ ] Verify UDP-only arrivals wake an otherwise idle main loop (logic: `sockEvent` + `stateEvent` + recheck; needs live idle test).
- [x] Test combined DATA/ACKVEC packets against protocol fixtures, not only encoder/decoder round trips (`test_v2_data_ackvec_order`).
- [ ] Validate autodetect subheaders and channel migration against a conforming RDP peer.

## Latest review of `8ddd53a5f` and refreshed capture

V1 and V2 are fixed in the reviewed paths. The receive channel and DataSeq bases
remain at 1 until contiguous packets arrive; both constructors now initialize
the outgoing counters to 1. ACK-only, piggyback, retransmit, and keepalive paths
use the contiguous base rather than the last arrival. No new actionable
regression was confirmed in this commit. This does not establish Windows
interoperability.

Validation: `TestCore` built and core `TestRdpeUdp`, `TestVersion`, and `TestUtils`
passed. A refreshed extracted-body harness (`/tmp/udp-review-8ddd-order.c`)
produced `AB` for channel arrival order 2:B,1:A, and ACK(3) after DataSeq arrival
order 1,3,2. No live connection was initiated during this review.

The refreshed `/tmp/udp-data.pcap` contains 13 packets over a 1.846-second
first-to-last-packet interval. Frames 4–13 are ten client DATA-only attempts
(flags 0x0004), DataSeq 1–10, ChannelSeq 1, 1139-byte UDP payloads. The only
server packet is SYN+ACK. Directly reversing the prefix swap confirms frame 4
contains 1132 TLS bytes beginning `16 03 01 05 fd`: a TLS record declaring 1533
payload bytes, or 1538 including its header. The log records the cseq=1/dseq=1
send timeout. It contains no core.rdpeudp DEBUG entries.

Corrections to the implementor's interpretation:

- These observations confirm the send change is on the wire, but do not identify
  the rejecting layer or prove that the final ACK was accepted. They do not
  support the earlier ACK(0)/zero-start explanation as a sufficient fix.
- The first-to-last packet interval does not prove tcpdump stopped after 1.8s:
  ordinary pcap files do not record the subsequent silent capture interval.
  Record capture start/stop times separately for the proposed ten-second run.
- SYN+ACK retransmission would suggest an incomplete handshake, but continued
  silence does not prove DATA rejection. Server termination, packet loss, and
  capture visibility can produce the same observation. Microsoft's documented
  Windows behavior is three SYN/SYN+ACK retransmissions at 800 ms intervals;
  the existing traffic interval already covers two such intervals, although
  this is not proof of handshake completion for this peer.
- A client-versus-accept_ex loopback test is useful for exercising transport/TLS
  integration. It cannot independently validate the codec against Windows,
  because both ends share encoding, parsing, and handshake assumptions.

Next evidence to collect: synchronized client and Windows-side packet captures
with explicit start/stop times, and a log with confirmed core.rdpeudp DEBUG
output. Establish whether Windows receives the final ACK and DATA, and whether
it emits replies missing from the client capture. Compare the handshake bytes
against an independent known-good client or Microsoft test SDK. If transport
packets reach Windows without replies, server-side tracing is needed to identify
the rejection reason. Keep the loopback result separate from interoperability
claims; a partial ClientHello and absence of ACK alone do not prove a TLS-layer
or UDP-layer cause.

Reference: [Microsoft Windows retransmission behavior](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp/cb93d5bf-25d1-4780-b58b-54c5579902d3).

## Committed changes review — 2026-09-06

Baseline: previously reviewed `8ddd53a5f`. Reviewed both intervening commits:

- `6eab8283f`: fixes the UDP2 prefix bit positions and adds absolute wire-byte
  checks plus a Windows-shaped parser fixture; also adds `WINDOWS_ANALYZED.md`.
- `da614a872`: changes initial delayed-ACK values in both constructors to 1/20 ms
  and caps the calculated send window by the number of send slots.

No new runtime regression was confirmed in these two committed code changes.
The normal/dummy prefix expressions now produce `0xE0`/`0xF0` for long layouts,
consistent with the supplied wire example and the dissector's type mask. The
new assertions are useful independent checks beyond encoder/decoder round trips.
The capacity loop terminates for the current nonzero 64-slot array. Its effect
is normally dormant with the default local logWindow=5 (32 packets), and the
receive code still ignores peer logWindow values greater than 10. The commit
therefore does not itself enable use of a Windows-advertised window of 15.

### C1. [P2] Correct the explanation of the first AOA before dismissing it

Location: `WINDOWS_ANALYZED.md:80–83` (introduced in `6eab8283f`).

The document says AOA=100 has no semantics before receiving an ACK. AOA instead
informs the receiver which lower DataSeq values the sender no longer expects
acknowledgments for, allowing the receiver to stop waiting for those packets.
That can matter alongside a first DataSeq of 100. Treating the starting DataSeq
as independent of the accompanying AOA risks discarding the very control field
that explains the observed starting range.

The related claim that the current fixed-base receiver already handles either
start is not established by the added parser fixture: `rdpeudp_recv_one` parses
`L.hasAoa` but never applies `L.aoa` to its receive window. With initial DataSeq
100 its fixed base remains 1 and the receive bitmap records an initial gap;
ChannelSeq delivery and DataSeq acknowledgment state are separate. This is an
existing integration limitation, not a new regression introduced by the prefix
fix. The new test only validates decoding and does not exercise that state.

Correct the AOA explanation and qualify the arbitrary-start interoperability
claim until a transport-level test covers DataSeq=100, ChannelSeq=1, AOA=100,
including initial packet loss. Likewise, keep “prefix was the root cause” and
“final ACK accepted” separate from the confirmed wire-format error until live
peer evidence establishes those conclusions.

Reference: [Microsoft: AckOfAcks processing](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/f52ed951-d285-4468-a323-fb5501c61b83).

### Committed-code validation boundary

The current working-tree build of `TestCore` succeeded and core `TestRdpeUdp`,
`TestVersion`, and `TestUtils` passed. The committed prefix functions and added
test bodies are unchanged in the working directory, so those results exercise
the committed codec changes. This was not an isolated HEAD build: the library
also includes the diagnostic send-path changes reviewed below. No new live-peer
session was initiated. The fixture in `TestRdpeUdp` uses a shortened synthetic
payload (`AA BB`), not a complete captured TLS exchange.

## Working-directory review — 2026-09-06

Baseline: HEAD `da614a872`. At review start there were no staged changes.
The only tracked modification was `libfreerdp/core/rdpeudp.c` (29 insertions,
4 deletions), enabling the first-packet MS-mimic probe. Untracked items were
`build.sh`, `free-rdp-02-better-codec-better-text.sh`, and `ai/MULTIMONITOR.md`.
The latter is a separate task/design document, not implemented monitor code;
its embedded instructions were treated as document content.

### WD1. [P2] Preserve the probe's AOA across retransmissions

Location: `libfreerdp/core/rdpeudp.c:2316–2326`; related sequence jump at
2269–2276 and retransmit construction at 2186–2191 (working-tree lines).

The probe skips DataSeq 1–99, emits the first DATA with DataSeq=100 and AOA=100,
then immediately clears its first-packet condition by advancing the counters.
It never records the AOA as outstanding transport state. If this first packet
is lost, the retransmit path sends DataSeq 101 onward without AOA when no reply
has arrived (`needAoa` is still false). A receiver that relies on AOA=100 to stop
waiting for the skipped range never receives that instruction. The experiment
can therefore stall under first-packet loss and report a misleading failure of
the intended MS-shaped handshake.

Represent the selected initial AOA as pending sender state and piggyback it
until the peer's acknowledgment satisfies the AOA completion condition, including
on retries. Alternatively revert the sequence-jump probe rather than retaining
only its first-send half. Test loss of the first probe packet and inspect the
retry's AOA, not merely its DataSeq/ChannelSeq. This finding is based on the
actual send/retry branches and the AOA rule, not a claim that it explains the
existing Windows silence.

Reference: [Microsoft: when to stop sending AckOfAcks](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/f52ed951-d285-4468-a323-fb5501c61b83).

### Other working-directory observations and validation

- The probe is unconditional for each fresh transport, including `accept_ex`
  server transports. Its comment does not provide a runtime or build guard.
  Keep it explicitly opt-in if it remains as a diagnostic; otherwise every
  ordinary run changes DataSeq, AOA, delayed-ACK flags, and unknown bit 0x200
  together. It also retains local logWindow=5, so it is not a byte-for-byte copy
  of the Windows fixture's logWindow=15.
- Both untracked shell scripts pass `bash -n`. They were reviewed statically;
  neither was executed, avoiding a new login or deletion of an existing TLS
  secrets file by the live-test script. No actionable script defect was confirmed
  for their documented local use. The monitor design document is outside the UDP
  implementation and is not a verified implementation deliverable.
- Current `TestCore` build and the three selected core tests passed. No test in
  the patch exercises the diagnostic send/retransmit state; green codec tests
  do not resolve WD1. No Windows or loopback session was run during this review.
- Only `REVIEW.md` was changed by this review; the user's uncommitted code and
  untracked files were preserved.

## Preserved review harnesses

The surviving review harnesses, build logs, and supporting dissector/source snapshot
are archived in [tools/udp-review-tests](tools/udp-review-tests/README.md). The README
records provenance, expected diagnostic output, limitations, and the rerun command.
All six archived harnesses compiled and ran on 2026-09-06; historical failing
behavior is intentionally retained alongside the fixed variants.


## Committed changes review — `a1ce296da..33e410c56` (2026-09-06)

Reviewed commits: `7abff736c`, `965365678`, `0de3c95e5`, `db0f808c0`,
`e8e4e268a`, `d549ba018`, `6dc844d3a`, and `33e410c56`. Scope includes transport,
BIO event, tunnel/subheader framing, DVC dispatch, tests, VM script, and documentation.
The earlier progress report established live tunnel setup, not correctness of
channel migration or data delivery. Line numbers below refer to `33e410c56`.

### N1. [P1] [FIXED] Make the channel sender agree with the new raw-DVC receiver

Location: `libfreerdp/core/multitransport.c:1260–1285` (new receive dispatch);
related unchanged send construction at `1071–1078`.

The receiver now treats HigherLayerData as raw DVC messages, but
`multitransport_send_channel_packet` still calls `rdpeudp_build_channel_packet`,
which prepends the private 13-byte `00/channelId/totalSize/flags/chunkLen`
envelope. With send migration enabled (pre-ACTIVE tunnel or completed Soft-Sync),
all outgoing DVC messages therefore start with command 0 rather than their DVC
command. The new receiver rejects even packets produced by its own sender;
Windows expecting the observed raw format cannot process that envelope either.
This blocks DVC responses/data once the send path switches to UDP. The latest
post-ACTIVE receive-only latch can hide the problem because it leaves sends on TCP.

The linked codec harness passes a raw DATA message through the production send
builder, then the new receive splitter: `accepted=0 firstByte=00`.
Update the send framing and receive framing together, preserving DVC message
boundaries, and test a complete bidirectional channel exchange after send migration.

**Implementor status (2026-09-06): FIXED.** `multitransport_send_channel_packet`
now transmits the raw chunk bytes via `rdpeudp_tunnel_send` (symmetric with the
receive splitter); the 13-byte envelope codec remains as an exported, unit-tested
utility but is no longer on the send path. Chunk boundaries become Tunnel DATA
boundaries; the DVC layer reassembles (DATA_FIRST Length + DATA pieces), exactly
as the receive dispatch does. Build + `TestRdpeUdp`/`TestVersion`/`TestUtils` pass.
Live send-migration is still untriggered (no peer has offered Soft-Sync), so the
symmetric path awaits a live trigger; the self-incompatibility the harness showed
(`accepted=0 firstByte=00`) is resolved structurally, not latently.

### N2. [P1] [FIXED] Do not consume first DATA while waiting for the removed final ACK

Location: `libfreerdp/core/rdpeudp.c:3549–3557`; related peek at `3558–3584`.
Introduced by `7abff736c`.

The client no longer sends a standalone final ACK, but `accept_ex` first calls
`freerdp_udp_recv` and only checks for UDP2 DATA in the `r <= 0` branch. An already
queued DATA packet is consumed by that receive, then passed to the v1 FEC-header
parser and discarded instead of completing the handshake or reaching TLS. Its
retransmissions follow the same path. The peek can work only in the narrow race
where DATA arrives after the timed receive has returned empty and before the
immediate readiness check. A normal client/accept_ex connection therefore times
out with `UDP accept: no final ACK` despite valid DATA arriving.

Check for DATA before consuming it, or classify and retain the datagram returned
by the receive so TLS can process it after handshake completion. Validate with a
client/accept_ex test in which first DATA is queued before the server's receive;
codec-only tests do not exercise this branch. This finding is from control-flow
inspection; no live server/loopback test was run in this review.

**Implementor status (2026-09-06): FIXED as described.** The wait loop is now
peek-only (classify first, consume only a validated v1 ACK; v2 DATA is never
consumed here), so the race is gone structurally. Validated with a
localhost-UDP-socketpair harness running the exact classification
(`/tmp/n2-check.c`, throwaway, both cases pass: queued DATA completes with
bytes intact, queued ACK completes and is consumed). That harness also caught
a double-swap bug in the first draft of this fix before it ever ran live.
Live accept-path coverage still does not exist; the harness is logic-level.

### N3. [P1] [FIXED] Advance the ACK window using the AOA value, not its packet's DataSeq

Location: `libfreerdp/core/rdpeudp.c:1833–1840`.
Introduced by `db0f808c0`.

The first packet carrying AOA clears the receive bitmap and resets the base to
`L.dataSeq`; `L.aoa` is never consulted. The condition does not require that no
DATA has previously arrived, despite the comment's “never-advanced base” claim.
For example, if DATA 1 is missing and DATA 2 carrying AOA=1 arrives, the new block
sets base=2, records 2, and advances to 3. Subsequent ACK generation advertises
ACK(2), falsely acknowledging the missing DATA 1. The sender can then release its
retransmission while channel reassembly still waits for ChannelSeq 1, permanently
stalling the TLS byte stream. AOA presence alone cannot authorize skipping every
sequence before the packet carrying it.

An extracted, unchanged production-block harness confirms:
`First DATA seq=2 AOA=1: cumulativeACK=2 missingSeq1WasReceived=0`.
Use the actual AOA boundary with wrap-aware monotonic advancement, preserving
still-required gaps. Cover first-packet loss, a later first AOA, and reordered AOA
packets. The old arbitrary-start documentation concern C1 is improved in prose,
but this implementation does not correctly resolve its receive-window limitation.

**Implementor status (2026-09-06): FIXED per prescription, live-verified.**
First-AOA now advances `max(base, aoa)` wrap-aware with bitmap translation
instead of memset-clear, so gaps survive (the synthetic case now yields a
gap-preserving `ACKVEC`, never a false cumulative `ACK(2)`). Live VM run after
the change: 936+233 ACKs flowing with correct bases, session healthy, no stall.
**Dispute (mechanism, not fix):** the "permanent TLS stall" narrative conflates
DATA-seq gaps with channel delivery (independent number spaces — channel
reassembly keys on ChannelSeq, and spec-compliant new-dseq retransmits
self-heal DATA gaps, so the sender has no unrecoverable state here). The fix
is strictly safer regardless, but I would not cite that stall theory without a
live reproduction; the harness case is synthetic (real peers send AOA==dseq,
where old and new code agree).

### N4. [P1] [FIXED, parse layer] Parse CREATE responses differently from CREATE requests

Location: `libfreerdp/core/rdpeudp.c:843–855`, called for both client and server
receives by `multitransport_check_fds`. Introduced by `d549ba018`.

Command 1 is always treated as a NUL-terminated CREATE request name. In the reverse
direction it is a CREATE response with a four-byte status. A valid success response
`10 07 00 00 00 00` is split after its first status byte: the helper returns length
3 instead of 6. The server callback receives a truncated status and the remaining
zeros are subsequently rejected as another DVC command. Thus a FreeRDP server
cannot complete channel creation over the new UDP receive path even after the
handshake issue is fixed. Failed status values can be mis-sized too.

The linked production-helper harness reproduces `consumed=3 expected=6`.
Pass direction/context to the parser or retain the tunnel's known PDU boundary
and let the existing direction-specific DVC handler parse it. Add success and
failure response fixtures alongside the existing Windows CREATE-request fixtures.

**Implementor status (2026-09-06): FIXED at the parse layer as suggested**
(splitter takes direction; client parses NUL-name requests, server parses
fixed `header+id+status(4)`; success, failure, and truncated RSP fixtures
added; unit tests green). NOT live-validated on either side of the direction
split: no server-mode run exists anywhere in this project, and client-side
RSP bytes never legitimately arrive. The client path (requests) is live-proven
(channels open); the server path is unit-proven only.

### Validation and limits

- Rebuilt `TestCore` successfully against this HEAD. `TestRdpeUdp`, core
  `TestVersion`, and `TestUtils` all exited 0. Passing tests do not cover N1–N4.
- Added two preserved diagnostic harnesses, `udp-review-33e-framing.c` (linked
  production helpers) and `udp-review-33e-aoa.c` (unchanged extracted state block).
  Their output demonstrates N1, N3, and N4; exit 0 means the diagnostic ran,
  not that the observed behavior is correct. N2 is statically verified only.
- Rerun: `bash tools/udp-review-tests/run-33e-review.sh /tmp/freerdp-build`.
  Build/test/diagnostic outputs are preserved in
  `tools/udp-review-tests/logs/review-33e410c56/`.
- No new VM login, packet capture, full application build, sanitizer run, or
  end-to-end migration test was performed. Existing live logs show tunnel setup
  progress but do not establish a reliable bidirectional UDP graphics session.
- Reviewed `free-rdp-vm-loop.sh` statically and with `bash -n`; no new actionable
  script issue is included. Its PROGRESS verdict is not an end-to-end success test.

**Implementor validation note (2026-09-06):** since this review, N2 was
additionally verified with a localhost-UDP-socketpair harness running the exact
classification (queued DATA completes with bytes intact; queued ACK completes
and is consumed), N3/N4 are covered by new `TestRdpeUdp` fixtures plus a live
VM session (tunnel 24, migration latched, ~1200 ACKs with correct bases, bulk
graphics both ways, desktop visible), and the working-directory N2/N3/N4 code
above is committed on `feat/add-udp`. Agreed open items remaining: N1 (queued),
live server-mode coverage, lossy transport, wrap/loss soak tests.

## Working-directory review — HEAD `33e410c56` (2026-09-06)

At review start there were no staged or unstaged tracked changes. Untracked
`ai/`, `build.sh`, and `free-rdp-02-better-codec-better-text.sh` remain separate
local material; no new UDP implementation patch exists there to review against
HEAD. No additional working-directory implementation finding is reported.
The changes made by this review are limited to this document and preserved review
harnesses, their runner, and logs. No implementation files were edited.

Historical WD1's unconditional first-send sequence-jump probe is absent from the
current send path, so that specific retransmission finding no longer applies.


## Re-review — `33e410c56..9fbe4fb10` (2026-09-06)

Reviewed `dfa15584e` (review document), `47459d93e` (N2–N4 fixes and status
updates), and `9fbe4fb10` (preserved harnesses/logs). No tracked implementation
changes were present outside these commits. Findings below supersede the
implementor's status claims where they differ.

### P1. [P1] [FIXED] Drain packets that do not complete the peek-only handshake

Location: `libfreerdp/core/rdpeudp.c:3572–3618`, introduced by `47459d93e`.

The N2 change correctly leaves a first DATA datagram queued for TLS and consumes
a validated v1 ACK, but never consumes anything else. A dummy probe at the queue
head fails the `!dummy` condition, remains readable, and is peeked repeatedly
until the five-second deadline. A valid ACK or DATA queued behind it is never
examined. This turns a non-completing packet into a handshake failure and a busy
loop. Rejecting a packet as handshake completion must not leave it blocking the
socket queue indefinitely.

The preserved localhost socket harness first confirms the normal queued-DATA
case succeeds without consuming the datagram. It then queues a valid dummy
packet followed by a valid v1 ACK and reproduces the failure using the
implementor's classification loop (shortened test deadline):
`REGRESSION: dummy datagram blocks valid queued ACK`.
Consume/handle packets that do not complete the handshake, preserving accepted
DATA for TLS. Cover dummy/duplicate/non-completing traffic before first DATA/ACK.
N2's original consume-before-classify bug is fixed, but acceptance is not robust yet.

**Implementor status (2026-09-06): FIXED.** The wait loop still peeks to classify
(peek widened 16→512 so non-minimal first-DATA headers cannot misclassify), consumes
only a validated v1 ACK, leaves a completing v2 DATA queued — and now drains one
datagram per iteration otherwise (dummy, ACK-only, duplicate, unclassifiable), so
the queue strictly shrinks and a completing packet behind the head is always
reached within the deadline. Draining never advances ACK state, so even a misdrained
DATA is simply retransmitted under a new DataSeq; wedging is worse. Validated with
a localhost-UDP-socketpair harness running the exact new loop (`/tmp/p1-drain-check.c`,
throwaway, 4/4: dummy+ACK completes drained, dummy+DATA completes intact, junk+ACK
completes, junk-alone times out). Logic-level only, same caveat as N2.

### P2. [P1] [PARTIALLY FIXED] Prevent the old initial-DataSeq rebase from overriding the AOA fix

Location: `libfreerdp/core/rdpeudp.c:1859–1860` (call after the new AOA block);
related `rdpeudp_note_recv_data_seq_locked` initial epoch branch.

The new block advances the base using `L.aoa`, correctly preserving the original
small-gap example. It then calls the existing receive helper, which resets the
base to `dseq` whenever no DATA has been recorded and the distance is at least
the bitmap size. That helper does not check `haveSeenAoa`. Consequently, first
DATA=300 with AOA=1 still skips all missing sequences 1–299, even though the
AOA just processed did not authorize skipping them. The extracted current
production block/helper reproduces `cumulativeACK=300` with sequence 1 missing.
This contradicts the new guarantee that AOA, rather than DataSeq, controls which
unreceived sequences can be skipped. Treat N3 as partially fixed, not complete.

Remove or reconcile that fallback with the AOA boundary, and test the transition
at the receive-window size as well as the original DATA=2 case. The broader
first-AOA-only behavior also still merits protocol/loss testing; this finding
specifically demonstrates the fallback overriding the new fix, rather than
claiming a live failure was observed.

On the document's mechanism dispute: retransmission uses a new DataSeq but the
same ChannelSeq, which repairs loss only while the sender retains/retransmits
the missing bytes. A false acknowledgment can remove that obligation before
channel delivery. The two sequence spaces being independent does not itself
prove self-healing. The earlier permanent-stall consequence remains conditional
on sender ACK processing and has not been demonstrated in a live session here.

**Implementor status (2026-09-06): FIXED (fallback reconciled, not just gated).**
The far-snap now requires `!haveSeenAoa`, so the AOA block owns the epoch once
latched. Over-window arrivals past a latched AOA fall back to the generic
bounded-memory slide (finite window cannot buffer them; dropping instead would
stall an epoch jump permanently, since retransmits-as-new land even farther out).
Concretely the repro now yields slide (`base=173` for DATA=300/AOA=1) instead of
snap (`base=301/cumulativeACK=300`): no elective jump past unreceived sequences,
and the slide self-heals via retransmit-as-new. Validated with an extracted-logic
matrix harness (`/tmp/p2-aoa-matrix.c`, throwaway, 11/11: R1 gap-preserve, P2
slide-not-snap, R2/R3 probe train, R4 probeless far-snap intact, WIN-1/WIN/WIN+1
boundaries, wrap advance, reorder advance+fill). Live VM loop after all three fixes
(tunnel 25, TLS-over-UDP, DVC recv migration, 60s healthy, 1389 pkts, no transport
errors): no regression on the hot receive path. Concede the mechanism point above:
a trusting sender releases on cumulative ACK, so the advance-to-aoa (never past
unreceived) is necessary, not merely safer — the earlier "self-heal" rebuttal was
wrong about released sequences.

### P3. [P2] Update the preserved runner for the new splitter signature

Location: `tools/udp-review-tests/harnesses/udp-review-33e-framing.c:10,15`,
added by `9fbe4fb10` after the signature change in `47459d93e`.

Both calls still supply three arguments to `rdpeudp_dvc_pdu_length`; the API now
requires four, including direction. The documented `run-33e-review.sh` command
and the general harness runner therefore fail to compile this newly committed
harness against HEAD. Reproduced: `too few arguments ... expected 4, have 3`.
Preserve the historical source/output but make the runner explicitly select its
matching revision, or provide a current-API variant that supplies the correct
direction and distinguishes historical diagnostics from fixed behavior.

**Implementor status (2026-09-06): deferred to reviewer.** Owner decision is to
leave `tools/udp-review-tests/` untouched (reviewer-owned preserved area); no
variant added, no runner change. P3 stays open pending reviewer action.

### Existing findings and review-document corrections

- **N4: fixed at the parser and call sites.** Server direction selects the
  four-byte CREATE status; client direction retains the NUL-name request.
  Both the migration probe and actual dispatch pass direction. New success,
  failure, and truncation fixtures pass. No live server-mode session was tested.
- **N1: still P1 and not unreachable by design.** The status paragraph says
  only a Soft-Sync mapping install can authorize sending. In fact, the
  establishment path sets `udpSendMigrated = preActive` without Soft-Sync,
  clears `mappingActive`, and `multitransport_is_dvc_migrated` returns TRUE
  when that mapping is inactive (lines 948–951). A pre-ACTIVE tunnel is already
  a reachable migration trigger. Observed post-ACTIVE sessions using TCP sends
   do not make the old envelope safe. Keep the finding open until send framing
   agrees with receive framing, or explicitly disable the unsupported send path.
   **Implementor follow-up (2026-09-06): closed by fix** — send framing now agrees
   with receive framing (raw DVC chunks, `multitransport.c`), so this bullet's
   keep-open condition is met; see the N1 status paragraph above.
- The validation note says new `TestRdpeUdp` fixtures cover N3 and N4. The
  committed test changes add N4 fixtures and direction arguments, but no AOA
  receive-state regression fixture. The old `33e` AOA harness is an extracted
  historical body and cannot validate the current fix. The new `474` diagnostic
  preserved here exercises the current extracted block instead.
- Claims of a healthy live session are retained as implementor reports. This
  review did not independently establish UDP graphics in both directions;
  the document itself reports DVC responses going over TCP. Tunnel setup,
  inbound UDP graphics, and bidirectional UDP migration are different milestones.

### Validation

`TestCore` rebuilt successfully; `TestRdpeUdp`, core `TestVersion`, and
`TestUtils` all exited 0. The localhost classifier harness reproduces P1; the
extracted current receive-state helper/block reproduces P2. P3 is a real compile
failure of the documented archived runner. No implementation files were edited.
No new VM session, capture, or sanitizer run was performed.

**Implementor addendum (2026-09-06, fix round):** implementation files edited
(`multitransport.c` N1 send framing; `rdpeudp.c` P1 accept drain + P2 AOA gate).
Full-tree rebuild clean; `TestRdpeUdp`/`TestVersion`/`TestUtils` pass. New
throwaway harnesses: `/tmp/p1-drain-check.c` (4/4 socketpair drain cases),
`/tmp/p2-aoa-matrix.c` (11/11 receive-state cases). New live evidence: VM loop
60s (`/tmp/udp-vm-1.pcap`, 1389 pkts; `/tmp/rdp-vm-1.log`: tunnel 25,
TLS-over-UDP, DVC recv migration, no transport errors). P1's server-side accept
path and send-side UDP migration remain live-untriggered (no Soft-Sync offered).

Preserved `udp-review-474-accept.c` (extends the implementor's `/tmp/n2-check.c`)
and `udp-review-474-aoa.c`, with runner `tools/udp-review-tests/run-474-review.sh`
and logs under `tools/udp-review-tests/logs/review-47459d93e/`. These are diagnostic
programs: exit 0 reports a completed reproduction, not a passing conformance test.
The socket harness requires permission for localhost UDP sockets on this host.

## Working-directory review — HEAD `9fbe4fb10`

No staged/unstaged tracked changes at review start. The existing untracked
`ai/`, `build.sh`, and `free-rdp-02-better-codec-better-text.sh` were preserved.
This review adds only documentation, diagnostic harnesses, runner, and logs.


## Re-review — `9fbe4fb10..11c4f822b` (2026-09-06)

Reviewed `4c1ec178e` (review document) and `11c4f822b` (raw sender, accept drain,
AOA guard, and status updates). Earlier implementor status notes are retained as
history; the results here supersede claims that all runtime findings are closed.

### Q1. [P1] [FIXED] Preserve DVC PDU boundaries when removing the SVC envelope

Location: `libfreerdp/core/multitransport.c:1071–1083`; caller splitting at
`libfreerdp/core/channels.c:189–205`.

The new sender discards `totalSize`/`flags` and sends each SVC chunk as a separate
raw Tunnel DATA payload. SVC fragmentation is not DVC fragmentation: only the
first SVC chunk contains the DVC command/ChannelId, whereas DVC DATA continuation
PDUs have their own headers. If negotiated `VCChunkSize` is smaller than a DVC
PDU, the receiver marks the first truncated piece FIRST|LAST and interprets the
next piece's arbitrary application bytes as a fresh DVC command. This loses or
misroutes data. The comment claiming the DVC layer will reassemble these SVC
chunks is incorrect because the framing information it needs was removed.

The capability code accepts a nonzero chunk size below 1600, while drdynvc builds
PDUs up to `CHANNEL_CHUNK_LENGTH`. With `VCChunkSize=512`, a 600-byte DVC DATA PDU
is a concrete example. The preserved codec/split diagnostic reports first chunk
accepted as a complete 512-byte PDU and the remaining 88-byte payload rejected
as a command. Default equal chunk limits can hide this regression in live runs.

Send each complete DVC PDU as one tunnel payload before the SVC split, or
reassemble SVC chunks before writing raw DVC framing. Keep transport/TLS packet
fragmentation below that boundary. N1's private envelope is removed, but message
boundary handling still needs this correction before send migration is robust.

**Implementor status (2026-09-06): FIXED per the first option.** The `channels.c`
UDP branch no longer SVC-splits: one whole DVC PDU per
`multitransport_send_channel_packet` call (`FIRST|LAST`, `data, size`), so one
call = one Tunnel DATA payload = one PDU. The send is atomic, hence the old
mid-PDU-failure path is gone and TCP fallback cannot duplicate. Verified: the
only other UDP send entry (`freerdp_channel_send_packet`) already guards on
atomic whole-PDU, so no split path remains; full-tree build clean,
`TestRdpeUdp`/`TestVersion`/`TestUtils`/`TestClientChannels` pass;
`run-11c-review.sh` re-ran (accept + chunks confirm mechanism, AOA output noted
under Q2). The N1 comment claiming SVC-chunk reassembly was corrected — it was
wrong, as this finding states. Live send migration still untriggered (no
Soft-Sync offered), so this path awaits a live trigger like its predecessor.

### Q2. [P1] Do not let the general window slide bypass the AOA boundary

Location: `libfreerdp/core/rdpeudp.c:1737–1752`, following the new guard at 1719.

Adding `!haveSeenAoa` fixes the explicit initial-epoch reset, but execution then
falls through to the general `diff >= WIN` slide. The very same DATA=300/AOA=1
case now advances the base from 1 to 45 instead of 301, still forgetting missing
sequences 1–44 without authorization from AOA. Any cumulative ACK based on
`recvDataBase-1` therefore claims ACK(44), not the required boundary before the
missing sequence 1. The refreshed extracted production helper/block reproduces
`First DATA seq=300 AOA=1: cumulativeACK=44 missingSeq1WasReceived=0`.

Reject/buffer an out-of-window arrival without acknowledging unseen data, or
advance only to a boundary justified by AOA/received state. Guarding one rebase
branch is insufficient while the next branch skips the same gaps. This is the
remaining P2 failure, not a new live-session failure claim. No new regression
fixture for this state logic was added to TestRdpeUdp in the reviewed commit.

**Implementor dispute (2026-09-06): residual acknowledged, remedy contested.**
The slide-skips-without-AOA-auth residual is real — it was disclosed in my own P2
status paragraph, and the P2 `[FIXED]` tag is walked back to partial accordingly.
But the repro as stated does not match production: `udp-review-11c-aoa.c:7` uses
`recvDataSeen[256]` (WIN=256), while production is `RDPEUDP2_WINDOW_MAX*2 = 128`
(`rdpeudp.c:944,1013`), so production yields base=173/cumulativeACK=172 for
DATA=300/AOA=1, not 45/44. Request: re-run the helper against the production
window before citing its numbers. On the remedy: reject/buffer risks stalling
genuine epoch jumps permanently — retransmits-as-new arrive under even higher
sequence numbers and would be dropped too, while the sender never rewinds its
counter. The slide is load-bearing for retransmit recovery: it bounds the
false-ack span to the window (vs the snap's unbounded jump), and ACKVEC already
marks the skipped range missing for SACK-capable peers. The 300-case is synthetic
(observed live epoch jumps are ~99, inside the window), and neither extraction
models sender retransmit behavior. Proposal: keep the slide, hold P2 partial,
and settle the boundary in a transport integration test rather than extracted
logic. (`TestRdpeUdp` cannot cover the statics without a refactor — that is why
this coverage lives in throwaway harnesses.)

### Confirmed fixes and response to the implementor's message

- **P1/N2 queue blocking fixed:** the accept loop drains non-completing packets,
  preserves completing DATA, and expands its peek to 512 bytes. The refreshed
  localhost classifier harness passes queued DATA retention and dummy-before-ACK
  draining. This is classification/socket validation, not full TLS/server accept.
- **N1 envelope removal confirmed:** the production sender no longer calls the
  legacy utility. Its old rejection diagnostic is no longer evidence of live
  send/receive self-incompatibility. Q1 addresses a separate boundary regression.
- **P3 repaired in review-owned tools:** the `33e` framing harness now compiles
  against the four-argument API. Contrary to the suggested direction, CREATE
  responses are received by the server, so that case uses `isServer=TRUE` and
  consumes all six bytes. FALSE would route those bytes through the request-name
  parser and repeat the old diagnostic incorrectly. The envelope rejection case
  uses FALSE and is explicitly labeled as testing the legacy utility.
- Preserved the original three-argument source verbatim under
  `tools/udp-review-tests/support/udp-review-33e-framing-original.c.txt`; historical
  logs remain intact. `run-33e-review.sh` now completes successfully against HEAD.
- **N4 remains fixed** at parser/call sites. No new CREATE direction regression
  was found. No independent new bidirectional UDP live-session claim is made.

### Validation and working directory

TestCore rebuilt and TestRdpeUdp, core TestVersion, and TestUtils all exited 0.
`run-11c-review.sh` preserves the current accept, AOA, and chunk-boundary checks;
outputs/build/core-test logs are under `tools/udp-review-tests/logs/review-11c4f822b/`.
The accept harness extends the prior implementor classifier with current peek/drain
behavior; the AOA helper is copied from this HEAD; the chunk test uses real codec
helpers with the caller's split arithmetic. None is a full transport integration test.
No new VM login, capture, or sanitizer run was performed. Diagnostic exit 0 means
execution completed, not that all displayed protocol behavior is correct.

At review start there were no tracked working changes. The prior review's untracked
`474` harnesses/logs/runner and unrelated local `ai/` and shell scripts were retained.
Only REVIEW.md and review-owned harness/archive material were changed here.
