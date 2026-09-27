/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * drdynvc Utils - Helper functions converting something to string
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

#include <freerdp/utils/drdynvc.h>
#include <freerdp/channels/drdynvc.h>

#define SOFT_SYNC_CMD_REQUEST 0x08
#define SOFT_SYNC_CMD_RESPONSE 0x09
#define SOFT_SYNC_FLAG_TCP_FLUSHED 0x01
#define SOFT_SYNC_FLAG_CHANNELLIST 0x02

const char* drdynvc_get_packet_type(BYTE cmd)
{
	switch (cmd)
	{
		case CREATE_REQUEST_PDU:
			return "CREATE_REQUEST_PDU";
		case DATA_FIRST_PDU:
			return "DATA_FIRST_PDU";
		case DATA_PDU:
			return "DATA_PDU";
		case CLOSE_REQUEST_PDU:
			return "CLOSE_REQUEST_PDU";
		case CAPABILITY_REQUEST_PDU:
			return "CAPABILITY_REQUEST_PDU";
		case DATA_FIRST_COMPRESSED_PDU:
			return "DATA_FIRST_COMPRESSED_PDU";
		case DATA_COMPRESSED_PDU:
			return "DATA_COMPRESSED_PDU";
		case SOFT_SYNC_REQUEST_PDU:
			return "SOFT_SYNC_REQUEST_PDU";
		case SOFT_SYNC_RESPONSE_PDU:
			return "SOFT_SYNC_RESPONSE_PDU";
		default:
			return "UNKNOWN";
	}
}

/* Strict Soft-Sync Request parser shared by the DVC managers and the
 * multitransport layer (MS-RDPEDYC 2.2.5.1/2.2.5.1.1). Whole PDU including the
 * header byte (Cmd high nibble, must be 0x08). Requires Pad == 0, mandatory
 * TCP_FLUSHED, exact len == 2 + Length, and fully valid tunnel/channel lists
 * with no trailing bytes. All lists are validated even after finding UDPFECR.
 * UDPFECR DVC IDs are collected when outIds is given (two-phase: NULL counts).
 */
static BOOL soft_sync_request_parse(const BYTE* pdu, size_t len, BOOL* offersUdp,
                                    UINT32* udpDvcIds, size_t maxIds, size_t* udpDvcCount)
{
	UINT32 length = 0;
	UINT16 flags = 0;
	UINT16 numTunnels = 0;
	size_t off = 0;
	BOOL offers = FALSE;
	size_t udpCount = 0;
	if (offersUdp)
		*offersUdp = FALSE;
	if (udpDvcCount)
		*udpDvcCount = 0;
	if (!pdu || (len < 10))
		return FALSE;
	if (((pdu[0] >> 4) & 0x0F) != SOFT_SYNC_CMD_REQUEST)
		return FALSE;
	if (pdu[1] != 0x00) /* Pad */
		return FALSE;
	length = (UINT32)pdu[2] | ((UINT32)pdu[3] << 8) | ((UINT32)pdu[4] << 16) |
	         ((UINT32)pdu[5] << 24);
	flags = (UINT16)(pdu[6] | ((UINT16)pdu[7] << 8));
	numTunnels = (UINT16)(pdu[8] | ((UINT16)pdu[9] << 8));
	if (length < 8)
		return FALSE;
	if (len != (size_t)2 + length)
		return FALSE; /* declared length must match supplied bytes */
	if (!(flags & SOFT_SYNC_FLAG_TCP_FLUSHED))
		return FALSE; /* mandatory completion barrier */
	if (numTunnels > DRDYNVC_SOFT_SYNC_MAX_TUNNELS)
		return FALSE;
	off = 10;
	if (!(flags & SOFT_SYNC_FLAG_CHANNELLIST))
	{
		/* No lists: total must be exactly header+Pad+Length+Flags+Tunnels. */
		if (len != 10)
			return FALSE;
		offers = (numTunnels > 0);
		if (offersUdp)
			*offersUdp = offers;
		return TRUE;
	}
	for (UINT16 ti = 0; ti < numTunnels; ti++)
	{
		UINT32 tunnelType = 0;
		UINT16 numDvcs = 0;
		if (off + 6 > len)
			return FALSE;
		tunnelType = (UINT32)pdu[off] | ((UINT32)pdu[off + 1] << 8) |
		             ((UINT32)pdu[off + 2] << 16) | ((UINT32)pdu[off + 3] << 24);
		numDvcs = (UINT16)(pdu[off + 4] | ((UINT16)pdu[off + 5] << 8));
		if (numDvcs > DRDYNVC_SOFT_SYNC_MAX_DVCS_PER_LIST)
			return FALSE;
		if (off + 6 + (size_t)numDvcs * 4 > len)
			return FALSE;
		if (tunnelType == DRDYNVC_TUNNELTYPE_UDPFECR)
		{
			offers = TRUE;
			for (UINT16 di = 0; di < numDvcs; di++)
			{
				const size_t o = off + 6 + (size_t)di * 4;
				const UINT32 id = (UINT32)pdu[o] | ((UINT32)pdu[o + 1] << 8) |
				                  ((UINT32)pdu[o + 2] << 16) | ((UINT32)pdu[o + 3] << 24);
				if (udpDvcIds && (udpCount < maxIds))
					udpDvcIds[udpCount] = id;
				udpCount++;
			}
		}
		off += 6 + (size_t)numDvcs * 4;
	}
	if (off != len)
		return FALSE; /* trailing bytes */
	if (udpDvcCount)
		*udpDvcCount = udpCount;
	if (offersUdp)
		*offersUdp = offers;
	/* Truncation guard: caller must size for the full set; report invalid so
	 * routing stays TCP-safe instead of migrating a subset. */
	if (offers && udpDvcIds && (udpCount > maxIds))
		return FALSE;
	return TRUE;
}

BOOL drdynvc_soft_sync_request_validate(const BYTE* pdu, size_t len)
{
	BOOL offers = FALSE;
	size_t count = 0;
	/* Validate only: offers/count outputs unused but parsing is identical. */
	return soft_sync_request_parse(pdu, len, &offers, nullptr, 0, &count);
}

BOOL drdynvc_soft_sync_request_offers_udp(const BYTE* pdu, size_t len)
{
	BOOL offers = FALSE;
	return soft_sync_request_parse(pdu, len, &offers, nullptr, 0, nullptr) && offers;
}

BOOL drdynvc_soft_sync_request_udp_dvcs(const BYTE* pdu, size_t len, UINT32* outIds,
                                        size_t maxIds, size_t* countOut)
{
	BOOL offers = FALSE;
	size_t count = 0;
	if (!soft_sync_request_parse(pdu, len, &offers, outIds, maxIds, &count))
		return FALSE;
	if (countOut)
		*countOut = count;
	return offers;
}

BOOL drdynvc_soft_sync_response_offers_udp(const BYTE* pdu, size_t len)
{
	UINT32 numTunnels = 0;
	BOOL offers = FALSE;
	if (!pdu || (len < 6))
		return FALSE;
	if (((pdu[0] >> 4) & 0x0F) != SOFT_SYNC_CMD_RESPONSE)
		return FALSE;
	if (pdu[1] != 0x00) /* Pad */
		return FALSE;
	numTunnels = (UINT32)pdu[2] | ((UINT32)pdu[3] << 8) | ((UINT32)pdu[4] << 16) |
	             ((UINT32)pdu[5] << 24);
	if (numTunnels > DRDYNVC_SOFT_SYNC_MAX_TUNNELS)
		return FALSE;
	if (len != 6 + (size_t)numTunnels * 4)
		return FALSE; /* exact match: no trailing bytes */
	for (UINT32 i = 0; i < numTunnels; i++)
	{
		const size_t o = 6 + (size_t)i * 4;
		const UINT32 tt = (UINT32)pdu[o] | ((UINT32)pdu[o + 1] << 8) |
		                  ((UINT32)pdu[o + 2] << 16) | ((UINT32)pdu[o + 3] << 24);
		if (tt == DRDYNVC_TUNNELTYPE_UDPFECR)
			offers = TRUE;
	}
	return offers;
}
