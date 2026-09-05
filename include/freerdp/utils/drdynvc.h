/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * GFX Utils - Helper functions converting something to string
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
 * Copyright 2023 Thincast Technologies GmbH
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

#ifndef FREERDP_UTILS_DRDYNVC_H
#define FREERDP_UTILS_DRDYNVC_H

#include <winpr/wtypes.h>
#include <freerdp/api.h>

#ifdef __cplusplus
extern "C"
{
#endif

	WINPR_ATTR_NODISCARD
	FREERDP_API const char* drdynvc_get_packet_type(BYTE cmd);

/* Strict Soft-Sync validation (MS-RDPEDYC 2.2.5.1/2.2.5.1.1/2.2.5.2).
 *
 * Shared by the DVC client/server managers and the multitransport layer so
 * handler processing, mapping, and response generation always agree on what
 * a well-formed request is. All functions take the whole DVC PDU including
 * the header byte (Cmd in the high nibble).
 *
 * Bounds shared by every user (DoS guards, not protocol limits):
 */
#define DRDYNVC_SOFT_SYNC_MAX_TUNNELS 16
#define DRDYNVC_SOFT_SYNC_MAX_DVCS_PER_LIST 1024

#define DRDYNVC_TUNNELTYPE_UDPFECR 0x00000001
#define DRDYNVC_TUNNELTYPE_UDPFECL 0x00000003

	/* Well-formedness only: header Cmd, Pad == 0, TCP_FLUSHED set, exact
	 * len == 2 + Length, tunnel/channel lists fully valid with no trailing
	 * bytes. */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL drdynvc_soft_sync_request_validate(const BYTE* pdu, size_t len);

	/* TRUE iff valid (as above) and UDPFECR is offered: no-list requests with
	 * at least one tunnel, or a listed UDPFECR tunnel entry. */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL drdynvc_soft_sync_request_offers_udp(const BYTE* pdu, size_t len);

	/* DVC IDs listed under UDPFECR tunnels. Two-phase: NULL/0 counts. FALSE
	 * unless valid and offering; when offering with an empty (no-list) request
	 * *countOut is 0. Truncates nothing: fails if more than maxIds. */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL drdynvc_soft_sync_request_udp_dvcs(const BYTE* pdu, size_t len,
	                                                    UINT32* outIds, size_t maxIds,
	                                                    size_t* countOut);

	/* Response: exact 6 + 4 * NumberOfTunnels bytes, Pad == 0, TRUE iff any
	 * entry is UDPFECR. */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL drdynvc_soft_sync_response_offers_udp(const BYTE* pdu, size_t len);

#ifdef __cplusplus
}
#endif

#endif
