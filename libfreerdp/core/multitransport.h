/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Multitransport PDUs
 *
 * Copyright 2014 Dell Software <Mike.McDonald@software.dell.com>
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

#ifndef FREERDP_LIB_CORE_MULTITRANSPORT_H
#define FREERDP_LIB_CORE_MULTITRANSPORT_H

typedef struct rdp_multitransport rdpMultitransport;

#include "rdp.h"
#include "state.h"

#include <freerdp/freerdp.h>
#include <freerdp/api.h>

#include <winpr/stream.h>

#include "rdpeudp.h"

typedef enum
{
	INITIATE_REQUEST_PROTOCOL_UDPFECR = 0x01,
	INITIATE_REQUEST_PROTOCOL_UDPFECL = 0x02
} MultitransportRequestProtocol;

typedef state_run_t (*MultiTransportRequestCb)(rdpMultitransport* multi, UINT32 reqId,
                                               UINT16 reqProto, const BYTE* cookie);
typedef state_run_t (*MultiTransportResponseCb)(rdpMultitransport* multi, UINT32 reqId,
                                                UINT32 hrResponse);

#define RDPUDP_COOKIE_LEN 16
#define RDPUDP_COOKIE_HASHLEN 32

WINPR_ATTR_NODISCARD
FREERDP_LOCAL state_run_t multitransport_recv_request(rdpMultitransport* multi, wStream* s);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL state_run_t multitransport_server_request(rdpMultitransport* multi, UINT16 reqProto);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL state_run_t multitransport_recv_response(rdpMultitransport* multi, wStream* s);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_client_send_response(rdpMultitransport* multi, UINT32 reqId,
                                                       HRESULT hr);

FREERDP_LOCAL void multitransport_free(rdpMultitransport* multi);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_is_udp_connected(const rdpMultitransport* multi);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL rdpUdpTransport* multitransport_get_udp(rdpMultitransport* multi);

/* Migration gating (MS-RDPEDYC 3.1.5.3 Soft-Sync, MS-RDPEMT 1.3).
 * Tunnel establishment alone does NOT authorize DVC migration when Soft-Sync
 * was negotiated. Send/recv each require their direction to be migrated. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_is_udp_send_migrated(const rdpMultitransport* multi);
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_is_udp_recv_migrated(const rdpMultitransport* multi);
/* Soft-Sync event hooks (called from DRDYNVC layer on TCP when PDUs observed):
 * - request sent (server, whole PDU): installs the mapping, enables send.
 * - request chunks received (client, per static-channel chunk): feeds the
 *   reassembly buffer; on the LAST chunk the validated mapping is installed
 *   and recv is enabled. Handles fragmented requests the single-chunk fast
 *   path cannot see.
 * - response sent (client) / received (server): complete the handshake, but
 *   only when a validated mapping was installed first (never migrate-all on
 *   failure). Request hooks take the whole DVC PDU (header byte included) and
 *   strict-parse it; malformed or non-UDPFECR requests change nothing (TCP-safe).
 * Without Soft-Sync negotiation these are no-ops (migration immediate). */
FREERDP_LOCAL void multitransport_on_soft_sync_request_sent(rdpMultitransport* multi,
                                                             const BYTE* pdu, size_t len);
FREERDP_LOCAL void multitransport_soft_sync_recv_feed(rdpMultitransport* multi, const BYTE* chunk,
                                                      size_t chunkLen, UINT32 flags);
FREERDP_LOCAL void multitransport_on_soft_sync_response_sent(rdpMultitransport* multi);
FREERDP_LOCAL void multitransport_on_soft_sync_response_received(rdpMultitransport* multi);
/* Per-DVC send migration (S2): TRUE iff the direction is migrated AND (no active
 * mapping OR dvcId was listed under UDPFECR). Unlisted DVCs stay on TCP. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_is_dvc_migrated(const rdpMultitransport* multi,
                                                   UINT32 dvcId);

/* Channel data over UDP ([MS-RDPEMT] RDP_TUNNEL_DATA + plaintext channel PDU).
 * Tries UDP tunnel first if connected and channel is UDP-capable (drdynvc);
 * returns TRUE if sent over UDP, FALSE to fall back to TCP. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_send_channel_packet(rdpMultitransport* multi, UINT16 channelId,
                                                       size_t totalSize, UINT32 flags,
                                                       const BYTE* chunk, size_t chunkLen);

/* Autodetect PDU over UDP ([MS-RDPBCGR] 2.2.14 via RDP_TUNNEL_DATA).
 * isRequest selects SEC_AUTODETECT_REQ vs RSP framing. Returns TRUE if sent. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL multitransport_send_autodetect(rdpMultitransport* multi, BOOL isRequest,
                                                   UINT16 secFlags, const BYTE* pdu,
                                                   size_t pduLen);

/* Background receive for Tunnel DATA channel packets.
 * Polls UDP tunnel (non-blocking) and dispatches to channel layer.
 * Returns >0 if a packet was dispatched, 0 if none, <0 on error. */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL int multitransport_check_fds(rdpMultitransport* multi);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL HANDLE multitransport_get_event(rdpMultitransport* multi);

WINPR_ATTR_MALLOC(multitransport_free, 1)
WINPR_ATTR_NODISCARD
FREERDP_LOCAL rdpMultitransport* multitransport_new(rdpRdp* rdp, UINT16 protocol);

/* ---- unit-test driver (no sockets; soft-sync mapping paths only) ----
 * Fixture transport comes up negotiated with a connected (socketless) UDP
 * transport, so the mapping install/feed hooks run exactly as in production.
 * multitransport_test_fail_alloc_after arms OOM injection for the mapping and
 * reassembly allocations: n >= 0 fails the (n+1)-th wrapped allocation (0 =
 * fail next), negative disables. Single-threaded test use only. */
WINPR_ATTR_MALLOC(multitransport_test_free, 1)
WINPR_ATTR_NODISCARD
FREERDP_API rdpMultitransport* multitransport_test_new(void);
FREERDP_API void multitransport_test_free(rdpMultitransport* multi);
FREERDP_API void multitransport_test_fail_alloc_after(int n);
FREERDP_API void multitransport_test_recv_feed(rdpMultitransport* multi, const BYTE* chunk,
                                               size_t chunkLen, UINT32 flags);
FREERDP_API void multitransport_test_request_sent(rdpMultitransport* multi, const BYTE* pdu,
                                                  size_t len);
FREERDP_API void multitransport_test_response_sent(rdpMultitransport* multi);
FREERDP_API void multitransport_test_response_received(rdpMultitransport* multi);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL multitransport_test_recvmigrated(const rdpMultitransport* multi);
WINPR_ATTR_NODISCARD
FREERDP_API BOOL multitransport_test_sendmigrated(const rdpMultitransport* multi);
/** Routing decision independent of connection: installed && (migrate-all or listed). */
WINPR_ATTR_NODISCARD
FREERDP_API BOOL multitransport_test_dvc_routed(const rdpMultitransport* multi, UINT32 dvcId);

#endif /* FREERDP_LIB_CORE_MULTITRANSPORT_H */
