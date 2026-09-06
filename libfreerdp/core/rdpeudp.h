/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP-UDP transport ([MS-RDPEUDP], [MS-RDPEUDP2], [MS-RDPEMT])
 *
 * Copyright 2026 FreeRDP Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FREERDP_LIB_CORE_RDPEUDP_H
#define FREERDP_LIB_CORE_RDPEUDP_H

#include <winpr/windows.h>
#include <winpr/stream.h>
#include <winpr/synch.h>

#include <freerdp/api.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>

#include <openssl/bio.h>

#include <stdio.h>
#include <stdlib.h>

/* Temporary live-debug tracing (remove once wlog-filter delivery is fixed:
 * correct `com.freerdp.*:DEBUG` filter strings are silently ignored by
 * sdl-freerdp even though an isolated wlog test honors them, so WLog_DBG
 * lines are invisible in live runs). Toggle at RUNTIME, no rebuild and no
 * add/remove churn around the call sites:
 *     RDPEUDP_TRACE=1 ./sdl-freerdp ...
 * Completely silent unless the variable is set to a non-"0" value. Call
 * sites stay in the tree; each expansion caches the lookup once. */
#define UDP_TRACE(...) \
	do \
	{ \
		static int udp_trace_on = -1; \
		if (udp_trace_on < 0) \
		{ \
			const char* udp_trace_env = getenv("RDPEUDP_TRACE"); \
			udp_trace_on = (udp_trace_env && (udp_trace_env[0] != '\0') && \
			                (udp_trace_env[0] != '0')) \
			                   ? 1 \
			                   : 0; \
		} \
		if (udp_trace_on) \
		{ \
			fprintf(stderr, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)

/* [MS-RDPEUDP] 2.2.2.1 RDPUDP_FEC_HEADER flags (big-endian on the wire) */
#define RDPUDP_FLAG_SYN 0x0001
#define RDPUDP_FLAG_FIN 0x0002
#define RDPUDP_FLAG_ACK 0x0004
#define RDPUDP_FLAG_DATA 0x0008
#define RDPUDP_FLAG_FEC 0x0010
#define RDPUDP_FLAG_CN 0x0020
#define RDPUDP_FLAG_CWR 0x0040
#define RDPUDP_FLAG_SACK_OPTION 0x0080
#define RDPUDP_FLAG_ACK_OF_ACKS 0x0100
#define RDPUDP_FLAG_SYNLOSSY 0x0200
#define RDPUDP_FLAG_ACKDELAYED 0x0400
#define RDPUDP_FLAG_CORRELATIONID 0x0800
#define RDPUDP_FLAG_SYNEX 0x1000

/* [MS-RDPEUDP] 2.2.2.9 protocol versions */
#define RDPUDP_PROTOCOL_VERSION_1 0x0001
#define RDPUDP_PROTOCOL_VERSION_2 0x0002
#define RDPUDP_PROTOCOL_VERSION_3 0x0101 /* means: switch to MS-RDPEUDP2 */
#define RDPUDP_SYNEX_VERSION_VALID 0x0001

/* [MS-RDPEUDP2] 2.2.1.1 header flags (little-endian on the wire) */
#define RDPUDP2_FLAG_ACK 0x001
#define RDPUDP2_FLAG_DATA 0x004
#define RDPUDP2_FLAG_ACKVEC 0x008
#define RDPUDP2_FLAG_AOA 0x010
#define RDPUDP2_FLAG_OVERHEAD 0x040
#define RDPUDP2_FLAG_DELAYACK 0x100
#define RDPUDP2_FLAGS_MASK 0xFFF

#define RDPUDP2_DEFAULT_LOGWINDOW 5 /* 32 * MTU window */
#define RDPUDP2_MAX_LOGWINDOW 10
#define RDPUDP2_MTU 1232
#define RDPUDP2_MAX_PAYLOAD 1150 /* MTU minus worst-case headers */

/* [MS-RDPEMT] 2.2.1.1 tunnel actions */
#define RDPTUNNEL_ACTION_CREATEREQUEST 0x00
#define RDPTUNNEL_ACTION_CREATERESPONSE 0x01
#define RDPTUNNEL_ACTION_DATA 0x02

#define RDPEUDP_COOKIE_LEN 16
#define RDPEUDP_COOKIE_HASHLEN 32
#define RDPEUDP_CORRELATION_LEN 16

/* Handshake / transport timeouts (ms) */
#define RDPEUDP_SYN_TIMEOUT_MS 1000
#define RDPEUDP_SYN_MAX_RETRIES 5
#define RDPEUDP_ACK_TIMEOUT_MS 200
#define RDPEUDP_MAX_RETRIES 5
#define RDPEUDP_KEEPALIVE_MS 30000
/* Per-chunk reliable-send deadline. Generous on purpose: loaded peers have
 * been observed to take ~2 s before their first ACK while blasting probe
 * trains, and a 2 s deadline lost that race by milliseconds (the ACKs kept
 * arriving right after). The establishment worker is async (main session on
 * TCP is unaffected), and MS's own reference waits an order of magnitude
 * longer, so patience here is safe. */
#define RDPEUDP_SEND_TIMEOUT_MS 10000

typedef struct rdp_udp_transport rdpUdpTransport;

/* ---- v1 SYN codec (network byte order / big-endian) ---- */

typedef struct
{
	UINT32 snSourceAck;
	UINT16 receiveWindow;
	UINT16 flags;
} RdpUdpFecHeader;

typedef struct
{
	UINT32 initialSeq;
	UINT16 upstreamMtu;
	UINT16 downstreamMtu;
} RdpUdpSynPayload;

typedef struct
{
	UINT16 synexFlags;
	UINT16 version;
	BYTE cookieHash[RDPEUDP_COOKIE_HASHLEN];
	BOOL haveCookieHash;
} RdpUdpSynExPayload;

WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_write_fec_header(wStream* s, const RdpUdpFecHeader* h);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_read_fec_header(wStream* s, RdpUdpFecHeader* h);

WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpeudp_build_syn(UINT32 initialSeq, UINT16 upstreamMtu,
                                       UINT16 downstreamMtu, UINT32 ackSeq, BOOL withAck,
                                       const BYTE* correlationId, UINT16 udpVer,
                                       const BYTE* cookieHash);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_parse_syn(const BYTE* data, size_t len, RdpUdpFecHeader* header,
                                   RdpUdpSynPayload* syn, BYTE* correlationId,
                                   RdpUdpSynExPayload* synex);

/* ---- v2 packet codec (little-endian, with prefix-byte transform) ---- */

typedef struct
{
	UINT16 flags; /* 12 bits */
	UINT8 logWindow; /* 4 bits */
} RdpUdp2Header;

typedef struct
{
	UINT16 seqNum;
	BYTE receivedTs[3];
	BYTE sendGap;
	BYTE numDelayed; /* low 4 bits */
	BYTE timeScale;  /* high 4 bits */
	/* delayed acks omitted for cumulative-ACK mode */
} RdpUdp2AckPayload;

typedef struct
{
	UINT16 baseSeq;
	BYTE codedSize; /* 7 bits */
	BOOL haveTs;
	BYTE timestamp[4]; /* 3-byte ts + 1-byte gap if haveTs */
	BYTE* vector;      /* codedSize bytes, caller-owned */
	size_t vectorLen;
} RdpUdp2AckVecPayload;

/* Pure RDP-UDP2 layout codec in spec order ([MS-RDPEUDP2] 2.2.1):
 * Header, ACK, OverheadSize, DelayAckInfo, AckOfAcks, DataHeader, ACKVEC,
 * DataBody. No socket, clock, or transport state; prefix transform is
 * applied separately via rdpeudp2_protect/unprotect. */
typedef struct
{
	UINT16 flags; /* 12 bits */
	UINT8 logWindow; /* 4 bits */
	BOOL hasAck;
	UINT16 ackBase;
	BYTE ackTs[3];
	BYTE ackGap;
	BYTE ackNumDelayed;
	BYTE ackScale;
	const BYTE* ackDelayed;
	size_t ackDelayedLen;
	BOOL hasOverhead;
	BYTE overhead;
	BOOL hasDelayAck;
	BYTE delayMax;
	UINT16 delayTimeout;
	BOOL hasAoa;
	UINT16 aoa;
	BOOL hasDataHeader;
	UINT16 dataSeq;
	BOOL hasAckvec;
	const BYTE* ackvec;
	size_t ackvecLen; /* raw ACKVEC bytes: base(2)+b2(1)+[ts(4)]+vector */
	BOOL hasDataBody;
	UINT16 channelSeq;
	const BYTE* dataBody;
	size_t dataBodyLen;
} RdpUdp2Layout;

WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp2_parse_layout(const BYTE* layout, size_t len, RdpUdp2Layout* out);

WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpeudp2_encode_layout(const RdpUdp2Layout* in);

/** Apply the [MS-RDPEUDP2] 3.1.1.1.5 prefix transform for sending.
 * Takes a complete RDP-UDP2 packet layout in @p s (position = length),
 * prepends PacketPrefixByte and swaps bytes 0 and 7. */
FREERDP_API BOOL rdpeudp2_protect(wStream* s, BOOL dummy);
/** Reverse the prefix transform in place.
 * @p data/@p len is the received UDP payload. Returns FALSE if too short. */
FREERDP_API BOOL rdpeudp2_unprotect(BYTE* data, size_t len, BOOL* dummy, size_t* payloadOffset);

/* ---- MTU negotiation ([MS-RDPEUDP] 3.1.1.3, conservative without bonus) ---- */
FREERDP_API void rdpeudp_compute_mtu(UINT16 ourUp, UINT16 ourDown, UINT16 peerUp,
                                     UINT16 peerDown, UINT16* negUp, UINT16* negDown);
WINPR_ATTR_NODISCARD
FREERDP_API UINT16 rdpeudp_payload_for_mtu(UINT16 mtu);

/* ---- ACK vector codec ([MS-RDPEUDP2] 2.2.1.2.6, little-endian) ----
 * Bitmap mode (MSB 0): 7 seqs per byte, bit=1 received.
 * RLE mode (MSB 1): bit6 = state, low 6 bits = run length. */
WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpeudp_build_ackvec(UINT16 baseSeq, const BOOL* received, size_t count,
                                          BOOL withTs, UINT32 ts, BYTE sendGap);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_parse_ackvec(const BYTE* data, size_t len, UINT16* baseSeq,
                                      BOOL** received, size_t* count);

/* ---- channel framing in HigherLayerData ----
 * type(1)=0x00 + channelId(2)+totalSize(4)+flags(4)+chunkLen(2)+chunk.
 * Autodetect travels in Tunnel DATA subheaders (MS-RDPEMT 2.2.1.1.1), not here.
 * Plaintext (TLS encrypts). */
WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpeudp_build_channel_packet(UINT16 channelId, UINT32 totalSize,
                                                  UINT32 flags, const BYTE* chunk,
                                                  size_t chunkLen);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_parse_channel_packet(const BYTE* data, size_t len, UINT16* channelId,
                                              UINT32* totalSize, UINT32* flags,
                                              const BYTE** chunk, size_t* chunkLen);
/* Length of one DYNVC PDU at data (MS-RDPEDYC 2.2, header byte
 * [cbId(2)|Sp/Pri(2)|Cmd(4)] + ChannelId of cbChId width). Live servers put
 * RAW DVC PDUs in Tunnel DATA HigherLayerData with no outer envelope
 * (observed: CREATEs `18 02/07/08 <name>\0`, one PDU per Tunnel DATA).
 * Supported: CREATE (NUL scan client-side, fixed status server-side),
 * CLOSE (fixed), DATA_FIRST (exact-fit only; fragmented across Tunnel DATAs
 * is rejected visibly), DATA (to end). isServer selects the CREATE direction
 * (N4: same command nibble both ways; server receives responses).
 * FALSE when the bytes do not hold one complete supported PDU. */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_dvc_pdu_length(const BYTE* data, size_t len, BOOL isServer,
                                        size_t* pduLenOut);

/* ---- tunnel codec ([MS-RDPEMT] 2.2, little-endian) ---- */

#define RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ 0x00
#define RDP_TUNNEL_SUBHEADER_AUTODETECT_RSP 0x01

WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpemt_build_create_request(UINT32 requestId, const BYTE* cookie);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpemt_parse_create_response(const BYTE* data, size_t len, UINT32* hr);
WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpemt_build_data(const BYTE* data, size_t len);
/* Tunnel DATA with explicit subheaders + HigherLayerData (MS-RDPEMT 2.2.1.1,
 * 2.2.1.1.1, 2.2.2.3). HeaderLength = 4 + subheadersLen, PayloadLength =
 * higherLayerLen. */
WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpemt_build_tunnel_data(const BYTE* subheaders, size_t subheadersLen,
                                              const BYTE* higherLayer, size_t higherLayerLen);
/* Single subheader in overlaid form: data must be a complete RDPBCGR PDU
 * whose [0]=total length and [1]=REQ/RSP typeId; the subheader IS those
 * framing bytes (emitted verbatim after validation). */
WINPR_ATTR_NODISCARD
FREERDP_API wStream* rdpemt_build_subheader(BYTE subHeaderType, const BYTE* data, size_t dataLen);
/* Length of an autodetect PDU starting at data (0 if incomplete). Uses
 * headerLength + payloadLength fields per MS-RDPBCGR 2.2.14. */
WINPR_ATTR_NODISCARD
FREERDP_API size_t rdpemt_autodetect_pdu_length(const BYTE* data, size_t len);
/* Iterate subheaders: at *offset, returns type + FULL PDU bytes + total
 * length, advances offset past this subheader (== PDU length). FALSE when
 * done/invalid. */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpemt_next_subheader(const BYTE* subheaders, size_t subheadersLen,
                                       size_t* offset, BYTE* subHeaderType,
                                       const BYTE** subData, size_t* subDataLen);
/* Soft-Sync offer checks on whole DVC PDUs (header byte included, MS-RDPEDYC
 * 2.2.5.1/2.2.5.2). TRUE iff the PDU offers TUNNELTYPE_UDPFECR (0x01):
 * Request with no channel list and >=1 tunnel is accepted; otherwise the
 * channel/tunnel lists must reference UDPFECR. */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_soft_sync_request_offers_udp(const BYTE* pdu, size_t len);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_soft_sync_response_offers_udp(const BYTE* pdu, size_t len);
/* Strict-validated extraction of DVC IDs listed under UDPFECR tunnels.
 * Returns TRUE iff the request is valid and offers UDPFECR; *countOut gets the
 * ID count (capped: returns FALSE if more than maxIds, staying TCP-safe). */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_soft_sync_request_udp_dvcs(const BYTE* pdu, size_t len,
                                                    UINT32* outIds, size_t maxIds,
                                                    size_t* countOut);
/* Decode a 4-byte tunnel header to learn payload/subheader lengths before the
 * rest of the PDU has been read. Unlike rdpemt_parse_header it does not
 * require the full PDU to be present. */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpemt_decode_header(const BYTE* data, size_t len, BYTE* action,
                                      UINT16* payloadLen, UINT8* headerLen);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpemt_parse_header(const BYTE* data, size_t len, BYTE* action,
                                     UINT16* payloadLen, UINT8* headerLen);
/* Incremental consume of one Tunnel DATA PDU from a byte stream (R3).
 * Inspects buf[0..len) without I/O: returns 1 with *consumedOut=hlen+plen and
 * sub/payload copied out when a complete PDU is present; 0 when more bytes are
 * needed (nothing consumed); -1 on corrupt framing; -2 when complete but
 * caller buffers are too small (nothing consumed). */
WINPR_ATTR_NODISCARD
FREERDP_API int rdpemt_tunnel_consume(const BYTE* buf, size_t len, size_t* consumedOut,
                                      BYTE* subBuf, size_t subBufLen, size_t* subLenOut,
                                      BYTE* payloadBuf, size_t payloadBufLen,
                                      size_t* payloadLenOut);

/* ---- transport ---- */

WINPR_ATTR_MALLOC(rdpeudp_free, 1)
WINPR_ATTR_NODISCARD
FREERDP_LOCAL rdpUdpTransport* rdpeudp_new(rdpContext* context, const char* hostname, int port,
                                           UINT32 requestId, UINT16 requestedProto,
                                           const BYTE* securityCookie);

FREERDP_LOCAL void rdpeudp_free(rdpUdpTransport* udp);

/** Establish UDP socket + RDPEUDP handshake + version negotiation.
 * On success the reliable RDPEUDP2 data path is ready (but not yet TLS).
 * @return TRUE if UDP transport is usable (negotiated v3 / RDPEUDP2). */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL rdpeudp_connect(rdpUdpTransport* udp, DWORD timeoutMs);

/** Perform TLS handshake over the reliable UDP transport.
 * Must be called after rdpeudp_connect(). */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL rdpeudp_tls_connect(rdpUdpTransport* udp);

/** Send Tunnel Create Request and wait for Tunnel Create Response.
 * Must be called after rdpeudp_tls_connect(). */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL rdpeudp_tunnel_create(rdpUdpTransport* udp, DWORD timeoutMs);

/** Reliable stream send/recv over the established tunnel (TLS).
 * These carry RDP_TUNNEL_DATA payloads (used for future DVC migration). */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL SSIZE_T rdpeudp_tunnel_send(rdpUdpTransport* udp, const BYTE* data, size_t len);
WINPR_ATTR_NODISCARD
FREERDP_LOCAL SSIZE_T rdpeudp_tunnel_recv(rdpUdpTransport* udp, BYTE* buffer, size_t len,
                                           DWORD timeoutMs);
/* Tunnel DATA with explicit subheaders (MS-RDPEMT 2.2.1.1.1). Sends one PDU
 * with HeaderLength=4+subheadersLen and PayloadLength=higherLayerLen.
 * Returns higherLayerLen on success, -1 on error. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL SSIZE_T rdpeudp_tunnel_send_full(rdpUdpTransport* udp, const BYTE* subheaders,
                                               size_t subheadersLen, const BYTE* higherLayer,
                                               size_t higherLayerLen);
/* Single autodetect PDU as a Tunnel DATA subheader (empty HigherLayerData).
 * subHeaderType is RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ/RSP. Returns pduLen. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL SSIZE_T rdpeudp_tunnel_send_autodetect(rdpUdpTransport* udp, BYTE subHeaderType,
                                                     const BYTE* pdu, size_t pduLen);
/* Receive one Tunnel DATA PDU, splitting subheaders and HigherLayerData.
 * Returns 1 on PDU received, 0 on timeout (no PDU), -1 on error. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL int rdpeudp_tunnel_recv_full(rdpUdpTransport* udp, BYTE* subBuf, size_t subBufLen,
                                           size_t* subLenOut, BYTE* payloadBuf,
                                           size_t payloadBufLen, size_t* payloadLenOut,
                                           DWORD timeoutMs);

/* ---- unit-test driver (Q2 transport integration, no sockets) ----
 * Feeds one wire datagram through the production v2 receive path (the same
 * block rdpeudp_recv_one runs for socket input): prefix/layout parse, AOA
 * epoch rule, DataSeq window, ACK accounting, channel delivery. The sender
 * side is modeled with the production builders (rdpeudp2_encode_layout +
 * rdpeudp2_protect), so wire bytes are real in both directions. */
typedef struct
{
	UINT16 recvDataBase;
	UINT16 lastAckSent;
	UINT16 expectedChannelSeq;
	BOOL haveRecvData;
	BOOL haveSeenAoa;
	BOOL haveRealData;
	size_t recvStreamLen; /* delivered in-order channel bytes */
} RdpUdpTestRecvState;

WINPR_ATTR_MALLOC(rdpeudp_test_free, 1)
WINPR_ATTR_NODISCARD
FREERDP_API rdpUdpTransport* rdpeudp_test_new(void);
FREERDP_API void rdpeudp_test_free(rdpUdpTransport* udp);
/** Feed one datagram; TRUE if received (even if ignored, same as recv_one). */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_test_feed(rdpUdpTransport* udp, const BYTE* datagram, size_t len);
/** Snapshot scalar receive state (bitmap via rdpeudp_test_seen below). */
FREERDP_API BOOL rdpeudp_test_recv_state(const rdpUdpTransport* udp, RdpUdpTestRecvState* out);
/** TRUE if DataSeq (base+i) is recorded received; FALSE if out of range. */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL rdpeudp_test_seen(const rdpUdpTransport* udp, size_t i);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL rdpeudp_is_connected(const rdpUdpTransport* udp);
WINPR_ATTR_NODISCARD
FREERDP_LOCAL UINT16 rdpeudp_negotiated_version(const rdpUdpTransport* udp);
WINPR_ATTR_NODISCARD
FREERDP_LOCAL UINT32 rdpeudp_get_request_id(const rdpUdpTransport* udp);

/* Reliability / keepalive / stats ([MS-RDPEUDP2] 3.1.1, 3.1.5) */
typedef struct
{
	UINT64 sentPackets;
	UINT64 recvPackets;
	UINT64 retransmits;
	UINT64 lostDetected;
	UINT64 ackSent;
	UINT64 ackvecSent;
} RdpUdpStats;

FREERDP_LOCAL BOOL rdpeudp_send_keepalive(rdpUdpTransport* udp);
FREERDP_LOCAL BOOL rdpeudp_check_keepalive(rdpUdpTransport* udp, DWORD idleMs);
FREERDP_LOCAL BOOL rdpeudp_get_stats(const rdpUdpTransport* udp, RdpUdpStats* stats);
FREERDP_LOCAL int rdpeudp_get_sockfd(const rdpUdpTransport* udp);
FREERDP_LOCAL HANDLE rdpeudp_get_event(rdpUdpTransport* udp);
/* Non-owning cancel signal; checked by blocking establishment and I/O loops. */
FREERDP_LOCAL void rdpeudp_set_abort_event(rdpUdpTransport* udp, HANDLE abortEvent);
/* Drain-safe re-arm of the readiness event (reset + recheck). */
FREERDP_LOCAL void rdpeudp_update_event(rdpUdpTransport* udp);

/** BIO wrapping the reliable RDPEUDP2 stream (for TLS). */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BIO_METHOD* BIO_s_rdpeudp(void);

/* Server-side accept ([MS-RDPEUDP] 3.2, [MS-RDPEMT] 3.2.5.1).
 * Binds a UDP socket on @p port, performs SYN handshake as server,
 * TLS accept with @p settings, validates Tunnel Create against
 * @p expectedReqId/@p expectedCookie, sends Tunnel Create Response.
 * @p abortEvent (optional, non-owning) is polled for cooperative cancel.
 * Returns a connected transport or NULL. */
WINPR_ATTR_MALLOC(rdpeudp_free, 1)
WINPR_ATTR_NODISCARD
FREERDP_LOCAL rdpUdpTransport* rdpeudp_accept(rdpContext* context, int port,
                                              UINT32 expectedReqId,
                                              const BYTE* expectedCookie, DWORD timeoutMs);
WINPR_ATTR_MALLOC(rdpeudp_free, 1)
WINPR_ATTR_NODISCARD
FREERDP_LOCAL rdpUdpTransport* rdpeudp_accept_ex(rdpContext* context, int port,
                                                 UINT32 expectedReqId,
                                                 const BYTE* expectedCookie, DWORD timeoutMs,
                                                 HANDLE abortEvent);

#endif /* FREERDP_LIB_CORE_RDPEUDP_H */
