# UDP implementation review

Date: 2026-09-05
Re-reviewed: 2026-09-05 (commit `a9eb02727`)
Latest review: 2026-09-05 (commit `6fefaf21e`)
Review result: U1 verified fixed; no new actionable findings in this commit.

Latest scope: commit `6fefaf21e` against its parent, with relevant UDP and DVC integration paths. Earlier sections retain the original review history.

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

## Latest review of `6fefaf21e`

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
