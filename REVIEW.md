# UDP implementation review

Date: 2026-09-05
Re-verified: 2026-09-06 (commit `8558f4094`)

Scope: currently modified files and newly added UDP implementation files.

Eight P1 correctness issues were identified. Two independently block tunnel establishment.
All eight are now FIXED in the working tree. The implementation still needs live-peer
validation before merge (see bottom), but no known P1 remains open.

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

**Status: FIXED.** Pure `rdpeudp2_parse_layout`/`rdpeudp2_encode_layout` implement spec
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

**Status: FIXED.** Migration state (`softSyncNegotiated/Complete`, `udpSend/RecvMigrated`)
with `is_udp_send/recv_migrated` gates (not mere connectivity). Without Soft-Sync,
migration latches only if the tunnel is ready pre-`ACTIVE` (post-`ACTIVE` completion
stays TCP-safe to avoid reordering in-flight TCP vs new UDP). With Soft-Sync,
send/recv enable via `on_soft_sync_request_sent/received`, `on_response_sent/received`.
Soft-Sync control PDUs (Cmd `0x08`/`0x09`) are forced TCP and snooped in `channels.c`
send/recv to drive the hooks; UDP recv pre-migration is ignored. Whole-PDU pinning
(no TCP/UDP split) and atomic-only `send_packet` UDP preserved.

## Validation and limitations

This was a static review. No build or live RDP session was run. The added tests cover codec round trips, but not establishment, stream delivery, socket wakeups, or shutdown. Source line numbers refer to the working tree at review time.

Recommended verification after fixes:

- [x] Codec/unit: `TestRdpeUdp` passes (fec/syn/protect/tunnel/ackvec/channel/mtu/subheader/layout-fixture); full build passes; `TestVersion/Utils/Settings` pass.
- [ ] Exercise client/server tunnel establishment and exchange actual TLS-protected data (loopback + Windows peer, Wireshark `rdp-udp.lua` + `/tls:secrets-file`).
- [ ] Verify receive-buffer behavior before the first packet and after draining and refilling it (covered by unit logic + `SealLength`; needs live re-check).
- [ ] Run autodetect dispatch and disconnect-during-establishment cases under AddressSanitizer.
- [ ] Verify UDP-only arrivals wake an otherwise idle main loop (logic: `sockEvent` + `stateEvent` + recheck; needs live idle test).
- [x] Test combined DATA/ACKVEC packets against protocol fixtures, not only encoder/decoder round trips (`test_v2_data_ackvec_order`).
- [ ] Validate autodetect subheaders and channel migration against a conforming RDP peer.
