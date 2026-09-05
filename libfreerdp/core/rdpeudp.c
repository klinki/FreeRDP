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

#include <freerdp/config.h>

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/crypto.h>
#include <winpr/print.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/winsock.h>

#include <freerdp/log.h>

#include <openssl/bio.h>
#include <openssl/sha.h>

#include "udp.h"
#include "rdpeudp.h"
#include "tcp.h"
#include "multitransport.h"
#include "../crypto/tls.h"

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#define TAG FREERDP_TAG("core.rdpeudp")

BOOL rdpeudp_check_keepalive(rdpUdpTransport* udp, DWORD idleMs);

/* ------------------------------------------------------------------ */
/* v1 codec (big-endian on the wire, see tools/wireshark/rdp-udp.lua) */
/* ------------------------------------------------------------------ */

BOOL rdpeudp_write_fec_header(wStream* s, const RdpUdpFecHeader* h)
{
	WINPR_ASSERT(s);
	WINPR_ASSERT(h);
	if (!Stream_EnsureRemainingCapacity(s, 8))
		return FALSE;
	Stream_Write_UINT32_BE(s, h->snSourceAck);
	Stream_Write_UINT16_BE(s, h->receiveWindow);
	Stream_Write_UINT16_BE(s, h->flags);
	return TRUE;
}

BOOL rdpeudp_read_fec_header(wStream* s, RdpUdpFecHeader* h)
{
	WINPR_ASSERT(s);
	WINPR_ASSERT(h);
	if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
		return FALSE;
	Stream_Read_UINT32_BE(s, h->snSourceAck);
	Stream_Read_UINT16_BE(s, h->receiveWindow);
	Stream_Read_UINT16_BE(s, h->flags);
	return TRUE;
}

wStream* rdpeudp_build_syn(UINT32 initialSeq, UINT16 upstreamMtu, UINT16 downstreamMtu,
                           UINT32 ackSeq, BOOL withAck, const BYTE* correlationId,
                           UINT16 udpVer, const BYTE* cookieHash)
{
	wStream* s = Stream_New(nullptr, upstreamMtu);
	if (!s)
		return nullptr;

	RdpUdpFecHeader h = { 0 };
	h.snSourceAck = withAck ? ackSeq : 0xFFFFFFFFu;
	h.receiveWindow = 128;
	h.flags = RDPUDP_FLAG_SYN | RDPUDP_FLAG_SYNEX;
	if (correlationId)
		h.flags |= RDPUDP_FLAG_CORRELATIONID;
	if (withAck)
		h.flags |= RDPUDP_FLAG_ACK;

	if (!rdpeudp_write_fec_header(s, &h))
		goto fail;

	/* RDPUDP_SYNDATA_PAYLOAD */
	Stream_Write_UINT32_BE(s, initialSeq);
	Stream_Write_UINT16_BE(s, upstreamMtu);
	Stream_Write_UINT16_BE(s, downstreamMtu);

	/* RDPUDP_CORRELATION_ID_PAYLOAD: 16 bytes id + 16 bytes reserved */
	if (correlationId)
	{
		Stream_Write(s, correlationId, RDPEUDP_CORRELATION_LEN);
		Stream_Zero(s, 16);
	}

	/* RDPUDP_SYNDATAEX_PAYLOAD */
	Stream_Write_UINT16_BE(s, RDPUDP_SYNEX_VERSION_VALID);
	Stream_Write_UINT16_BE(s, udpVer);
	if ((udpVer == RDPUDP_PROTOCOL_VERSION_3) && cookieHash)
		Stream_Write(s, cookieHash, RDPEUDP_COOKIE_HASHLEN);

	/* Pad to MTU (spec mandates zero padding to min(up,down)) */
	{
		const size_t pos = Stream_GetPosition(s);
		const size_t want = (size_t)upstreamMtu < downstreamMtu ? upstreamMtu : downstreamMtu;
		if (pos < want)
		{
			if (!Stream_EnsureRemainingCapacity(s, want - pos))
				goto fail;
			Stream_Zero(s, want - pos);
		}
	}

	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
		goto fail;
	return s;

fail:
	Stream_Release(s);
	return nullptr;
}

BOOL rdpeudp_parse_syn(const BYTE* data, size_t len, RdpUdpFecHeader* header,
                       RdpUdpSynPayload* syn, BYTE* correlationId, RdpUdpSynExPayload* synex)
{
	WINPR_ASSERT(data);
	WINPR_ASSERT(header);
	if (len < 8)
		return FALSE;

	wStream sbuffer = { 0 };
	wStream* s = Stream_StaticConstInit(&sbuffer, data, len);
	if (!rdpeudp_read_fec_header(s, header))
		return FALSE;

	if (!(header->flags & RDPUDP_FLAG_SYN))
		return FALSE;

	if (syn)
	{
		if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
			return FALSE;
		Stream_Read_UINT32_BE(s, syn->initialSeq);
		Stream_Read_UINT16_BE(s, syn->upstreamMtu);
		Stream_Read_UINT16_BE(s, syn->downstreamMtu);
	}
	else
	{
		if (!Stream_SafeSeek(s, 8))
			return FALSE;
	}

	if (header->flags & RDPUDP_FLAG_CORRELATIONID)
	{
		if (!Stream_CheckAndLogRequiredLength(TAG, s, 32))
			return FALSE;
		if (correlationId)
			Stream_Read(s, correlationId, RDPEUDP_CORRELATION_LEN);
		else
			Stream_Seek(s, RDPEUDP_CORRELATION_LEN);
		Stream_Seek(s, 16); /* reserved */
	}

	if ((header->flags & RDPUDP_FLAG_SYNEX) && synex)
	{
		if (!Stream_CheckAndLogRequiredLength(TAG, s, 4))
			return FALSE;
		Stream_Read_UINT16_BE(s, synex->synexFlags);
		Stream_Read_UINT16_BE(s, synex->version);
		synex->haveCookieHash = FALSE;
		if ((synex->version == RDPUDP_PROTOCOL_VERSION_3) &&
		    (Stream_GetRemainingLength(s) >= RDPEUDP_COOKIE_HASHLEN))
		{
			/* Only present in client->server SYN for v3 */
			if (!(header->flags & RDPUDP_FLAG_ACK))
			{
				Stream_Read(s, synex->cookieHash, RDPEUDP_COOKIE_HASHLEN);
				synex->haveCookieHash = TRUE;
			}
		}
	}

	return TRUE;
}

/* ------------------------------------------------------------------ */
/* v2 prefix transform ([MS-RDPEUDP2] 3.1.1.1.5)                        */
/* ------------------------------------------------------------------ */

static BYTE rdpeudp2_make_prefix(BOOL dummy, size_t layoutLen)
{
	BYTE shortLen = 7;
	if (layoutLen < 7)
		shortLen = (BYTE)(layoutLen & 0x07);
	const BYTE type = dummy ? 8 : 0;
	/* A(1)=0 | B(4)=type | C(3)=shortLen */
	return (BYTE)(((type & 0x0F) << 3) | (shortLen & 0x07));
}

BOOL rdpeudp2_protect(wStream* s, BOOL dummy)
{
	WINPR_ASSERT(s);
	size_t layoutLen = Stream_GetPosition(s);
	if (layoutLen > FREERDP_UDP_MAX_DATAGRAM - 1)
		return FALSE;

	/* Pad short packets to 7 bytes */
	size_t paddedLen = layoutLen;
	BYTE shortLen = 7;
	if (layoutLen < 7)
	{
		shortLen = (BYTE)layoutLen;
		paddedLen = 7;
		if (!Stream_EnsureRemainingCapacity(s, paddedLen - layoutLen))
			return FALSE;
		Stream_Zero(s, paddedLen - layoutLen);
		layoutLen = paddedLen;
	}

	const BYTE prefix = rdpeudp2_make_prefix(dummy, shortLen == 7 ? layoutLen : shortLen);
	/* Need 1 extra byte for prefix */
	if (!Stream_EnsureRemainingCapacity(s, 1))
		return FALSE;

	BYTE* buf = Stream_Buffer(s);
	/* shift layout right by 1 */
	memmove(buf + 1, buf, layoutLen);
	buf[0] = prefix;
	const size_t total = layoutLen + 1;
	if (!Stream_SetPosition(s, total))
		return FALSE;
	Stream_SealLength(s);

	/* swap bytes 0 and 7 */
	if (total >= 8)
	{
		BYTE tmp = buf[0];
		buf[0] = buf[7];
		buf[7] = tmp;
	}

	if (!Stream_SetPosition(s, 0))
		return FALSE;
	return TRUE;
}

BOOL rdpeudp2_unprotect(BYTE* data, size_t len, BOOL* dummy, size_t* payloadOffset)
{
	WINPR_ASSERT(data);
	if (len < 8)
		return FALSE;

	/* reverse swap of bytes 0 and 7 */
	BYTE tmp = data[0];
	data[0] = data[7];
	data[7] = tmp;

	const BYTE prefix = data[0];
	BOOL isDummy = FALSE;
	/* Spec: type in bits 6..3. Be lenient: also accept lua-style mask. */
	const BYTE typeSpec = (BYTE)((prefix >> 3) & 0x0F);
	const BYTE typeLua = (BYTE)(prefix & 0x1E);
	if ((typeSpec == 8) || (typeLua == 8))
		isDummy = TRUE;

	if (dummy)
		*dummy = isDummy;
	if (payloadOffset)
		*payloadOffset = 1;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* tunnel codec ([MS-RDPEMT] 2.2, little-endian)                        */
/* ------------------------------------------------------------------ */

BOOL rdpemt_decode_header(const BYTE* data, size_t len, BYTE* action, UINT16* payloadLen,
                          UINT8* headerLen)
{
	if (!data || (len < 4))
		return FALSE;
	const BYTE b0 = data[0];
	const BYTE act = (BYTE)(b0 & 0x0F);
	const BYTE flags = (BYTE)((b0 >> 4) & 0x0F);
	if (flags != 0)
		return FALSE;
	if ((act != RDPTUNNEL_ACTION_CREATEREQUEST) && (act != RDPTUNNEL_ACTION_CREATERESPONSE) &&
	    (act != RDPTUNNEL_ACTION_DATA))
		return FALSE;

	const UINT16 plen = (UINT16)(data[1] | ((UINT16)data[2] << 8));
	const UINT8 hlen = data[3];
	if (hlen < 4)
		return FALSE;

	if (action)
		*action = act;
	if (payloadLen)
		*payloadLen = plen;
	if (headerLen)
		*headerLen = hlen;
	return TRUE;
}

BOOL rdpemt_parse_header(const BYTE* data, size_t len, BYTE* action, UINT16* payloadLen,
                         UINT8* headerLen)
{
	BYTE act = 0;
	UINT16 plen = 0;
	UINT8 hlen = 0;
	if (!rdpemt_decode_header(data, len, &act, &plen, &hlen))
		return FALSE;
	if ((hlen > len) || (len < (size_t)hlen + plen))
		return FALSE;

	if (action)
		*action = act;
	if (payloadLen)
		*payloadLen = plen;
	if (headerLen)
		*headerLen = hlen;
	return TRUE;
}

int rdpemt_tunnel_consume(const BYTE* buf, size_t len, size_t* consumedOut, BYTE* subBuf,
                          size_t subBufLen, size_t* subLenOut, BYTE* payloadBuf,
                          size_t payloadBufLen, size_t* payloadLenOut)
{
	BYTE action = 0;
	UINT16 plen = 0;
	UINT8 hlen = 0;
	size_t subLen = 0;
	size_t total = 0;
	if (!buf)
		return -1;
	if (len < 4)
		return 0; /* need header */
	if (!rdpemt_decode_header(buf, len, &action, &plen, &hlen))
		return -1; /* corrupt */
	if ((action != RDPTUNNEL_ACTION_DATA) || (hlen < 4))
		return -1; /* corrupt */
	subLen = (size_t)hlen - 4;
	total = (size_t)hlen + plen;
	if (len < total)
		return 0; /* incomplete */
	if ((subLen > subBufLen) || ((size_t)plen > payloadBufLen))
		return -2; /* complete but caller buffers too small */
	if ((subLen > 0) && subBuf)
		memcpy(subBuf, buf + 4, subLen);
	if (((size_t)plen > 0) && payloadBuf)
		memcpy(payloadBuf, buf + hlen, plen);
	if (consumedOut)
		*consumedOut = total;
	if (subLenOut)
		*subLenOut = subLen;
	if (payloadLenOut)
		*payloadLenOut = plen;
	return 1;
}

static BOOL rdpemt_write_header_ex(wStream* s, BYTE action, UINT16 payloadLen,
                                     const BYTE* subheaders, size_t subheadersLen)
{
	WINPR_ASSERT(s);
	if (subheadersLen > 255 - 4)
		return FALSE;
	const size_t hlen = 4 + subheadersLen;
	if (!Stream_EnsureRemainingCapacity(s, hlen + payloadLen))
		return FALSE;
	Stream_Write_UINT8(s, (BYTE)(action & 0x0F));
	Stream_Write_UINT16(s, payloadLen);
	Stream_Write_UINT8(s, (UINT8)hlen);
	if (subheadersLen > 0)
		Stream_Write(s, subheaders, subheadersLen);
	return TRUE;
}

static BOOL rdpemt_write_header(wStream* s, BYTE action, UINT16 payloadLen)
{
	return rdpemt_write_header_ex(s, action, payloadLen, nullptr, 0);
}

wStream* rdpemt_build_create_request(UINT32 requestId, const BYTE* cookie)
{
	WINPR_ASSERT(cookie);
	wStream* s = Stream_New(nullptr, 32);
	if (!s)
		return nullptr;
	if (!rdpemt_write_header(s, RDPTUNNEL_ACTION_CREATEREQUEST, 24))
		goto fail;
	Stream_Write_UINT32(s, requestId);
	Stream_Write_UINT32(s, 0); /* reserved */
	Stream_Write(s, cookie, RDPEUDP_COOKIE_LEN);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
		goto fail;
	return s;
fail:
	Stream_Release(s);
	return nullptr;
}

BOOL rdpemt_parse_create_response(const BYTE* data, size_t len, UINT32* hr)
{
	BYTE action = 0;
	UINT16 plen = 0;
	UINT8 hlen = 0;
	if (!rdpemt_parse_header(data, len, &action, &plen, &hlen))
		return FALSE;
	if (action != RDPTUNNEL_ACTION_CREATERESPONSE)
		return FALSE;
	if (plen != 4)
		return FALSE;
	if (hr)
	{
		*hr = (UINT32)(data[hlen] | ((UINT32)data[hlen + 1] << 8) |
		               ((UINT32)data[hlen + 2] << 16) | ((UINT32)data[hlen + 3] << 24));
	}
	return TRUE;
}

wStream* rdpemt_build_data(const BYTE* data, size_t len)
{
	if (!data && (len != 0))
		return nullptr;
	if (len > UINT16_MAX)
		return nullptr;
	wStream* s = Stream_New(nullptr, len + 4);
	if (!s)
		return nullptr;
	if (!rdpemt_write_header(s, RDPTUNNEL_ACTION_DATA, (UINT16)len))
		goto fail;
	if (len > 0)
		Stream_Write(s, data, len);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
		goto fail;
	return s;
fail:
	Stream_Release(s);
	return nullptr;
}

wStream* rdpemt_build_tunnel_data(const BYTE* subheaders, size_t subheadersLen,
                                  const BYTE* higherLayer, size_t higherLayerLen)
{
	if (!subheaders && (subheadersLen != 0))
		return nullptr;
	if (!higherLayer && (higherLayerLen != 0))
		return nullptr;
	if ((subheadersLen > 251) || (higherLayerLen > UINT16_MAX))
		return nullptr;
	wStream* s = Stream_New(nullptr, 4 + subheadersLen + higherLayerLen);
	if (!s)
		return nullptr;
	if (!rdpemt_write_header_ex(s, RDPTUNNEL_ACTION_DATA, (UINT16)higherLayerLen,
	                            subheaders, subheadersLen))
		goto fail;
	if (higherLayerLen > 0)
		Stream_Write(s, higherLayer, higherLayerLen);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
		goto fail;
	return s;
fail:
	Stream_Release(s);
	return nullptr;
}

wStream* rdpemt_build_subheader(BYTE subHeaderType, const BYTE* data, size_t dataLen)
{
	if (!data && (dataLen != 0))
		return nullptr;
	if (dataLen > 4096)
		return nullptr;
	if ((subHeaderType != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) &&
	    (subHeaderType != RDP_TUNNEL_SUBHEADER_AUTODETECT_RSP))
		return nullptr;
	wStream* s = Stream_New(nullptr, 2 + dataLen);
	if (!s)
		return nullptr;
	/* SubHeaderLength = header fields len (Length+Type only, no extras). */
	Stream_Write_UINT8(s, 2);
	Stream_Write_UINT8(s, subHeaderType);
	if (dataLen > 0)
		Stream_Write(s, data, dataLen);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
	{
		Stream_Release(s);
		return nullptr;
	}
	return s;
}

size_t rdpemt_autodetect_pdu_length(const BYTE* data, size_t len)
{
	UINT8 headerLength = 0;
	if (!data || (len < 6))
		return 0;
	headerLength = data[0];
	if (headerLength < 6)
		return 0;
	if ((headerLength != 0x06) && (headerLength != 0x08) && (headerLength != 0x0E) &&
	    (headerLength != 0x12))
	{
		/* Unknown headerLength; treat as headerLength == total if plausible. */
		if ((size_t)headerLength > len)
			return 0;
		return headerLength;
	}
	if (headerLength == 0x06)
	{
		if (len < 6)
			return 0;
		return 6;
	}
	if (headerLength == 0x0E)
	{
		if (len < 14)
			return 0;
		return 14;
	}
	if (headerLength == 0x12)
	{
		if (len < 18)
			return 0;
		return 18;
	}
	/* headerLength == 0x08: base 8 + payloadLength(UINT16 at offset 6) + payload */
	if (len < 8)
		return 0;
	const UINT16 payloadLength = (UINT16)(data[6] | ((UINT16)data[7] << 8));
	if ((size_t)8 + payloadLength > len)
		return 0;
	/* payloadLength already validated against len; total fits. */
	return 8 + payloadLength;
}

BOOL rdpemt_next_subheader(const BYTE* subheaders, size_t subheadersLen, size_t* offset,
                           BYTE* subHeaderType, const BYTE** subData, size_t* subDataLen)
{
	size_t off = 0;
	if (!subheaders || !offset || (*offset >= subheadersLen))
		return FALSE;
	off = *offset;
	if (subheadersLen - off < 2)
		return FALSE;
	const BYTE subLen = subheaders[off];
	const BYTE subType = subheaders[off + 1];
	if (subLen < 2)
		return FALSE;
	if ((subType != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) &&
	    (subType != RDP_TUNNEL_SUBHEADER_AUTODETECT_RSP))
		return FALSE;
	/* SubHeaderData starts after Length+Type; its length is determined by the
	 * embedded autodetect PDU length (SubHeaderLength is header-fields len). */
	const BYTE* pdu = subheaders + off + 2;
	const size_t avail = subheadersLen - off - 2;
	const size_t pduLen = rdpemt_autodetect_pdu_length(pdu, avail);
	if (pduLen == 0)
		return FALSE;
	if (subHeaderType)
		*subHeaderType = subType;
	if (subData)
		*subData = pdu;
	if (subDataLen)
		*subDataLen = pduLen;
	*offset = off + 2 + pduLen;
	return TRUE;
}

#define SOFT_SYNC_CMD_REQUEST 0x08
#define SOFT_SYNC_CMD_RESPONSE 0x09
#define SOFT_SYNC_FLAG_TCP_FLUSHED 0x01
#define SOFT_SYNC_FLAG_CHANNELLIST 0x02
#define SOFT_SYNC_TUNNEL_UDPFECR 0x00000001
#define SOFT_SYNC_MAX_TUNNELS 16
#define SOFT_SYNC_MAX_DVCS 1024

/* Strict Soft-Sync Request parser (MS-RDPEDYC 2.2.5.1/2.2.5.1.1, S3): requires
 * TCP_FLUSHED, exact Length-vs-size match, and fully valid tunnel/channel lists.
 * All lists are validated even after finding UDPFECR (no early exit). */
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
	if (numTunnels > SOFT_SYNC_MAX_TUNNELS)
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
		if (numDvcs > SOFT_SYNC_MAX_DVCS)
			return FALSE;
		if (off + 6 + (size_t)numDvcs * 4 > len)
			return FALSE;
		if (tunnelType == SOFT_SYNC_TUNNEL_UDPFECR)
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

BOOL rdpeudp_soft_sync_request_offers_udp(const BYTE* pdu, size_t len)
{
	BOOL offers = FALSE;
	return soft_sync_request_parse(pdu, len, &offers, nullptr, 0, nullptr) && offers;
}

BOOL rdpeudp_soft_sync_request_udp_dvcs(const BYTE* pdu, size_t len, UINT32* outIds,
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

BOOL rdpeudp_soft_sync_response_offers_udp(const BYTE* pdu, size_t len)
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
	if (numTunnels > SOFT_SYNC_MAX_TUNNELS)
		return FALSE;
	if (len != 6 + (size_t)numTunnels * 4)
		return FALSE; /* exact match: no trailing bytes */
	for (UINT32 i = 0; i < numTunnels; i++)
	{
		const size_t o = 6 + (size_t)i * 4;
		const UINT32 tt = (UINT32)pdu[o] | ((UINT32)pdu[o + 1] << 8) |
		                  ((UINT32)pdu[o + 2] << 16) | ((UINT32)pdu[o + 3] << 24);
		if (tt == SOFT_SYNC_TUNNEL_UDPFECR)
			offers = TRUE;
	}
	return offers;
}

/* ------------------------------------------------------------------ */
/* ACK vector codec ([MS-RDPEUDP2] 2.2.1.2.6)                           */
/* ------------------------------------------------------------------ */

wStream* rdpeudp_build_ackvec(UINT16 baseSeq, const BOOL* received, size_t count, BOOL withTs,
                              UINT32 ts, BYTE sendGap)
{
	if (!received || (count == 0) || (count > 256))
		return nullptr;

	/* Bitmap mode always covers exactly 7 seqs; RLE covers exact run.
	 * Greedy: RLE for runs >=4, else full 7-bit bitmaps (pad last). */
	BYTE vec[64] = { 0 };
	size_t vlen = 0;
	size_t i = 0;
	while ((i < count) && (vlen < sizeof(vec)))
	{
		size_t run = 1;
		while (((i + run) < count) && (run < 63) && (received[i + run] == received[i]))
			run++;

		if (run >= 4)
		{
			const size_t r = (run > 63) ? 63 : run;
			vec[vlen++] = (BYTE)(0x80 | (received[i] ? 0x40 : 0x00) | (r & 0x3F));
			i += r;
		}
		else
		{
			BYTE b = 0;
			for (size_t n = 0; n < 7; n++)
			{
				if (((i + n) < count) && received[i + n])
					b |= (BYTE)(1u << n);
			}
			vec[vlen++] = b;
			i += 7;
			if (i > count)
				i = count;
		}
	}

	wStream* s = Stream_New(nullptr, vlen + 8);
	if (!s)
		return nullptr;
	Stream_Write_UINT16(s, baseSeq);
	Stream_Write_UINT8(s, (BYTE)((vlen & 0x7F) | (withTs ? 0x80 : 0x00)));
	if (withTs)
	{
		Stream_Write_UINT8(s, (BYTE)(ts & 0xFF));
		Stream_Write_UINT8(s, (BYTE)((ts >> 8) & 0xFF));
		Stream_Write_UINT8(s, (BYTE)((ts >> 16) & 0xFF));
		Stream_Write_UINT8(s, sendGap);
	}
	if (vlen > 0)
		Stream_Write(s, vec, vlen);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
	{
		Stream_Release(s);
		return nullptr;
	}
	return s;
}

BOOL rdpeudp_parse_ackvec(const BYTE* data, size_t len, UINT16* baseSeq, BOOL** received,
                          size_t* count)
{
	if (!data || (len < 3) || !baseSeq || !received || !count)
		return FALSE;
	const UINT16 base = (UINT16)(data[0] | ((UINT16)data[1] << 8));
	const BYTE b2 = data[2];
	const BYTE csize = (BYTE)(b2 & 0x7F);
	const BOOL haveTs = (b2 & 0x80) != 0;
	const size_t need = 3 + (haveTs ? 4 : 0) + csize;
	if (len < need)
		return FALSE;

	const BYTE* vec = data + 3 + (haveTs ? 4 : 0);
	/* First pass: compute total seq count */
	size_t total = 0;
	for (size_t vi = 0; vi < csize; vi++)
	{
		const BYTE b = vec[vi];
		if ((b & 0x80) == 0)
			total += 7;
		else
			total += (b & 0x3F);
	}
	if (total == 0 || total > 1024)
		return FALSE;

	BOOL* out = calloc(total, sizeof(BOOL));
	if (!out)
		return FALSE;
	size_t pos = 0;
	UINT16 seq = base;
	WINPR_UNUSED(seq);
	for (size_t vi = 0; vi < csize; vi++)
	{
		const BYTE b = vec[vi];
		if ((b & 0x80) == 0)
		{
			for (int bit = 0; bit < 7; bit++)
				out[pos++] = (b & (1u << bit)) ? TRUE : FALSE;
		}
		else
		{
			const BOOL st = (b & 0x40) ? TRUE : FALSE;
			const BYTE run = (BYTE)(b & 0x3F);
			for (BYTE k = 0; k < run; k++)
				out[pos++] = st;
		}
	}

	*baseSeq = base;
	*received = out;
	*count = total;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Tunnel channel framing (DVC/static channel data over RDP_TUNNEL_DATA) */
/* Payload type multiplexing (first byte):                              */
/*  0x00 = channel (channelId/totalSize/flags/chunk)                    */
/*  0x01 = autodetect request (sec_flags + autodetect PDU)              */
/* HigherLayerData carries channel PDUs; autodetect uses subheaders. */
/* ------------------------------------------------------------------ */

#define UDP_TUNNEL_PTYPE_CHANNEL 0x00 /* HigherLayerData multiplex tag (internal) */

wStream* rdpeudp_build_channel_packet(UINT16 channelId, UINT32 totalSize, UINT32 flags,
                                      const BYTE* chunk, size_t chunkLen)
{
	if (!chunk && (chunkLen != 0))
		return nullptr;
	if (chunkLen > UINT16_MAX)
		return nullptr;
	wStream* s = Stream_New(nullptr, chunkLen + 13);
	if (!s)
		return nullptr;
	Stream_Write_UINT8(s, UDP_TUNNEL_PTYPE_CHANNEL);
	Stream_Write_UINT16(s, channelId);
	Stream_Write_UINT32(s, totalSize);
	Stream_Write_UINT32(s, flags);
	Stream_Write_UINT16(s, (UINT16)chunkLen);
	if (chunkLen > 0)
		Stream_Write(s, chunk, chunkLen);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
	{
		Stream_Release(s);
		return nullptr;
	}
	return s;
}

BOOL rdpeudp_parse_channel_packet(const BYTE* data, size_t len, UINT16* channelId,
                                   UINT32* totalSize, UINT32* flags, const BYTE** chunk,
                                   size_t* chunkLen)
{
	if (!data || (len < 13) || (data[0] != UDP_TUNNEL_PTYPE_CHANNEL))
		return FALSE;
	const BYTE* p = data + 1;
	len -= 1;
	const UINT16 cid = (UINT16)(p[0] | ((UINT16)p[1] << 8));
	const UINT32 tot =
	    (UINT32)p[2] | ((UINT32)p[3] << 8) | ((UINT32)p[4] << 16) | ((UINT32)p[5] << 24);
	const UINT32 fl =
	    (UINT32)p[6] | ((UINT32)p[7] << 8) | ((UINT32)p[8] << 16) | ((UINT32)p[9] << 24);
	const UINT16 clen = (UINT16)(p[10] | ((UINT16)p[11] << 8));
	if (len < (size_t)12 + clen)
		return FALSE;
	if (channelId)
		*channelId = cid;
	if (totalSize)
		*totalSize = tot;
	if (flags)
		*flags = fl;
	if (chunk)
		*chunk = p + 12;
	if (chunkLen)
		*chunkLen = clen;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* reliable RDPEUDP2 transport                                          */
/* ------------------------------------------------------------------ */

#define RDPEUDP2_WINDOW_MAX 64
#define RDPEUDP2_RECV_MAP 256

typedef struct
{
	UINT16 dataSeq;
	UINT16 channelSeq;
	BYTE* data;
	size_t len;
	UINT64 sentTs;
	UINT32 retries;
} UdpSentEntry;

typedef struct
{
	BOOL occupied;
	UINT16 channelSeq;
	BYTE* data;
	size_t len;
} UdpRecvSlot;

struct rdp_udp_transport
{
	rdpContext* context;
	char* hostname;
	int port;

	int sockfd;
	BYTE securityCookie[RDPEUDP_COOKIE_LEN];
	BYTE cookieHash[RDPEUDP_COOKIE_HASHLEN];
	BYTE correlationId[RDPEUDP_CORRELATION_LEN];
	UINT32 requestId;
	UINT16 requestedProto;

	UINT32 initialSeq;
	UINT32 peerInitialSeq;
	UINT16 negotiatedVer;
	BOOL connected;
	BOOL useUdp2;

	/* v2 reliable state */
	UINT16 nextDataSeq;
	UINT16 nextChannelSeq;
	UINT16 expectedChannelSeq;
	UINT16 lastAckSent;
	UINT16 lastAckReceived;
	UINT16 lastAoaSent;
	BYTE logWindow;
	BYTE peerLogWindow;

	/* Negotiated MTU per [MS-RDPEUDP] 3.1.1.3 (default 1232 until SYN+ACK) */
	UINT16 negotiatedUpMtu;
	UINT16 negotiatedDownMtu;
	UINT16 maxPayload;

	/* Receiver DataSeq window for ACK/ACKVEC generation ([MS-RDPEUDP2] 3.1.1.2.2).
	 * recvDataBase = lowest DataSeq not yet cumulatively acked.
	 * recvDataSeen[i] = received(DataSeq = base+i). */
	UINT16 recvDataBase;
	BOOL recvDataSeen[RDPEUDP2_WINDOW_MAX * 2];
	BOOL needAoa;
	BYTE overhead;
	BYTE delayAckMax;
	UINT16 delayAckTimeoutMs;

	UdpSentEntry sent[RDPEUDP2_WINDOW_MAX];
	size_t sentCount;

	UdpRecvSlot recvMap[RDPEUDP2_RECV_MAP];
	wStream* recvStream; /* reassembled byte stream for TLS */
	size_t recvPos;      /* read cursor inside recvStream */
	/* Tunnel TLS byte reassembly (R3): preserves partial Tunnel DATA PDUs
	 * across non-blocking polls. Length = buffered bytes, Position always 0
	 * except transiently during append. */
	wStream* tunnelBuf;
	CRITICAL_SECTION lock;

	/* TLS over UDP */
	rdpTls* tls;
	BIO* tlsBio;
	BIO* udpBio; /* BIO_s_rdpeudp instance bound to this transport */

	UINT64 lastRecvTs;
	UINT64 lastSendTs;
	UINT64 lastKeepaliveTs;
	BOOL tunnelEstablished;
	/* Socket-readiness event for the main loop (WSA-associated once the
	 * socket exists; manual before that). Signaled on arrival, reset after
	 * the socket is drained (with recheck to avoid lost wakeups). */
	HANDLE udpEvent;
	HANDLE sockEvent;
	HANDLE abortEvent; /* non-owning cancel signal from owner */
	RdpUdpStats stats;
	wLog* log;
};

static BOOL udp_aborted(const rdpUdpTransport* udp)
{
	HANDLE ev = udp ? udp->abortEvent : nullptr;
	if (!ev || (ev == INVALID_HANDLE_VALUE))
		return FALSE;
	return WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
}

void rdpeudp_set_abort_event(rdpUdpTransport* udp, HANDLE abortEvent)
{
	if (!udp)
		return;
	EnterCriticalSection(&udp->lock);
	udp->abortEvent = abortEvent;
	LeaveCriticalSection(&udp->lock);
}

static UINT64 udp_now_ms(void)
{
	return GetTickCount64();
}

void rdpeudp_compute_mtu(UINT16 ourUp, UINT16 ourDown, UINT16 peerUp, UINT16 peerDown,
                                UINT16* negUp, UINT16* negDown)
{
	/* [MS-RDPEUDP] 3.1.1.3, conservative without ACK-vector bonus (safe). */
	UINT16 up = ourUp;
	if (peerDown < up)
		up = peerDown;
	if (up > RDPUDP2_MTU)
		up = RDPUDP2_MTU;
	if (up < 1132)
		up = 1132;
	UINT16 down = ourDown;
	if (peerUp < down)
		down = peerUp;
	if (down > RDPUDP2_MTU)
		down = RDPUDP2_MTU;
	if (down < 1132)
		down = 1132;
	if (negUp)
		*negUp = up;
	if (negDown)
		*negDown = down;
}

UINT16 rdpeudp_payload_for_mtu(UINT16 mtu)
{
	/* Worst-case v2 headers: prefix(1)+header(2)+ACK(7)+DATA hdr(4)+AOA(2)+
	 * OVERHEAD(1)+DELAYACK(3) ~= 20, plus TLS record overhead margin. */
	const int margin = 100;
	int p = (int)mtu - margin;
	if (p < 800)
		p = 800;
	if (p > RDPUDP2_MAX_PAYLOAD)
		p = RDPUDP2_MAX_PAYLOAD;
	return (UINT16)p;
}

static void udp_compute_cookie_hash(const BYTE* cookie, BYTE* out)
{
	WINPR_ASSERT(cookie);
	WINPR_ASSERT(out);
	/* cookieHash = SHA256(securityCookie), each 4-byte word in network order.
	 * SHA256 output bytes preserved; per-word BE write is a memcpy on the
	 * byte stream. */
	BYTE digest[SHA256_DIGEST_LENGTH] = { 0 };
	SHA256(cookie, RDPEUDP_COOKIE_LEN, digest);
	for (int i = 0; i < 8; i++)
	{
		out[i * 4 + 0] = digest[i * 4 + 0];
		out[i * 4 + 1] = digest[i * 4 + 1];
		out[i * 4 + 2] = digest[i * 4 + 2];
		out[i * 4 + 3] = digest[i * 4 + 3];
	}
}

rdpUdpTransport* rdpeudp_new(rdpContext* context, const char* hostname, int port,
                             UINT32 requestId, UINT16 requestedProto, const BYTE* securityCookie)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(hostname);
	WINPR_ASSERT(securityCookie);

	rdpUdpTransport* udp = calloc(1, sizeof(rdpUdpTransport));
	if (!udp)
		return nullptr;

	udp->log = WLog_Get(TAG);
	udp->context = context;
	udp->hostname = _strdup(hostname);
	if (!udp->hostname)
	{
		free(udp);
		return nullptr;
	}
	udp->port = (port > 0) ? port : 3389;
	udp->sockfd = -1;
	udp->requestId = requestId;
	udp->requestedProto = requestedProto;
	memcpy(udp->securityCookie, securityCookie, RDPEUDP_COOKIE_LEN);
	udp_compute_cookie_hash(securityCookie, udp->cookieHash);

	/* correlationId: reuse RDP negotiation correlation if available? For now
	 * generate random GUID-like value honoring MS-RDPEUDP constraints
	 * (first byte != 0x00/F4, no 0x0D bytes). */
	if (winpr_RAND(udp->correlationId, sizeof(udp->correlationId)) < 0)
	{
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	if ((udp->correlationId[0] == 0x00) || (udp->correlationId[0] == 0xF4))
		udp->correlationId[0] = 0x01;
	for (size_t i = 0; i < sizeof(udp->correlationId); i++)
	{
		if (udp->correlationId[i] == 0x0D)
			udp->correlationId[i] = 0x0E;
	}

	if (winpr_RAND(&udp->initialSeq, sizeof(udp->initialSeq)) < 0)
	{
		free(udp->hostname);
		free(udp);
		return nullptr;
	}

	udp->logWindow = RDPUDP2_DEFAULT_LOGWINDOW;
	udp->peerLogWindow = RDPUDP2_DEFAULT_LOGWINDOW;
	udp->negotiatedUpMtu = RDPUDP2_MTU;
	udp->negotiatedDownMtu = RDPUDP2_MTU;
	udp->maxPayload = RDPUDP2_MAX_PAYLOAD;
	udp->nextDataSeq = 0;
	udp->nextChannelSeq = 0;
	udp->expectedChannelSeq = 0;
	udp->recvDataBase = 0;
	udp->overhead = 50; /* avg RDPUDP2+UDP+IP overhead estimate */
	udp->delayAckMax = 2;
	udp->delayAckTimeoutMs = 50;
	udp->lastKeepaliveTs = udp_now_ms();
	udp->udpEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!udp->udpEvent || (udp->udpEvent == INVALID_HANDLE_VALUE))
	{
		udp->udpEvent = nullptr;
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	udp->recvStream = Stream_New(nullptr, 65536);
	if (!udp->recvStream)
	{
		(void)CloseHandle(udp->udpEvent);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	/* Stream_New sets length == capacity; the reassembly buffer starts empty. */
	if (!Stream_SetPosition(udp->recvStream, 0) || !Stream_SetLength(udp->recvStream, 0))
	{
		(void)CloseHandle(udp->udpEvent);
		Stream_Release(udp->recvStream);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	udp->tunnelBuf = Stream_New(nullptr, 131072);
	if (!udp->tunnelBuf)
	{
		(void)CloseHandle(udp->udpEvent);
		Stream_Release(udp->recvStream);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	if (!Stream_SetPosition(udp->tunnelBuf, 0) || !Stream_SetLength(udp->tunnelBuf, 0))
	{
		(void)CloseHandle(udp->udpEvent);
		Stream_Release(udp->recvStream);
		Stream_Release(udp->tunnelBuf);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}

	InitializeCriticalSection(&udp->lock);
	return udp;
}

void rdpeudp_free(rdpUdpTransport* udp)
{
	if (!udp)
		return;

	EnterCriticalSection(&udp->lock);
	if (udp->tls)
	{
		/* tls owns tlsBio chain; avoid double free of udpBio */
		udp->udpBio = nullptr;
		freerdp_tls_free(udp->tls);
		udp->tls = nullptr;
	}
	if (udp->sockfd >= 0)
	{
		freerdp_udp_close(udp->sockfd);
		udp->sockfd = -1;
	}
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
		free(udp->sent[i].data);
	for (size_t i = 0; i < ARRAYSIZE(udp->recvMap); i++)
		free(udp->recvMap[i].data);
	Stream_Release(udp->recvStream);
	udp->recvStream = nullptr;
	if (udp->tunnelBuf)
		Stream_Release(udp->tunnelBuf);
	udp->tunnelBuf = nullptr;
	if (udp->udpEvent && (udp->udpEvent != INVALID_HANDLE_VALUE))
		(void)CloseHandle(udp->udpEvent);
	udp->udpEvent = nullptr;
	if (udp->sockEvent && (udp->sockEvent != INVALID_HANDLE_VALUE))
		(void)WSACloseEvent(udp->sockEvent);
	udp->sockEvent = nullptr;
	udp->abortEvent = nullptr; /* non-owning */
	LeaveCriticalSection(&udp->lock);
	DeleteCriticalSection(&udp->lock);
	free(udp->hostname);
	free(udp);
}

BOOL rdpeudp_is_connected(const rdpUdpTransport* udp)
{
	return udp && udp->connected;
}

UINT16 rdpeudp_negotiated_version(const rdpUdpTransport* udp)
{
	return udp ? udp->negotiatedVer : 0;
}

UINT32 rdpeudp_get_request_id(const rdpUdpTransport* udp)
{
	return udp ? udp->requestId : 0;
}

/* ---- v2 packet building (little-endian) ---- */

BOOL rdpeudp2_parse_layout(const BYTE* layout, size_t len, RdpUdp2Layout* out)
{
	const BYTE* p = nullptr;
	size_t rem = 0;
	if (!layout || (len < 2) || !out)
		return FALSE;
	memset(out, 0, sizeof(*out));
	p = layout;
	rem = len;
	const UINT16 header = (UINT16)(p[0] | ((UINT16)p[1] << 8));
	out->flags = (UINT16)(header & 0x0FFF);
	out->logWindow = (UINT8)((header >> 12) & 0x0F);
	p += 2;
	rem -= 2;

	if (out->flags & RDPUDP2_FLAG_ACK)
	{
		if (rem < 7)
			return FALSE;
		out->hasAck = TRUE;
		out->ackBase = (UINT16)(p[0] | ((UINT16)p[1] << 8));
		out->ackTs[0] = p[2];
		out->ackTs[1] = p[3];
		out->ackTs[2] = p[4];
		out->ackGap = p[5];
		out->ackNumDelayed = (BYTE)(p[6] & 0x0F);
		out->ackScale = (BYTE)((p[6] >> 4) & 0x0F);
		const size_t ackLen = 7 + out->ackNumDelayed;
		if (rem < ackLen)
			return FALSE;
		out->ackDelayed = p + 7;
		out->ackDelayedLen = out->ackNumDelayed;
		p += ackLen;
		rem -= ackLen;
	}

	if (out->flags & RDPUDP2_FLAG_OVERHEAD)
	{
		if (rem < 1)
			return FALSE;
		out->hasOverhead = TRUE;
		out->overhead = p[0];
		p += 1;
		rem -= 1;
	}

	if (out->flags & RDPUDP2_FLAG_DELAYACK)
	{
		if (rem < 3)
			return FALSE;
		out->hasDelayAck = TRUE;
		out->delayMax = p[0];
		out->delayTimeout = (UINT16)(p[1] | ((UINT16)p[2] << 8));
		p += 3;
		rem -= 3;
	}

	if (out->flags & RDPUDP2_FLAG_AOA)
	{
		if (rem < 2)
			return FALSE;
		out->hasAoa = TRUE;
		out->aoa = (UINT16)(p[0] | ((UINT16)p[1] << 8));
		p += 2;
		rem -= 2;
	}

	if (out->flags & RDPUDP2_FLAG_DATA)
	{
		if (rem < 2)
			return FALSE;
		out->hasDataHeader = TRUE;
		out->dataSeq = (UINT16)(p[0] | ((UINT16)p[1] << 8));
		p += 2;
		rem -= 2;
	}

	if (out->flags & RDPUDP2_FLAG_ACKVEC)
	{
		if (rem < 3)
			return FALSE;
		const BYTE b2 = p[2];
		const BYTE csize = (BYTE)(b2 & 0x7F);
		const BOOL haveTs = (b2 & 0x80) != 0;
		const size_t need = 3 + (haveTs ? 4 : 0) + csize;
		if (rem < need)
			return FALSE;
		out->hasAckvec = TRUE;
		out->ackvec = p;
		out->ackvecLen = need;
		p += need;
		rem -= need;
	}

	if (out->flags & RDPUDP2_FLAG_DATA)
	{
		if (rem < 2)
			return FALSE;
		out->hasDataBody = TRUE;
		out->channelSeq = (UINT16)(p[0] | ((UINT16)p[1] << 8));
		p += 2;
		rem -= 2;
		out->dataBody = p;
		out->dataBodyLen = rem;
	}
	else if (out->hasAckvec && (rem != 0))
	{
		/* Trailing bytes with no DATA flag are malformed; be strict so
		 * fixtures detect layout errors instead of hiding them. */
		return FALSE;
	}

	return TRUE;
}

wStream* rdpeudp2_encode_layout(const RdpUdp2Layout* in)
{
	size_t need = 2;
	if (!in || (in->flags & ~RDPUDP2_FLAGS_MASK))
		return nullptr;
	if (!!(in->flags & RDPUDP2_FLAG_ACK) != in->hasAck)
		return nullptr;
	if (!!(in->flags & RDPUDP2_FLAG_OVERHEAD) != in->hasOverhead)
		return nullptr;
	if (!!(in->flags & RDPUDP2_FLAG_DELAYACK) != in->hasDelayAck)
		return nullptr;
	if (!!(in->flags & RDPUDP2_FLAG_AOA) != in->hasAoa)
		return nullptr;
	if (!!(in->flags & RDPUDP2_FLAG_DATA) != (in->hasDataHeader && in->hasDataBody))
		return nullptr;
	if (!!(in->flags & RDPUDP2_FLAG_ACKVEC) != in->hasAckvec)
		return nullptr;
	if ((in->flags & (RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_ACKVEC)) ==
	    (RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_ACKVEC))
		return nullptr;
	if (in->logWindow > 0x0F)
		return nullptr;

	if (in->hasAck)
		need += 7 + in->ackDelayedLen;
	if (in->hasOverhead)
		need += 1;
	if (in->hasDelayAck)
		need += 3;
	if (in->hasAoa)
		need += 2;
	if (in->hasDataHeader)
		need += 2;
	if (in->hasAckvec)
		need += in->ackvecLen;
	if (in->hasDataBody)
		need += 2 + in->dataBodyLen;

	wStream* s = Stream_New(nullptr, need);
	if (!s)
		return nullptr;
	Stream_Write_UINT16(s, (UINT16)(((in->logWindow & 0x0F) << 12) | (in->flags & 0x0FFF)));
	if (in->hasAck)
	{
		Stream_Write_UINT16(s, in->ackBase);
		Stream_Write(s, in->ackTs, 3);
		Stream_Write_UINT8(s, in->ackGap);
		Stream_Write_UINT8(s, (BYTE)(((in->ackScale & 0x0F) << 4) | (in->ackNumDelayed & 0x0F)));
		if (in->ackDelayedLen > 0)
			Stream_Write(s, in->ackDelayed, in->ackDelayedLen);
	}
	if (in->hasOverhead)
		Stream_Write_UINT8(s, in->overhead);
	if (in->hasDelayAck)
	{
		Stream_Write_UINT8(s, in->delayMax);
		Stream_Write_UINT16(s, in->delayTimeout);
	}
	if (in->hasAoa)
		Stream_Write_UINT16(s, in->aoa);
	if (in->hasDataHeader)
		Stream_Write_UINT16(s, in->dataSeq);
	if (in->hasAckvec)
		Stream_Write(s, in->ackvec, in->ackvecLen);
	if (in->hasDataBody)
	{
		Stream_Write_UINT16(s, in->channelSeq);
		if (in->dataBodyLen > 0)
			Stream_Write(s, in->dataBody, in->dataBodyLen);
	}
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
	{
		Stream_Release(s);
		return nullptr;
	}
	return s;
}

static wStream* rdpeudp2_build_packet_ex(rdpUdpTransport* udp, UINT16 flags, UINT16 ackBase,
                                         UINT16 ackvecBase, wStream* ackvecBody, UINT16 aoa,
                                         BYTE overhead, BYTE delayMax, UINT16 delayTimeout,
                                         UINT16 dataSeq, UINT16 channelSeq, const BYTE* payload,
                                         size_t payloadLen, BOOL dummy)
{
	WINPR_UNUSED(ackvecBase);
	RdpUdp2Layout in = WINPR_C_ARRAY_INIT;
	BYTE ackTs[3] = { 0 };
	in.flags = (UINT16)(flags & RDPUDP2_FLAGS_MASK);
	in.logWindow = (UINT8)(udp->logWindow & 0x0F);
	if (flags & RDPUDP2_FLAG_ACK)
	{
		const UINT64 now = udp_now_ms() * 250; /* 4us units */
		in.hasAck = TRUE;
		in.ackBase = ackBase;
		ackTs[0] = (BYTE)(now & 0xFF);
		ackTs[1] = (BYTE)((now >> 8) & 0xFF);
		ackTs[2] = (BYTE)((now >> 16) & 0xFF);
		in.ackTs[0] = ackTs[0];
		in.ackTs[1] = ackTs[1];
		in.ackTs[2] = ackTs[2];
		in.ackGap = 0;
		in.ackNumDelayed = 0;
		in.ackScale = 0;
	}
	if (flags & RDPUDP2_FLAG_OVERHEAD)
	{
		in.hasOverhead = TRUE;
		in.overhead = overhead;
	}
	if (flags & RDPUDP2_FLAG_DELAYACK)
	{
		in.hasDelayAck = TRUE;
		in.delayMax = delayMax;
		in.delayTimeout = delayTimeout;
	}
	if (flags & RDPUDP2_FLAG_AOA)
	{
		in.hasAoa = TRUE;
		in.aoa = aoa;
	}
	if (flags & RDPUDP2_FLAG_DATA)
	{
		in.hasDataHeader = TRUE;
		in.dataSeq = dataSeq;
		in.hasDataBody = TRUE;
		in.channelSeq = channelSeq;
		in.dataBody = payload;
		in.dataBodyLen = payloadLen;
	}
	if (flags & RDPUDP2_FLAG_ACKVEC)
	{
		if (!ackvecBody)
			return nullptr;
		in.hasAckvec = TRUE;
		in.ackvec = Stream_Buffer(ackvecBody);
		in.ackvecLen = Stream_Length(ackvecBody);
	}
	wStream* s = rdpeudp2_encode_layout(&in);
	if (!s)
		return nullptr;

	/* encode_layout returns sealed with cursor rewound; protect requires
	 * cursor at end (Position == Length) to append padding/prefix. */
	if (!Stream_SetPosition(s, Stream_Length(s)))
	{
		Stream_Release(s);
		return nullptr;
	}
	if (!rdpeudp2_protect(s, dummy))
	{
		Stream_Release(s);
		return nullptr;
	}
	udp->stats.sentPackets++;
	if (flags & RDPUDP2_FLAG_ACK)
		udp->stats.ackSent++;
	if (flags & RDPUDP2_FLAG_ACKVEC)
		udp->stats.ackvecSent++;
	return s;
}

static wStream* rdpeudp2_build_packet(rdpUdpTransport* udp, UINT16 flags, UINT16 ackBase,
                                      const BYTE* ackExtra, size_t ackExtraLen, UINT16 dataSeq,
                                      UINT16 channelSeq, const BYTE* payload, size_t payloadLen,
                                      BOOL dummy)
{
	WINPR_UNUSED(ackExtra);
	WINPR_UNUSED(ackExtraLen);
	UINT16 aoa = udp->lastAckReceived;
	BYTE overhead = udp->overhead;
	/* Opportunistically piggyback OVERHEAD+DELAYACK on first data packets */
	if ((flags & RDPUDP2_FLAG_DATA) && (udp->stats.sentPackets < 2))
		flags |= (UINT16)(RDPUDP2_FLAG_OVERHEAD | RDPUDP2_FLAG_DELAYACK);
	if (udp->needAoa)
		flags |= RDPUDP2_FLAG_AOA;
	return rdpeudp2_build_packet_ex(udp, flags, ackBase, 0, nullptr, aoa, overhead,
	                                udp->delayAckMax, udp->delayAckTimeoutMs, dataSeq,
	                                channelSeq, payload, payloadLen, dummy);
}

static BOOL rdpeudp_attach_socket_event(rdpUdpTransport* udp)
{
	if (!udp || (udp->sockfd < 0))
		return FALSE;
	if (!udp->sockEvent || (udp->sockEvent == INVALID_HANDLE_VALUE))
	{
		udp->sockEvent = WSACreateEvent();
		if (!udp->sockEvent || (udp->sockEvent == INVALID_HANDLE_VALUE))
		{
			udp->sockEvent = nullptr;
			return FALSE;
		}
	}
	/* Associate readability/close notifications; also makes the socket
	 * non-blocking on some platforms (harmless, we already set it). */
	if (WSAEventSelect((SOCKET)udp->sockfd, udp->sockEvent, FD_READ | FD_CLOSE) != 0)
	{
		WLog_WARN(TAG, "WSAEventSelect(UDP) failed 0x%08x", (unsigned)WSAGetLastError());
		return FALSE;
	}
	return TRUE;
}

/** Re-arm the readiness event after draining: reset, then recheck for
 * arrivals between the last recv (EAGAIN) and the reset to avoid lost
 * wakeups. Must be called with no lock held (it polls the socket). */
void rdpeudp_update_event(rdpUdpTransport* udp)
{
	if (!udp)
		return;
	HANDLE ev = nullptr;
	int fd = -1;
	EnterCriticalSection(&udp->lock);
	ev = udp->sockEvent;
	fd = udp->sockfd;
	const BOOL hasBuffered = (Stream_Length(udp->recvStream) > udp->recvPos);
	LeaveCriticalSection(&udp->lock);
	if (!ev || (ev == INVALID_HANDLE_VALUE) || (fd < 0))
		return;
	(void)WSAResetEvent(ev);
	/* Recheck: data arrived between drain and reset, or buffered TLS bytes. */
	BOOL readable = freerdp_udp_wait_readable(fd, 0);
	EnterCriticalSection(&udp->lock);
	const BOOL buffered = (Stream_Length(udp->recvStream) > udp->recvPos);
	LeaveCriticalSection(&udp->lock);
	if (readable || buffered || hasBuffered)
		(void)WSASetEvent(ev);
}

static BOOL rdpeudp_udp_send_stream(rdpUdpTransport* udp, wStream* s)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(s);
	const size_t len = Stream_Length(s);
	const BYTE* data = Stream_Buffer(s);
	if (len == 0)
		return FALSE;
	const SSIZE_T sent = freerdp_udp_send(udp->sockfd, data, len);
	if (sent != (SSIZE_T)len)
		return FALSE;
	udp->lastSendTs = udp_now_ms();
	return TRUE;
}

static void rdpeudp_ack_single_locked(rdpUdpTransport* udp, UINT16 dseq)
{
	/* Find ChannelSeq for this DataSeq, then free all attempts with same ChannelSeq
	 * (spec retransmits use new DataSeq, same ChannelSeq). */
	UINT16 cseq = 0;
	BOOL found = FALSE;
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		if (udp->sent[i].data && (udp->sent[i].dataSeq == dseq))
		{
			cseq = udp->sent[i].channelSeq;
			found = TRUE;
			break;
		}
	}
	if (!found)
	{
		/* Still try cumulative: free everything <= base (mod-aware, small window) */
		for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
		{
			if (udp->sent[i].data)
			{
				const INT16 diff = (INT16)(dseq - udp->sent[i].dataSeq);
				if ((diff >= 0) && (diff < (INT16)RDPEUDP2_WINDOW_MAX * 2))
				{
					free(udp->sent[i].data);
					udp->sent[i].data = nullptr;
					udp->sent[i].len = 0;
					if (udp->sentCount > 0)
						udp->sentCount--;
				}
			}
		}
		return;
	}
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		if (udp->sent[i].data && (udp->sent[i].channelSeq == cseq))
		{
			free(udp->sent[i].data);
			udp->sent[i].data = nullptr;
			udp->sent[i].len = 0;
			if (udp->sentCount > 0)
				udp->sentCount--;
		}
	}
}

static void rdpeudp_note_recv_data_seq_locked(rdpUdpTransport* udp, UINT16 dseq)
{
	const size_t WIN = ARRAYSIZE(udp->recvDataSeen);
	INT16 diff = (INT16)(dseq - udp->recvDataBase);
	if (diff < 0)
		return; /* duplicate/old, still acked via lastAckSent */
	if ((size_t)diff >= WIN)
	{
		/* Window slide: advance base to dseq-WIN+1, dropping old */
		const UINT16 newBase = (UINT16)(dseq - (WIN - 1));
		const INT16 adv = (INT16)(newBase - udp->recvDataBase);
		if (adv > 0)
		{
			const size_t a = ((size_t)adv < WIN) ? (size_t)adv : WIN;
			memmove(udp->recvDataSeen, udp->recvDataSeen + a, (WIN - a) * sizeof(BOOL));
			memset(udp->recvDataSeen + (WIN - a), 0, a * sizeof(BOOL));
			udp->recvDataBase = newBase;
			diff = (INT16)(dseq - udp->recvDataBase);
		}
		else
			return;
	}
	udp->recvDataSeen[diff] = TRUE;
	/* Advance base while contiguous */
	while (udp->recvDataSeen[0])
	{
		memmove(udp->recvDataSeen, udp->recvDataSeen + 1, (WIN - 1) * sizeof(BOOL));
		udp->recvDataSeen[WIN - 1] = FALSE;
		udp->recvDataBase++;
	}
	udp->lastAckSent = dseq;
	udp->stats.recvPackets++;
}

/* Returns TRUE if a datagram was received (even if ignored). */
static BOOL rdpeudp_recv_one(rdpUdpTransport* udp, DWORD timeoutMs, BOOL* haveV1SynAck,
                             RdpUdpFecHeader* v1hdr, RdpUdpSynPayload* v1syn,
                             RdpUdpSynExPayload* v1synex)
{
	BYTE buf[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
	const SSIZE_T r = freerdp_udp_recv(udp->sockfd, buf, sizeof(buf), timeoutMs);
	if (r <= 0)
		return FALSE;
	udp->lastRecvTs = udp_now_ms();

	/* Try v1 SYN+ACK first during handshake (starts with 0xFF..? snSourceAck) */
	if (haveV1SynAck && ((size_t)r >= 16))
	{
		RdpUdpFecHeader h = { 0 };
		RdpUdpSynPayload syn = { 0 };
		RdpUdpSynExPayload ex = { 0 };
		if (rdpeudp_parse_syn(buf, (size_t)r, &h, &syn, nullptr, &ex))
		{
			if ((h.flags & (RDPUDP_FLAG_SYN | RDPUDP_FLAG_ACK)) ==
			    (RDPUDP_FLAG_SYN | RDPUDP_FLAG_ACK))
			{
				*haveV1SynAck = TRUE;
				if (v1hdr)
					*v1hdr = h;
				if (v1syn)
					*v1syn = syn;
				if (v1synex)
					*v1synex = ex;
				return TRUE;
			}
		}
		*haveV1SynAck = FALSE;
	}

	/* Otherwise treat as v2 packet */
	{
		BOOL dummy = FALSE;
		size_t off = 0;
		BYTE tmp[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
		if ((size_t)r > sizeof(tmp))
			return TRUE; /* ignore oversize */
		memcpy(tmp, buf, (size_t)r);
		if (!rdpeudp2_unprotect(tmp, (size_t)r, &dummy, &off))
			return TRUE;
		if (dummy)
			return TRUE; /* ignore dummy for reliability */

		const BYTE* p = tmp + off;
		size_t rem = (size_t)r - off;
		if (rem < 2)
			return TRUE;
		const UINT16 header = (UINT16)(p[0] | ((UINT16)p[1] << 8));
		const UINT16 flags = (UINT16)(header & 0x0FFF);
		const BYTE peerLog = (BYTE)((header >> 12) & 0x0F);
		p += 2;
		rem -= 2;

		RdpUdp2Layout L = WINPR_C_ARRAY_INIT;
		if (!rdpeudp2_parse_layout(tmp + off, (size_t)r - off, &L))
			return TRUE;

		EnterCriticalSection(&udp->lock);
		udp->stats.recvPackets++;
		if ((L.logWindow <= RDPUDP2_MAX_LOGWINDOW) && (L.logWindow >= 2))
			udp->peerLogWindow = L.logWindow;

		if (L.hasAck)
		{
			udp->lastAckReceived = L.ackBase;
			udp->needAoa = TRUE;
			rdpeudp_ack_single_locked(udp, L.ackBase);
		}

		if (L.hasOverhead)
			udp->overhead = L.overhead;

		if (L.hasDelayAck)
		{
			udp->delayAckMax = L.delayMax;
			udp->delayAckTimeoutMs = L.delayTimeout;
		}

		if (L.hasAckvec)
		{
			UINT16 base = 0;
			BOOL* received = nullptr;
			size_t count = 0;
			/* Reuse the ACKVEC codec for selective free. */
			if (rdpeudp_parse_ackvec(L.ackvec, L.ackvecLen, &base, &received, &count))
			{
				udp->lastAckReceived = base;
				udp->needAoa = TRUE;
				for (size_t si = 0; si < count; si++)
				{
					if (received[si])
						rdpeudp_ack_single_locked(udp, (UINT16)(base + si));
					else if (count > 7)
						udp->stats.lostDetected += 0; /* counted per-run below */
				}
				/* Count loss runs for stats (RLE not expanded here in detail). */
				free(received);
			}
			else
			{
				/* Fall back to raw scan on parse failure (should not happen). */
				udp->needAoa = TRUE;
			}
		}

		/* DataBody (ChannelSeqNum + Data) follows ACKVEC per spec. */
		if (L.hasDataHeader && L.hasDataBody)
		{
			const UINT16 dseq = L.dataSeq;
			const UINT16 cseq = L.channelSeq;
			const BYTE* dp = L.dataBody;
			const size_t drem = L.dataBodyLen;

			rdpeudp_note_recv_data_seq_locked(udp, dseq);

			/* Deduplicate by channel seq */
			BOOL dup = FALSE;
			if (((INT16)(cseq - udp->expectedChannelSeq) < 0))
			{
				/* Already delivered (within window) -> dup, still ACK */
				dup = TRUE;
			}
			else
			{
				const size_t slot = (size_t)(cseq % RDPEUDP2_RECV_MAP);
				if (udp->recvMap[slot].occupied &&
				    (udp->recvMap[slot].channelSeq == cseq))
					dup = TRUE;
			}

			if (!dup && (drem <= 65535))
			{
				const size_t slot = (size_t)(cseq % RDPEUDP2_RECV_MAP);
				free(udp->recvMap[slot].data);
				udp->recvMap[slot].data = nullptr;
				udp->recvMap[slot].len = 0;
				udp->recvMap[slot].occupied = FALSE;
				if ((drem == 0) || ((udp->recvMap[slot].data = malloc(drem)) != nullptr))
				{
					if (drem > 0)
						memcpy(udp->recvMap[slot].data, dp, drem);
					udp->recvMap[slot].len = drem;
					udp->recvMap[slot].channelSeq = cseq;
					udp->recvMap[slot].occupied = TRUE;
				}

				/* Deliver in-order channel stream */
				while (TRUE)
				{
					const size_t eslot =
					    (size_t)(udp->expectedChannelSeq % RDPEUDP2_RECV_MAP);
					if (!udp->recvMap[eslot].occupied ||
					    (udp->recvMap[eslot].channelSeq != udp->expectedChannelSeq))
						break;
					if (!Stream_EnsureRemainingCapacity(
					        udp->recvStream, udp->recvMap[eslot].len))
						break;
					if (udp->recvMap[eslot].len > 0)
						Stream_Write(udp->recvStream, udp->recvMap[eslot].data,
						             udp->recvMap[eslot].len);
					free(udp->recvMap[eslot].data);
					udp->recvMap[eslot].data = nullptr;
					udp->recvMap[eslot].len = 0;
					udp->recvMap[eslot].occupied = FALSE;
					udp->expectedChannelSeq++;
				}
				/* Stream_Write advances position only; publish bytes via length. */
				Stream_SealLength(udp->recvStream);
				if (udp->sockEvent && (udp->sockEvent != INVALID_HANDLE_VALUE))
					(void)WSASetEvent(udp->sockEvent);
				if (udp->udpEvent && (udp->udpEvent != INVALID_HANDLE_VALUE))
					(void)SetEvent(udp->udpEvent);
			}
		}

		LeaveCriticalSection(&udp->lock);
		if (udp->sockEvent && (udp->sockEvent != INVALID_HANDLE_VALUE))
			(void)WSASetEvent(udp->sockEvent);
		if (udp->udpEvent && (udp->udpEvent != INVALID_HANDLE_VALUE))
			(void)SetEvent(udp->udpEvent);
	}

	return TRUE;
}

static BOOL rdpeudp2_send_ack(rdpUdpTransport* udp)
{
	UINT16 ackBase = 0;
	BOOL useAckvec = FALSE;
	wStream* ackvecBody = nullptr;
	UINT16 ackvecBase = 0;
	UINT16 aoa = 0;
	BOOL withAoa = FALSE;
	BYTE overhead = 50;
	BOOL withOverhead = FALSE;

	EnterCriticalSection(&udp->lock);
	/* Decide ACK vs ACKVEC based on DataSeq window gaps */
	{
		const size_t WIN = ARRAYSIZE(udp->recvDataSeen);
		BOOL anySeen = FALSE;
		for (size_t i = 0; i < WIN; i++)
		{
			if (udp->recvDataSeen[i])
			{
				anySeen = TRUE;
				break;
			}
		}
		if (anySeen)
		{
			/* Out-of-order: send ACKVEC with base + vector */
			useAckvec = TRUE;
			ackvecBase = udp->recvDataBase;
			/* Build BOOL array for vector (up to window) */
			size_t vcount = WIN;
			/* Trim trailing FALSE to reduce size */
			while ((vcount > 1) && !udp->recvDataSeen[vcount - 1])
				vcount--;
			BOOL* tmp = malloc(vcount * sizeof(BOOL));
			if (tmp)
			{
				for (size_t i = 0; i < vcount; i++)
					tmp[i] = udp->recvDataSeen[i];
				LeaveCriticalSection(&udp->lock);
				ackvecBody = rdpeudp_build_ackvec(ackvecBase, tmp, vcount, FALSE, 0, 0);
				free(tmp);
				EnterCriticalSection(&udp->lock);
				if (!ackvecBody)
					useAckvec = FALSE;
			}
			else
				useAckvec = FALSE;
		}
		if (!useAckvec)
		{
			/* Contiguous: cumulative ACK of base-1 (last received contiguous) */
			ackBase = (UINT16)(udp->recvDataBase - 1);
			/* Fallback to lastAckSent if nothing yet (base==0 and empty) */
			if ((udp->recvDataBase == 0) && !anySeen)
				ackBase = udp->lastAckSent;
		}
	}
	aoa = udp->lastAckReceived;
	withAoa = udp->needAoa;
	if (withAoa)
		udp->needAoa = FALSE;
	overhead = udp->overhead;
	/* Piggyback overhead on ACK-only packets occasionally */
	withOverhead = (udp->stats.ackSent % 10) == 0;
	LeaveCriticalSection(&udp->lock);

	wStream* s = nullptr;
	if (useAckvec && ackvecBody)
	{
		const size_t alen = Stream_Length(ackvecBody);
		/* Build packet with ACKVEC (no ACK flag, mutually exclusive) */
		UINT16 flags = (UINT16)(RDPUDP2_FLAG_ACKVEC | (withAoa ? RDPUDP2_FLAG_AOA : 0) |
		                        (withOverhead ? RDPUDP2_FLAG_OVERHEAD : 0));
		/* Reuse ex builder: ackvecBody already contains base+vector encoding */
		EnterCriticalSection(&udp->lock);
		const BYTE lw = udp->logWindow;
		LeaveCriticalSection(&udp->lock);
		s = Stream_New(nullptr, alen + 16);
		if (!s)
		{
			Stream_Release(ackvecBody);
			return FALSE;
		}
		Stream_Write_UINT16(s, (UINT16)(((lw & 0x0F) << 12) | (flags & 0x0FFF)));
		if (flags & RDPUDP2_FLAG_OVERHEAD)
			Stream_Write_UINT8(s, overhead);
		if (flags & RDPUDP2_FLAG_AOA)
			Stream_Write_UINT16(s, aoa);
		Stream_Write(s, Stream_Buffer(ackvecBody), alen);
		Stream_Release(ackvecBody);
		if (!rdpeudp2_protect(s, FALSE))
		{
			Stream_Release(s);
			return FALSE;
		}
	}
	else
	{
		if (ackvecBody)
			Stream_Release(ackvecBody);
		UINT16 flags = (UINT16)(RDPUDP2_FLAG_ACK | (withAoa ? RDPUDP2_FLAG_AOA : 0) |
		                        (withOverhead ? RDPUDP2_FLAG_OVERHEAD : 0));
		/* Temporarily set overhead for builder */
		s = rdpeudp2_build_packet(udp, flags, ackBase, nullptr, 0, 0, 0, nullptr, 0, FALSE);
		if (!s)
			return FALSE;
	}
	const BOOL rc = rdpeudp_udp_send_stream(udp, s);
	Stream_Release(s);
	if (rc)
	{
		EnterCriticalSection(&udp->lock);
		if (useAckvec)
			udp->stats.ackvecSent++;
		else
			udp->stats.ackSent++;
		LeaveCriticalSection(&udp->lock);
	}
	return rc;
}

BOOL rdpeudp_connect(rdpUdpTransport* udp, DWORD timeoutMs)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(udp->context);

	if (udp->requestedProto != INITIATE_REQUEST_PROTOCOL_UDPFECR)
	{
		WLog_WARN(TAG, "only reliable UDP (UDPFECR) supported, requested=0x%04" PRIx16,
		          udp->requestedProto);
		return FALSE;
	}

	if (freerdp_settings_get_bool(udp->context->settings, FreeRDP_GatewayEnabled))
	{
		WLog_WARN(TAG, "UDP multitransport via gateway not supported, declining");
		return FALSE;
	}

	udp->sockfd = freerdp_udp_connect(udp->context, udp->hostname, udp->port, timeoutMs);
	if (udp->sockfd < 0)
	{
		WLog_ERR(TAG, "UDP connect to %s:%d failed", udp->hostname, udp->port);
		return FALSE;
	}
	(void)rdpeudp_attach_socket_event(udp);

	const UINT64 deadline = udp_now_ms() + timeoutMs;
	RdpUdpFecHeader rhdr = { 0 };
	RdpUdpSynPayload rsyn = { 0 };
	RdpUdpSynExPayload rex = { 0 };

	for (int attempt = 0; attempt < RDPEUDP_SYN_MAX_RETRIES; attempt++)
	{
		if (udp_aborted(udp))
			return FALSE;
		if (udp->context && udp->context->rdp &&
		    freerdp_shall_disconnect_context(udp->context))
			return FALSE;
		wStream* syn = rdpeudp_build_syn(udp->initialSeq, RDPUDP2_MTU, RDPUDP2_MTU, 0, FALSE,
		                                 udp->correlationId, RDPUDP_PROTOCOL_VERSION_3,
		                                 udp->cookieHash);
		if (!syn)
			return FALSE;
		if (!rdpeudp_udp_send_stream(udp, syn))
		{
			Stream_Release(syn);
			return FALSE;
		}
		Stream_Release(syn);
		WLog_DBG(TAG, "UDP SYN attempt %d sent (seq=0x%08" PRIx32 ")", attempt + 1,
		         udp->initialSeq);

		const UINT64 now = udp_now_ms();
		if (now >= deadline)
			break;
		DWORD wait = (DWORD)(deadline - now);
		if (wait > RDPEUDP_SYN_TIMEOUT_MS)
			wait = RDPEUDP_SYN_TIMEOUT_MS;

		BOOL gotSynAck = FALSE;
		memset(&rhdr, 0, sizeof(rhdr));
		memset(&rsyn, 0, sizeof(rsyn));
		memset(&rex, 0, sizeof(rex));

		/* Poll until timeout or SYN+ACK */
		const UINT64 attemptDeadline = udp_now_ms() + wait;
		while (udp_now_ms() < attemptDeadline)
		{
			if (udp_aborted(udp))
				return FALSE;
			DWORD step = (DWORD)(attemptDeadline - udp_now_ms());
			if (step > 100)
				step = 100;
			BOOL flag = FALSE;
			if (!rdpeudp_recv_one(udp, step, &flag, &rhdr, &rsyn, &rex))
				continue;
			if (flag)
			{
				gotSynAck = TRUE;
				break;
			}
			/* v2 packet received before handshake? ignore */
		}

		if (!gotSynAck)
		{
			WLog_DBG(TAG, "UDP SYN attempt %d timeout, retrying", attempt + 1);
			continue;
		}

		/* Validate SYN+ACK */
		if (rhdr.snSourceAck != udp->initialSeq)
		{
			WLog_WARN(TAG, "SYN+ACK ack mismatch: got 0x%08" PRIx32 " want 0x%08" PRIx32,
			          rhdr.snSourceAck, udp->initialSeq);
			continue;
		}

		udp->peerInitialSeq = rsyn.initialSeq;
		if ((rhdr.flags & RDPUDP_FLAG_SYNEX) && (rex.synexFlags & RDPUDP_SYNEX_VERSION_VALID))
			udp->negotiatedVer = rex.version;
		else
			udp->negotiatedVer = RDPUDP_PROTOCOL_VERSION_1;

		WLog_DBG(TAG, "UDP SYN+ACK: peerSeq=0x%08" PRIx32 " version=0x%04" PRIx16, rsyn.initialSeq,
		         udp->negotiatedVer);

		if (udp->negotiatedVer != RDPUDP_PROTOCOL_VERSION_3)
		{
			WLog_WARN(TAG, "server negotiated RDPUDP v%" PRIu16 ", need v3 (RDPEUDP2)",
			          udp->negotiatedVer);
			return FALSE;
		}

		/* MTU negotiation per [MS-RDPEUDP] 3.1.1.3 */
		{
			UINT16 negUp = 0;
			UINT16 negDown = 0;
			rdpeudp_compute_mtu(RDPUDP2_MTU, RDPUDP2_MTU, rsyn.upstreamMtu,
			                    rsyn.downstreamMtu, &negUp, &negDown);
			udp->negotiatedUpMtu = negUp;
			udp->negotiatedDownMtu = negDown;
			udp->maxPayload = rdpeudp_payload_for_mtu(negUp);
			WLog_DBG(TAG, "UDP MTU negotiated up=%u down=%u payload=%u (peer up=%u down=%u)",
			         negUp, negDown, udp->maxPayload, rsyn.upstreamMtu, rsyn.downstreamMtu);
		}

		/* Send final ACK of handshake (v1 ACK, no SYN) */
		wStream* ack = Stream_New(nullptr, 32);
		if (!ack)
			return FALSE;
		RdpUdpFecHeader ah = { 0 };
		ah.snSourceAck = udp->peerInitialSeq;
		ah.receiveWindow = 128;
		ah.flags = RDPUDP_FLAG_ACK;
		if (!rdpeudp_write_fec_header(ack, &ah))
		{
			Stream_Release(ack);
			return FALSE;
		}
		/* Minimal ACK vector header: size 0 + padding */
		Stream_Write_UINT16_BE(ack, 0);
		Stream_Write_UINT16_BE(ack, 0);
		Stream_SealLength(ack);
		if (!Stream_SetPosition(ack, 0))
		{
			Stream_Release(ack);
			return FALSE;
		}
		const BOOL sent = rdpeudp_udp_send_stream(udp, ack);
		Stream_Release(ack);
		if (!sent)
			return FALSE;

		udp->connected = TRUE;
		udp->useUdp2 = TRUE;
		udp->lastRecvTs = udp_now_ms();
		udp->lastSendTs = udp->lastRecvTs;
		WLog_INFO(TAG, "RDP-UDP connected to %s:%d (RDPEUDP2)", udp->hostname, udp->port);
		return TRUE;
	}

	WLog_ERR(TAG, "RDP-UDP handshake to %s:%d failed after %d attempts", udp->hostname,
	         udp->port, RDPEUDP_SYN_MAX_RETRIES);
	return FALSE;
}

/* ---- reliable DATA send/recv over v2 ---- */

static BOOL rdpeudp2_wait_acked(rdpUdpTransport* udp, UINT16 channelSeq, DWORD timeoutMs)
{
	const UINT64 deadline = udp_now_ms() + timeoutMs;
	while (udp_now_ms() < deadline)
	{
		if (udp_aborted(udp))
			return FALSE;
		BOOL empty = TRUE;
		EnterCriticalSection(&udp->lock);
		for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
		{
			if (udp->sent[i].data && (udp->sent[i].channelSeq == channelSeq))
			{
				empty = FALSE;
				break;
			}
		}
		LeaveCriticalSection(&udp->lock);
		if (empty)
			return TRUE;

		DWORD step = (DWORD)(deadline - udp_now_ms());
		if (step > 50)
			step = 50;
		(void)rdpeudp_recv_one(udp, step, nullptr, nullptr, nullptr, nullptr);

		/* Retransmit check: spec requires NEW DataSeq, SAME ChannelSeq */
		EnterCriticalSection(&udp->lock);
		for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
		{
			if (udp->sent[i].data && (udp->sent[i].channelSeq == channelSeq))
			{
				const UINT64 now = udp_now_ms();
				/* Exponential backoff: 200ms * (retries+1), capped */
				const UINT64 rto = RDPEUDP_ACK_TIMEOUT_MS * (udp->sent[i].retries + 1);
				const UINT64 capped = (rto > 1000) ? 1000 : rto;
				if ((now - udp->sent[i].sentTs) >= capped)
				{
					/* Find free slot for retransmit with new DataSeq */
					size_t freeSlot = ARRAYSIZE(udp->sent);
					for (size_t j = 0; j < ARRAYSIZE(udp->sent); j++)
					{
						if (!udp->sent[j].data)
						{
							freeSlot = j;
							break;
						}
					}
					if (freeSlot >= ARRAYSIZE(udp->sent))
					{
						LeaveCriticalSection(&udp->lock);
						return FALSE;
					}
					const UINT16 newDseq = udp->nextDataSeq++;
					BYTE* copy = malloc(udp->sent[i].len);
					if (!copy)
					{
						LeaveCriticalSection(&udp->lock);
						return FALSE;
					}
					memcpy(copy, udp->sent[i].data, udp->sent[i].len);
					udp->sent[freeSlot].data = copy;
					udp->sent[freeSlot].len = udp->sent[i].len;
					udp->sent[freeSlot].dataSeq = newDseq;
					udp->sent[freeSlot].channelSeq = channelSeq;
					udp->sent[freeSlot].sentTs = now;
					udp->sent[freeSlot].retries = udp->sent[i].retries + 1;
					udp->sentCount++;
					udp->stats.retransmits++;
					const UINT16 cseq = channelSeq;
					const size_t clen = udp->sent[i].len;
					BYTE* cdata = udp->sent[i].data;
					/* Copy for send outside lock */
					BYTE tmp[RDPUDP2_MAX_PAYLOAD + 1] = { 0 };
					if (clen > sizeof(tmp))
					{
						LeaveCriticalSection(&udp->lock);
						return FALSE;
					}
					memcpy(tmp, cdata, clen);
					const UINT16 ackBase = udp->lastAckSent;
					LeaveCriticalSection(&udp->lock);

					wStream* rs = rdpeudp2_build_packet(
					    udp, (UINT16)(RDPUDP2_FLAG_DATA | RDPUDP2_FLAG_ACK), ackBase,
					    nullptr, 0, newDseq, cseq, tmp, clen, FALSE);
					if (rs)
					{
						(void)rdpeudp_udp_send_stream(udp, rs);
						Stream_Release(rs);
					}
					EnterCriticalSection(&udp->lock);
					/* Update original's timestamp to avoid tight loop;
					 * keep retry count on new slot, mark old as retried */
					udp->sent[i].sentTs = now;
					if (udp->sent[freeSlot].retries > RDPEUDP_MAX_RETRIES)
					{
						LeaveCriticalSection(&udp->lock);
						return FALSE;
					}
					LeaveCriticalSection(&udp->lock);
					/* Opportunistic keepalive check */
					(void)rdpeudp_check_keepalive(udp, 5000);
					EnterCriticalSection(&udp->lock);
				}
				break;
			}
		}
		LeaveCriticalSection(&udp->lock);
	}
	return FALSE;
}

static SSIZE_T rdpeudp2_send_reliable(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	WINPR_ASSERT(udp);
	if (!udp->connected || !udp->useUdp2)
		return -1;
	if (!data || (len == 0) || (len > 60000))
		return -1;

	size_t off = 0;
	while (off < len)
	{
		if (udp_aborted(udp))
			return -1;
		/* Flow control: effective window = min(ours, peer's) */
		while (TRUE)
		{
			size_t used = 0;
			BYTE effWindow = RDPUDP2_DEFAULT_LOGWINDOW;
			EnterCriticalSection(&udp->lock);
			used = udp->sentCount;
			effWindow = (udp->logWindow < udp->peerLogWindow) ? udp->logWindow
			                                                  : udp->peerLogWindow;
			if (effWindow > RDPUDP2_MAX_LOGWINDOW)
				effWindow = RDPUDP2_MAX_LOGWINDOW;
			LeaveCriticalSection(&udp->lock);
			if (used < (size_t)(1u << effWindow))
				break;
			(void)rdpeudp_recv_one(udp, 20, nullptr, nullptr, nullptr, nullptr);
			/* Abort promptly if RDP disconnecting */
			if (udp_aborted(udp))
				return -1;
			if (udp->context && udp->context->rdp &&
			    freerdp_shall_disconnect_context(udp->context))
				return -1;
		}

		UINT16 curPayload = RDPUDP2_MAX_PAYLOAD;
		EnterCriticalSection(&udp->lock);
		curPayload = udp->maxPayload;
		LeaveCriticalSection(&udp->lock);
		const size_t chunk = (len - off > curPayload) ? curPayload : (len - off);

		EnterCriticalSection(&udp->lock);
		const UINT16 dseq = udp->nextDataSeq++;
		const UINT16 cseq = udp->nextChannelSeq++;
		/* reserve slot */
		size_t slot = ARRAYSIZE(udp->sent);
		for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
		{
			if (!udp->sent[i].data)
			{
				slot = i;
				break;
			}
		}
		if (slot >= ARRAYSIZE(udp->sent))
		{
			LeaveCriticalSection(&udp->lock);
			return -1;
		}
		udp->sent[slot].data = malloc(chunk);
		if (!udp->sent[slot].data)
		{
			LeaveCriticalSection(&udp->lock);
			return -1;
		}
		memcpy(udp->sent[slot].data, data + off, chunk);
		udp->sent[slot].len = chunk;
		udp->sent[slot].dataSeq = dseq;
		udp->sent[slot].channelSeq = cseq;
		udp->sent[slot].sentTs = udp_now_ms();
		udp->sent[slot].retries = 0;
		udp->sentCount++;
		const UINT16 ackBase = udp->lastAckSent;
		LeaveCriticalSection(&udp->lock);

		wStream* pkt = rdpeudp2_build_packet(udp, RDPUDP2_FLAG_DATA | RDPUDP2_FLAG_ACK,
		                                     ackBase, nullptr, 0, dseq, cseq, data + off,
		                                     chunk, FALSE);
		if (!pkt)
			return -1;
		if (!rdpeudp_udp_send_stream(udp, pkt))
		{
			Stream_Release(pkt);
			return -1;
		}
		Stream_Release(pkt);

		if (!rdpeudp2_wait_acked(udp, cseq, 2000))
		{
			WLog_WARN(TAG, "RDPEUDP2 send timeout cseq=0x%04" PRIx16 " dseq=0x%04" PRIx16,
			          cseq, dseq);
			return -1;
		}

		off += chunk;
	}

	return (SSIZE_T)len;
}

static SSIZE_T rdpeudp2_recv_reliable(rdpUdpTransport* udp, BYTE* buffer, size_t len,
                                      DWORD timeoutMs)
{
	WINPR_ASSERT(udp);
	if (!udp->connected)
		return -1;

	const UINT64 deadline = udp_now_ms() + timeoutMs;
	while (udp_now_ms() < deadline)
	{
		if (udp_aborted(udp))
			return -1;
		EnterCriticalSection(&udp->lock);
		const size_t avail = Stream_Length(udp->recvStream) - udp->recvPos;
		if (avail > 0)
		{
			const size_t copy = (avail < len) ? avail : len;
			memcpy(buffer, Stream_Buffer(udp->recvStream) + udp->recvPos, copy);
			udp->recvPos += copy;
			if (udp->recvPos >= Stream_Length(udp->recvStream))
			{
				if (!Stream_SetPosition(udp->recvStream, 0) ||
				    !Stream_SetLength(udp->recvStream, 0))
				{
					LeaveCriticalSection(&udp->lock);
					return -1;
				}
				udp->recvPos = 0;
			}
			LeaveCriticalSection(&udp->lock);
			/* ACK what we consumed (also serves as keepalive) */
			(void)rdpeudp2_send_ack(udp);
			return (SSIZE_T)copy;
		}
		LeaveCriticalSection(&udp->lock);

		DWORD step = (DWORD)(deadline - udp_now_ms());
		if (step > 50)
			step = 50;
		BOOL dummyFlag = FALSE;
		(void)rdpeudp_recv_one(udp, step, nullptr, nullptr, nullptr, nullptr);
		WINPR_UNUSED(dummyFlag);

		/* Send periodic ACKs so sender window progresses even without data */
		EnterCriticalSection(&udp->lock);
		const BOOL needAck = (Stream_Length(udp->recvStream) > udp->recvPos);
		LeaveCriticalSection(&udp->lock);
		if (needAck)
			(void)rdpeudp2_send_ack(udp);
	}

	errno = EAGAIN;
	return 0;
}

/* ------------------------------------------------------------------ */
/* BIO over reliable UDP (for TLS)                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
	rdpUdpTransport* udp;
	HANDLE hEvent;
} RdpUdpBio;

static int rdpeudp_bio_write(BIO* bio, const char* buf, int size)
{
	RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(bio);
	if (!ptr || !ptr->udp || !buf || (size <= 0))
		return 0;
	BIO_clear_flags(bio, BIO_FLAGS_WRITE);
	const SSIZE_T rc =
	    rdpeudp2_send_reliable(ptr->udp, (const BYTE*)buf, (size_t)size);
	if (rc <= 0)
	{
		BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
		return -1;
	}
	return (int)rc;
}

static int rdpeudp_bio_read(BIO* bio, char* buf, int size)
{
	RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(bio);
	if (!ptr || !ptr->udp || !buf || (size <= 0))
		return 0;
	BIO_clear_flags(bio, BIO_FLAGS_READ);
	const SSIZE_T rc =
	    rdpeudp2_recv_reliable(ptr->udp, (BYTE*)buf, (size_t)size, 100);
	if (rc < 0)
	{
		BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
		return -1;
	}
	if (rc == 0)
	{
		BIO_set_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_SHOULD_RETRY);
		return -1;
	}
	return (int)rc;
}

static int rdpeudp_bio_puts(BIO* bio, const char* str)
{
	WINPR_UNUSED(bio);
	WINPR_UNUSED(str);
	return -2;
}

static int rdpeudp_bio_gets(BIO* bio, char* str, int size)
{
	WINPR_UNUSED(bio);
	WINPR_UNUSED(str);
	WINPR_UNUSED(size);
	return 1;
}

static long rdpeudp_bio_ctrl(BIO* bio, int cmd, long arg1, void* arg2)
{
	RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(bio);
	switch (cmd)
	{
		case BIO_CTRL_FLUSH:
			return 1;
		case BIO_C_GET_EVENT:
			if (!ptr || !arg2)
				return 0;
			*((HANDLE*)arg2) = ptr->hEvent;
			return 1;
		case BIO_C_SET_NONBLOCK:
			return 1;
		case BIO_C_WAIT_READ:
		{
			if (!ptr || !ptr->udp)
				return -1;
			return freerdp_udp_wait_readable(ptr->udp->sockfd, (DWORD)arg1) ? 1 : -1;
		}
		case BIO_C_WAIT_WRITE:
		{
			if (!ptr || !ptr->udp)
				return -1;
			return freerdp_udp_wait_writable(ptr->udp->sockfd, (DWORD)arg1) ? 1 : -1;
		}
		case BIO_CTRL_GET_CLOSE:
			return BIO_get_shutdown(bio);
		case BIO_CTRL_SET_CLOSE:
			BIO_set_shutdown(bio, (int)arg1);
			return 1;
		case BIO_C_READ_BLOCKED:
		case BIO_C_WRITE_BLOCKED:
			return 0;
		default:
			return 0;
	}
}

static int rdpeudp_bio_new(BIO* bio)
{
	RdpUdpBio* ptr = calloc(1, sizeof(RdpUdpBio));
	if (!ptr)
		return 0;
	ptr->hEvent = WSACreateEvent();
	if (!ptr->hEvent)
	{
		free(ptr);
		return 0;
	}
	BIO_set_data(bio, ptr);
	BIO_set_init(bio, 1);
	BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY);
	return 1;
}

static int rdpeudp_bio_free(BIO* bio)
{
	if (!bio)
		return 0;
	RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(bio);
	if (ptr)
	{
		if (ptr->hEvent)
			(void)CloseHandle(ptr->hEvent);
		BIO_set_data(bio, nullptr);
		free(ptr);
	}
	BIO_set_init(bio, 0);
	BIO_set_flags(bio, 0);
	return 1;
}

BIO_METHOD* BIO_s_rdpeudp(void)
{
	static BIO_METHOD* meth = nullptr;
	if (!meth)
	{
		meth = BIO_meth_new(BIO_TYPE_DGRAM + 7, "RdpUdp");
		if (!meth)
			return nullptr;
		BIO_meth_set_write(meth, rdpeudp_bio_write);
		BIO_meth_set_read(meth, rdpeudp_bio_read);
		BIO_meth_set_puts(meth, rdpeudp_bio_puts);
		BIO_meth_set_gets(meth, rdpeudp_bio_gets);
		BIO_meth_set_ctrl(meth, rdpeudp_bio_ctrl);
		BIO_meth_set_create(meth, rdpeudp_bio_new);
		BIO_meth_set_destroy(meth, rdpeudp_bio_free);
	}
	return meth;
}

/* ---- TLS + tunnel ---- */

BOOL rdpeudp_tls_connect(rdpUdpTransport* udp)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(udp->context);
	if (!udp->connected)
		return FALSE;

	rdpContext* context = udp->context;
	rdpSettings* settings = context->settings;
	WINPR_ASSERT(settings);

	rdpTls* tls = freerdp_tls_new(context);
	if (!tls)
		return FALSE;

	BIO* ubio = BIO_new(BIO_s_rdpeudp());
	if (!ubio)
	{
		freerdp_tls_free(tls);
		return FALSE;
	}
	RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(ubio);
	ptr->udp = udp;

	tls->hostname = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
	tls->serverName = freerdp_settings_get_string(settings, FreeRDP_UserSpecifiedServerName);
	tls->port =
	    WINPR_ASSERTING_INT_CAST(int32_t, MIN(UINT16_MAX,
	                                         freerdp_settings_get_uint32(settings,
	                                                                     FreeRDP_ServerPort)));
	if (tls->port == 0)
		tls->port = 3389;
	tls->isGatewayTransport = FALSE;

	const int status = freerdp_tls_connect(tls, ubio);
	if (status < 1)
	{
		WLog_ERR(TAG, "TLS over RDP-UDP failed (%d)", status);
		/* ubio owned by tls on success only; on failure free explicitly
		 * if tls did not take ownership. freerdp_tls_free handles bio. */
		freerdp_tls_free(tls);
		return FALSE;
	}

	udp->tls = tls;
	udp->tlsBio = tls->bio;
	udp->udpBio = ubio;
	WLog_INFO(TAG, "TLS over RDP-UDP established to %s:%d", udp->hostname, udp->port);
	return TRUE;
}

static SSIZE_T tls_send_all(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(udp->tls);
	size_t off = 0;
	while (off < len)
	{
		if (udp_aborted(udp))
			return -1;
		ERR_clear_error();
		const size_t chunk = len - off;
		const int w = (chunk > (size_t)INT_MAX) ? INT_MAX : (int)chunk;
		const int rc = BIO_write(udp->tlsBio, data + off, w);
		if (rc <= 0)
		{
			if (!BIO_should_retry(udp->tlsBio))
				return -1;
			if (BIO_wait_write(udp->tlsBio, 100) < 0)
				return -1;
			continue;
		}
		off += (size_t)rc;
	}
	(void)BIO_flush(udp->tlsBio);
	return (SSIZE_T)len;
}

static SSIZE_T tls_recv_all(rdpUdpTransport* udp, BYTE* buffer, size_t len, DWORD timeoutMs)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(udp->tls);
	size_t off = 0;
	const UINT64 deadline = udp_now_ms() + timeoutMs;
	while (off < len)
	{
		if (udp_aborted(udp))
			return -1;
		ERR_clear_error();
		const size_t chunk = len - off;
		const int r = (chunk > (size_t)INT_MAX) ? INT_MAX : (int)chunk;
		const int rc = BIO_read(udp->tlsBio, buffer + off, r);
		if (rc > 0)
		{
			off += (size_t)rc;
			continue;
		}
		if (!BIO_should_retry(udp->tlsBio))
			return -1;
		if (udp_now_ms() >= deadline)
		{
			errno = EAGAIN;
			return (off == 0) ? 0 : (SSIZE_T)off;
		}
		if (BIO_wait_read(udp->tlsBio, 50) < 0)
		{
			/* keep polling until deadline */
		}
	}
	return (SSIZE_T)off;
}

BOOL rdpeudp_tunnel_create(rdpUdpTransport* udp, DWORD timeoutMs)
{
	WINPR_ASSERT(udp);
	if (!udp->tls)
		return FALSE;

	wStream* req = rdpemt_build_create_request(udp->requestId, udp->securityCookie);
	if (!req)
		return FALSE;
	const size_t reqlen = Stream_Length(req);
	BYTE* reqbuf = Stream_Buffer(req);
	const SSIZE_T sent = tls_send_all(udp, reqbuf, reqlen);
	Stream_Release(req);
	if (sent != (SSIZE_T)reqlen)
	{
		WLog_ERR(TAG, "Tunnel Create Request send failed");
		return FALSE;
	}

	/* Read response header (4 bytes) then payload */
	BYTE hdr[4] = { 0 };
	if (tls_recv_all(udp, hdr, sizeof(hdr), timeoutMs) != (SSIZE_T)sizeof(hdr))
	{
		WLog_ERR(TAG, "Tunnel Create Response header recv failed");
		return FALSE;
	}

	BYTE action = 0;
	UINT16 plen = 0;
	UINT8 hlen = 0;
	BYTE full[4 + 4] = { 0 };
	memcpy(full, hdr, 4);
	if (!rdpemt_decode_header(hdr, sizeof(hdr), &action, &plen, &hlen))
	{
		WLog_ERR(TAG, "Tunnel Create Response bad header");
		return FALSE;
	}
	if ((action != RDPTUNNEL_ACTION_CREATERESPONSE) || (plen != 4) || (hlen != 4))
	{
		WLog_ERR(TAG, "Tunnel Create Response unexpected action=%u plen=%u hlen=%u", action,
		         plen, hlen);
		return FALSE;
	}

	BYTE hrbuf[4] = { 0 };
	if (tls_recv_all(udp, hrbuf, sizeof(hrbuf), timeoutMs) != (SSIZE_T)sizeof(hrbuf))
	{
		WLog_ERR(TAG, "Tunnel Create Response payload recv failed");
		return FALSE;
	}
	memcpy(full + 4, hrbuf, 4);

	UINT32 hr = 0;
	if (!rdpemt_parse_create_response(full, sizeof(full), &hr))
	{
		WLog_ERR(TAG, "Tunnel Create Response parse failed");
		return FALSE;
	}
	if (hr != 0)
	{
		WLog_ERR(TAG, "Tunnel Create rejected hr=0x%08" PRIx32, hr);
		return FALSE;
	}

	udp->tunnelEstablished = TRUE;
	WLog_INFO(TAG, "RDP multitransport tunnel %u established over UDP", udp->requestId);
	return TRUE;
}

SSIZE_T rdpeudp_tunnel_send(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	if (!udp || !udp->tunnelEstablished || !data)
		return -1;
	wStream* s = rdpemt_build_data(data, len);
	if (!s)
		return -1;
	const size_t total = Stream_Length(s);
	const SSIZE_T rc = tls_send_all(udp, Stream_Buffer(s), total);
	Stream_Release(s);
	return (rc == (SSIZE_T)total) ? (SSIZE_T)len : -1;
}

SSIZE_T rdpeudp_tunnel_recv(rdpUdpTransport* udp, BYTE* buffer, size_t len, DWORD timeoutMs)
{
	BYTE sub[256] = { 0 };
	size_t subLen = 0;
	size_t payloadLen = 0;
	/* Legacy helper: returns HigherLayerData only; fails if subheaders present
	 * so callers cannot silently miss autodetect subheaders (P1-7). */
	BYTE tmpPayload[65535] = { 0 };
	const int rc =
	    rdpeudp_tunnel_recv_full(udp, sub, sizeof(sub), &subLen, tmpPayload,
	                             (len < sizeof(tmpPayload) ? len : sizeof(tmpPayload)),
	                             &payloadLen, timeoutMs);
	if (rc <= 0)
		return rc;
	if (subLen != 0)
		return -1; /* subheaders must be handled via recv_full */
	if (payloadLen > len)
		return -1;
	memcpy(buffer, tmpPayload, payloadLen);
	return (SSIZE_T)payloadLen;
}

SSIZE_T rdpeudp_tunnel_send_full(rdpUdpTransport* udp, const BYTE* subheaders,
                                 size_t subheadersLen, const BYTE* higherLayer,
                                 size_t higherLayerLen)
{
	if (!udp || !udp->tunnelEstablished)
		return -1;
	if (!subheaders && (subheadersLen != 0))
		return -1;
	if (!higherLayer && (higherLayerLen != 0))
		return -1;
	wStream* s = rdpemt_build_tunnel_data(subheaders, subheadersLen, higherLayer,
	                                      higherLayerLen);
	if (!s)
		return -1;
	const size_t total = Stream_Length(s);
	const SSIZE_T rc = tls_send_all(udp, Stream_Buffer(s), total);
	Stream_Release(s);
	return (rc == (SSIZE_T)total) ? (SSIZE_T)higherLayerLen : -1;
}

SSIZE_T rdpeudp_tunnel_send_autodetect(rdpUdpTransport* udp, BYTE subHeaderType,
                                       const BYTE* pdu, size_t pduLen)
{
	wStream* sub = nullptr;
	SSIZE_T rc = -1;
	if (!udp || !pdu || (pduLen == 0))
		return -1;
	sub = rdpemt_build_subheader(subHeaderType, pdu, pduLen);
	if (!sub)
		return -1;
	const size_t subLen = Stream_Length(sub);
	rc = rdpeudp_tunnel_send_full(udp, Stream_Buffer(sub), subLen, nullptr, 0);
	Stream_Release(sub);
	return (rc == 0) ? (SSIZE_T)pduLen : -1;
}

static SSIZE_T tls_recv_some(rdpUdpTransport* udp, BYTE* buffer, size_t len)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(udp->tls);
	if (!buffer && (len != 0))
		return -1;
	if (len == 0)
		return 0;
	if (udp_aborted(udp))
		return -1;
	ERR_clear_error();
	const int r = (len > (size_t)INT_MAX) ? INT_MAX : (int)len;
	const int rc = BIO_read(udp->tlsBio, buffer, r);
	if (rc > 0)
		return rc;
	if (!BIO_should_retry(udp->tlsBio))
		return -1;
	return 0; /* no data within BIO's internal wait */
}

int rdpeudp_tunnel_recv_full(rdpUdpTransport* udp, BYTE* subBuf, size_t subBufLen,
                             size_t* subLenOut, BYTE* payloadBuf, size_t payloadBufLen,
                             size_t* payloadLenOut, DWORD timeoutMs)
{
	BYTE tmp[16384] = { 0 };
	UINT64 deadline = 0;
	if (!udp || !udp->tunnelEstablished)
		return -1;
	deadline = udp_now_ms() + timeoutMs;

	while (TRUE)
	{
		BYTE action = 0;
		UINT16 plen = 0;
		UINT8 hlen = 0;
		size_t total = 0;
		size_t subLen = 0;
		size_t buffered = 0;
		BYTE* base = nullptr;

		/* 1. Check for a complete PDU in the reassembly buffer (pure
		 * framing via rdpemt_tunnel_consume; same helper covered by unit
		 * tests with split fragments). */
		EnterCriticalSection(&udp->lock);
		if (!udp->tunnelBuf)
		{
			LeaveCriticalSection(&udp->lock);
			return -1;
		}
		buffered = Stream_Length(udp->tunnelBuf);
		base = Stream_Buffer(udp->tunnelBuf);
		{
			size_t consumed = 0;
			size_t gotSub = 0;
			size_t gotPayload = 0;
			const int cr = rdpemt_tunnel_consume(
			    base, buffered, &consumed, subBuf, subBufLen, &gotSub, payloadBuf,
			    payloadBufLen, &gotPayload);
			if (cr == 1)
			{
				const size_t remain = buffered - consumed;
				if (remain > 0)
					memmove(base, base + consumed, remain);
				(void)Stream_SetPosition(udp->tunnelBuf, 0);
				(void)Stream_SetLength(udp->tunnelBuf, remain);
				(void)Stream_SetPosition(udp->tunnelBuf, 0);
				LeaveCriticalSection(&udp->lock);
				if (subLenOut)
					*subLenOut = gotSub;
				if (payloadLenOut)
					*payloadLenOut = gotPayload;
				return 1;
			}
			if (cr == -1)
			{
				/* Corrupt stream; resync by dropping buffered bytes. */
				(void)Stream_SetPosition(udp->tunnelBuf, 0);
				(void)Stream_SetLength(udp->tunnelBuf, 0);
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			if (cr == -2)
			{
				/* Complete but caller buffers too small; preserve bytes. */
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			/* cr == 0: incomplete, fall through to read more below. */
		}
		/* Incomplete: capture buffered count for fast-path check below. */
		buffered = Stream_Length(udp->tunnelBuf);
		(void)action;
		(void)plen;
		(void)hlen;
		(void)total;
		(void)subLen;
		LeaveCriticalSection(&udp->lock);

		/* 2. Timeout? Preserve progress and return "no PDU yet".
		 * NOTE: timeoutMs==0 means "one non-blocking attempt", not "already
		 * expired": fall through to the readiness/read attempt below and only
		 * return 0 after it (see step 4). Checking expiry first would return
		 * before ever reading, starving the event loop (S1). */
		if ((timeoutMs != 0) && (udp_now_ms() >= deadline))
			return 0;
		if (udp_aborted(udp))
			return -1;

		/* 3. Fast path for non-blocking drain: if nothing is buffered in TLS
		 * and the socket has nothing readable, avoid the BIO's internal wait
		 * and return immediately (progress preserved in tunnelBuf). */
		if (timeoutMs == 0)
		{
			BOOL tlsBuffered = FALSE;
			BOOL sockReadable = FALSE;
			int fd = -1;
			EnterCriticalSection(&udp->lock);
			if (udp->recvStream)
				tlsBuffered = (Stream_Length(udp->recvStream) > udp->recvPos);
			fd = udp->sockfd;
			LeaveCriticalSection(&udp->lock);
			if (!tlsBuffered && (buffered == 0))
			{
				if (fd >= 0)
					sockReadable = freerdp_udp_wait_readable(fd, 0);
				if (!sockReadable)
					return 0;
			}
			else if (!tlsBuffered && (buffered > 0))
			{
				/* Partial PDU buffered but TLS has nothing new yet; still try
				 * one read in case a record is mid-reassembly, else return. */
				if (fd >= 0)
					sockReadable = freerdp_udp_wait_readable(fd, 0);
				if (!sockReadable)
					return 0;
			}
		}

		/* 4. Read more TLS bytes (single attempt) and append to reassembly. */
		{
			UINT64 now = udp_now_ms();
			DWORD chunkWait = 0;
			if (deadline > now)
			{
				UINT64 left = deadline - now;
				chunkWait = (left > 50) ? 50 : (DWORD)left;
				WINPR_UNUSED(chunkWait);
			}
			/* tls_recv_some does one BIO_read (internal ~100ms max when data
			 * is pending; returns 0 quickly-ish otherwise). */
			const SSIZE_T got = tls_recv_some(udp, tmp, sizeof(tmp));
			if (got < 0)
				return -1;
			if (got == 0)
			{
				if (udp_now_ms() >= deadline)
					return 0;
				/* No progress; poll briefly before retrying (unless non-blocking
				 * drain, where one attempt suffices). */
				if (timeoutMs == 0)
					return 0;
				continue;
			}
			EnterCriticalSection(&udp->lock);
			if (!udp->tunnelBuf)
			{
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			if (Stream_Length(udp->tunnelBuf) + (size_t)got > (1u << 20))
			{
				LeaveCriticalSection(&udp->lock);
				return -1; /* runaway peer; avoid OOM */
			}
			if (!Stream_SetPosition(udp->tunnelBuf, Stream_Length(udp->tunnelBuf)) ||
			    !Stream_EnsureRemainingCapacity(udp->tunnelBuf, (size_t)got))
			{
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			Stream_Write(udp->tunnelBuf, tmp, (size_t)got);
			Stream_SealLength(udp->tunnelBuf);
			(void)Stream_SetPosition(udp->tunnelBuf, 0);
			LeaveCriticalSection(&udp->lock);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Stats / keepalive / event                                            */
/* ------------------------------------------------------------------ */

BOOL rdpeudp_get_stats(const rdpUdpTransport* udp, RdpUdpStats* stats)
{
	if (!udp || !stats)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&udp->lock);
	*stats = udp->stats;
	LeaveCriticalSection((CRITICAL_SECTION*)&udp->lock);
	return TRUE;
}

int rdpeudp_get_sockfd(const rdpUdpTransport* udp)
{
	return udp ? udp->sockfd : -1;
}

HANDLE rdpeudp_get_event(rdpUdpTransport* udp)
{
	if (!udp)
		return nullptr;
	EnterCriticalSection(&udp->lock);
	HANDLE ev = nullptr;
	if (udp->sockEvent && (udp->sockEvent != INVALID_HANDLE_VALUE))
		ev = udp->sockEvent;
	else
		ev = udp->udpEvent;
	LeaveCriticalSection(&udp->lock);
	return ev;
}

BOOL rdpeudp_send_keepalive(rdpUdpTransport* udp)
{
	if (!udp || !udp->connected)
		return FALSE;
	/* Dummy packet keeps NAT binding alive without affecting reliability.
	 * Per MS-RDPEUDP2, dummy loss must not trigger retransmit and contents
	 * are ignored by higher layers. */
	wStream* s = Stream_New(nullptr, 16);
	if (!s)
		return FALSE;
	/* Minimal layout: header only (no payloads would be invalid, so include
	 * empty DATA? Instead send ACK-only as keepalive which also progresses
	 * the window. Prefer ACK-only over dummy for simplicity. */
	Stream_Release(s);

	EnterCriticalSection(&udp->lock);
	const BOOL useAckvec = FALSE;
	UINT16 base = 0;
	if (udp->recvDataBase != 0 || udp->recvDataSeen[0])
		base = (UINT16)(udp->recvDataBase - 1);
	else
		base = udp->lastAckSent;
	LeaveCriticalSection(&udp->lock);
	WINPR_UNUSED(useAckvec);

	wStream* pkt = rdpeudp2_build_packet(udp, RDPUDP2_FLAG_ACK, base, nullptr, 0, 0, 0,
	                                     nullptr, 0, FALSE);
	if (!pkt)
		return FALSE;
	const BOOL rc = rdpeudp_udp_send_stream(udp, pkt);
	Stream_Release(pkt);
	if (rc)
	{
		udp->lastKeepaliveTs = udp_now_ms();
		WLog_DBG(TAG, "RDP-UDP keepalive sent");
	}
	return rc;
}

BOOL rdpeudp_check_keepalive(rdpUdpTransport* udp, DWORD idleMs)
{
	if (!udp || !udp->connected)
		return FALSE;
	const UINT64 now = udp_now_ms();
	BOOL need = FALSE;
	EnterCriticalSection(&udp->lock);
	if ((now - udp->lastSendTs >= idleMs) && (now - udp->lastKeepaliveTs >= idleMs))
		need = TRUE;
	LeaveCriticalSection(&udp->lock);
	if (need)
		return rdpeudp_send_keepalive(udp);
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Server-side accept ([MS-RDPEUDP] 3.2 server, [MS-RDPEMT] 3.2.5.1)      */
/* Single-peer MVP: binds UDP port, handles one SYN handshake, TLS     */
/* accept, Tunnel Create validation. For multi-peer, caller should      */
/* demux by cookieHash and call per-peer (future: shared listener).     */
/* ------------------------------------------------------------------ */

static int rdpeudp_bind_udp(int port)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0)
	{
		fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (fd < 0)
			return -1;
		{
			int one = 1;
			(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
			(void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
		}
		struct sockaddr_in a4 = { 0 };
		a4.sin_family = AF_INET;
		a4.sin_addr.s_addr = htonl(INADDR_ANY);
		a4.sin_port = htons((UINT16)port);
		if (bind(fd, (struct sockaddr*)&a4, sizeof(a4)) != 0)
		{
			freerdp_udp_close(fd);
			return -1;
		}
		{
			int rcvbuf = 2 * 1024 * 1024;
			(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
		}
		if (!freerdp_udp_set_nonblocking(fd, TRUE))
			WLog_WARN(TAG, "could not set accept UDP socket non-blocking");
		return fd;
	}
	{
		int one = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
		int off = 0;
		(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
		struct sockaddr_in6 a6 = { 0 };
		a6.sin6_family = AF_INET6;
		a6.sin6_addr = in6addr_any;
		a6.sin6_port = htons((UINT16)port);
		if (bind(fd, (struct sockaddr*)&a6, sizeof(a6)) != 0)
		{
			freerdp_udp_close(fd);
			return -1;
		}
	}
	{
		int rcvbuf = 2 * 1024 * 1024;
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	}
	if (!freerdp_udp_set_nonblocking(fd, TRUE))
		WLog_WARN(TAG, "could not set accept UDP socket non-blocking");
	return fd;
}

rdpUdpTransport* rdpeudp_accept(rdpContext* context, int port, UINT32 expectedReqId,
                                const BYTE* expectedCookie, DWORD timeoutMs)
{
	return rdpeudp_accept_ex(context, port, expectedReqId, expectedCookie, timeoutMs, nullptr);
}

static BOOL rdpeudp_accept_aborted(rdpUdpTransport* udp, rdpContext* context)
{
	if (udp && udp_aborted(udp))
		return TRUE;
	if (context && context->rdp && freerdp_shall_disconnect_context(context))
		return TRUE;
	return FALSE;
}

rdpUdpTransport* rdpeudp_accept_ex(rdpContext* context, int port, UINT32 expectedReqId,
                                   const BYTE* expectedCookie, DWORD timeoutMs,
                                   HANDLE abortEvent)
{
	if (!context || !expectedCookie || (port <= 0))
		return nullptr;

	/* Create server transport shell (no hostname, inbound) */
	rdpUdpTransport* udp = calloc(1, sizeof(rdpUdpTransport));
	if (!udp)
		return nullptr;
	udp->log = WLog_Get(TAG);
	udp->context = context;
	udp->hostname = _strdup("0.0.0.0");
	if (!udp->hostname)
	{
		free(udp);
		return nullptr;
	}
	udp->port = port;
	udp->sockfd = -1;
	udp->requestId = expectedReqId;
	udp->requestedProto = INITIATE_REQUEST_PROTOCOL_UDPFECR;
	udp->abortEvent = abortEvent;
	memcpy(udp->securityCookie, expectedCookie, RDPEUDP_COOKIE_LEN);
	udp_compute_cookie_hash(expectedCookie, udp->cookieHash);
	udp->logWindow = RDPUDP2_DEFAULT_LOGWINDOW;
	udp->peerLogWindow = RDPUDP2_DEFAULT_LOGWINDOW;
	udp->negotiatedUpMtu = RDPUDP2_MTU;
	udp->negotiatedDownMtu = RDPUDP2_MTU;
	udp->maxPayload = RDPUDP2_MAX_PAYLOAD;
	udp->overhead = 50;
	udp->delayAckMax = 2;
	udp->delayAckTimeoutMs = 50;
	udp->lastKeepaliveTs = udp_now_ms();
	udp->udpEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!udp->udpEvent || (udp->udpEvent == INVALID_HANDLE_VALUE))
	{
		udp->udpEvent = nullptr;
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	udp->recvStream = Stream_New(nullptr, 65536);
	if (!udp->recvStream)
	{
		(void)CloseHandle(udp->udpEvent);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	/* Stream_New sets length == capacity; the reassembly buffer starts empty. */
	if (!Stream_SetPosition(udp->recvStream, 0) || !Stream_SetLength(udp->recvStream, 0))
	{
		(void)CloseHandle(udp->udpEvent);
		Stream_Release(udp->recvStream);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	udp->tunnelBuf = Stream_New(nullptr, 131072);
	if (!udp->tunnelBuf)
	{
		(void)CloseHandle(udp->udpEvent);
		Stream_Release(udp->recvStream);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	if (!Stream_SetPosition(udp->tunnelBuf, 0) || !Stream_SetLength(udp->tunnelBuf, 0))
	{
		(void)CloseHandle(udp->udpEvent);
		Stream_Release(udp->recvStream);
		Stream_Release(udp->tunnelBuf);
		free(udp->hostname);
		free(udp);
		return nullptr;
	}
	InitializeCriticalSection(&udp->lock);

	udp->sockfd = rdpeudp_bind_udp(port);
	if (udp->sockfd < 0)
	{
		WLog_ERR(TAG, "UDP bind :%d failed", port);
		rdpeudp_free(udp);
		return nullptr;
	}

	/* 1. Wait for SYN (v1) */
	const UINT64 deadline = udp_now_ms() + timeoutMs;
	RdpUdpFecHeader rhdr = { 0 };
	RdpUdpSynPayload rsyn = { 0 };
	RdpUdpSynExPayload rex = { 0 };
	BYTE peerCorr[RDPEUDP_CORRELATION_LEN] = { 0 };
	BOOL gotSyn = FALSE;
	UINT32 peerSeq = 0;

	while (udp_now_ms() < deadline)
	{
		if (rdpeudp_accept_aborted(udp, context))
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		BYTE buf[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
		/* Use recvfrom to learn peer? Our sock is unconnected; freerdp_udp_recv
		 * uses recv (connected). For accept, peek with recvfrom then connect. */
		struct sockaddr_storage peer = { 0 };
		socklen_t plen = sizeof(peer);
		if (!freerdp_udp_wait_readable(udp->sockfd, 100))
			continue;
		const SSIZE_T r = recvfrom(udp->sockfd, (char*)buf, sizeof(buf), 0,
		                           (struct sockaddr*)&peer, &plen);
		if (r <= 0)
			continue;
		if (!rdpeudp_parse_syn(buf, (size_t)r, &rhdr, &rsyn, peerCorr, &rex))
			continue;
		if (!(rhdr.flags & RDPUDP_FLAG_SYN) || (rhdr.flags & RDPUDP_FLAG_ACK))
			continue;
		/* Validate version: require v3 (UDP2) for MVP */
		UINT16 ver = RDPUDP_PROTOCOL_VERSION_1;
		if ((rhdr.flags & RDPUDP_FLAG_SYNEX) &&
		    (rex.synexFlags & RDPUDP_SYNEX_VERSION_VALID))
			ver = rex.version;
		if (ver != RDPUDP_PROTOCOL_VERSION_3)
		{
			WLog_WARN(TAG, "peer SYN version 0x%04x, need v3", ver);
			continue;
		}
		/* Validate cookieHash if present */
		if (rex.haveCookieHash && (memcmp(rex.cookieHash, udp->cookieHash, 32) != 0))
		{
			WLog_WARN(TAG, "peer SYN cookieHash mismatch (expected req %u)", expectedReqId);
			/* Per spec, downgrade to v2 instead of fail. For MVP, fail to
			 * avoid binding wrong TCP connection. */
			continue;
		}
		peerSeq = rsyn.initialSeq;
		/* MTU negotiation (server view): our 1232 vs peer's values */
		{
			UINT16 negUp = 0;
			UINT16 negDown = 0;
			rdpeudp_compute_mtu(RDPUDP2_MTU, RDPUDP2_MTU, rsyn.upstreamMtu,
			                    rsyn.downstreamMtu, &negUp, &negDown);
			udp->negotiatedUpMtu = negUp;
			udp->negotiatedDownMtu = negDown;
			udp->maxPayload = rdpeudp_payload_for_mtu(negUp);
		}
		/* Connect socket to peer for filtering */
		if (connect(udp->sockfd, (struct sockaddr*)&peer, plen) != 0)
		{
			WLog_WARN(TAG, "UDP connect-back to peer failed");
			continue;
		}
		(void)rdpeudp_attach_socket_event(udp);
		if (winpr_RAND(&udp->initialSeq, sizeof(udp->initialSeq)) < 0)
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		memcpy(udp->correlationId, peerCorr, sizeof(peerCorr));
		gotSyn = TRUE;
		break;
	}

	if (!gotSyn)
	{
		WLog_ERR(TAG, "UDP accept: no SYN on :%d", port);
		rdpeudp_free(udp);
		return nullptr;
	}

	/* 2. Send SYN+ACK */
	{
		wStream* synack = rdpeudp_build_syn(udp->initialSeq, RDPUDP2_MTU, RDPUDP2_MTU,
		                                    peerSeq, TRUE, udp->correlationId,
		                                    RDPUDP_PROTOCOL_VERSION_3, nullptr);
		if (!synack || !rdpeudp_udp_send_stream(udp, synack))
		{
			if (synack)
				Stream_Release(synack);
			rdpeudp_free(udp);
			return nullptr;
		}
		Stream_Release(synack);
	}

	/* 3. Wait for final ACK (v1 ACK, no SYN) */
	{
		BOOL ok = FALSE;
		const UINT64 adead = udp_now_ms() + 5000;
		while (udp_now_ms() < adead)
		{
			if (rdpeudp_accept_aborted(udp, context))
			{
				rdpeudp_free(udp);
				return nullptr;
			}
			BYTE buf[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
			const SSIZE_T r = freerdp_udp_recv(udp->sockfd, buf, sizeof(buf), 100);
			if (r <= 0)
				continue;
			RdpUdpFecHeader h = { 0 };
			wStream sb = { 0 };
			wStream* s = Stream_StaticConstInit(&sb, buf, (size_t)r);
			if (!rdpeudp_read_fec_header(s, &h))
				continue;
			if ((h.flags & RDPUDP_FLAG_ACK) && !(h.flags & RDPUDP_FLAG_SYN) &&
			    (h.snSourceAck == udp->initialSeq))
			{
				ok = TRUE;
				break;
			}
		}
		if (!ok)
		{
			WLog_ERR(TAG, "UDP accept: no final ACK");
			rdpeudp_free(udp);
			return nullptr;
		}
	}

	udp->peerInitialSeq = peerSeq;
	udp->negotiatedVer = RDPUDP_PROTOCOL_VERSION_3;
	udp->connected = TRUE;
	udp->useUdp2 = TRUE;
	udp->lastRecvTs = udp_now_ms();
	udp->lastSendTs = udp->lastRecvTs;

	/* 4. TLS accept over reliable UDP */
	{
		rdpTls* tls = freerdp_tls_new(context);
		if (!tls)
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		BIO* ubio = BIO_new(BIO_s_rdpeudp());
		if (!ubio)
		{
			freerdp_tls_free(tls);
			rdpeudp_free(udp);
			return nullptr;
		}
		RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(ubio);
		ptr->udp = udp;
		/* freerdp_tls_accept takes frontBio + settings internally? Use accept API
		 * mirroring transport_default_accept_tls. */
		tls->isGatewayTransport = FALSE;
		/* Note: tls->bio will be stacked over ubio on success */
		if (!freerdp_tls_accept(tls, ubio, context->settings))
		{
			WLog_ERR(TAG, "TLS accept over RDP-UDP failed");
			freerdp_tls_free(tls);
			rdpeudp_free(udp);
			return nullptr;
		}
		udp->tls = tls;
		udp->tlsBio = tls->bio;
		udp->udpBio = ubio;
	}

	/* 5. Wait Tunnel Create Request, validate, send Response S_OK */
	{
		BYTE hdr[4] = { 0 };
		if (tls_recv_all(udp, hdr, sizeof(hdr), 5000) != (SSIZE_T)sizeof(hdr))
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		BYTE action = 0;
		UINT16 plen = 0;
		UINT8 hlen = 0;
		if (!rdpemt_decode_header(hdr, sizeof(hdr), &action, &plen, &hlen) ||
		    (action != RDPTUNNEL_ACTION_CREATEREQUEST) || (hlen != 4) || (plen != 24))
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		BYTE pay[24] = { 0 };
		if (tls_recv_all(udp, pay, sizeof(pay), 5000) != (SSIZE_T)sizeof(pay))
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		const UINT32 reqId = (UINT32)pay[0] | ((UINT32)pay[1] << 8) |
		                     ((UINT32)pay[2] << 16) | ((UINT32)pay[3] << 24);
		/* pay[4..7] reserved, pay[8..23] cookie */
		if ((reqId != expectedReqId) || (memcmp(pay + 8, expectedCookie, 16) != 0))
		{
			WLog_ERR(TAG, "Tunnel Create cookie/req mismatch (got %u want %u)", reqId,
			         expectedReqId);
			/* Send failure response */
			BYTE fail[8] = { RDPTUNNEL_ACTION_CREATERESPONSE, 0x04, 0x00, 0x04,
				             0x04, 0x04, 0x00, 0x80 }; /* E_ABORT-ish */
			(void)tls_send_all(udp, fail, sizeof(fail));
			rdpeudp_free(udp);
			return nullptr;
		}
		BYTE ok[8] = { RDPTUNNEL_ACTION_CREATERESPONSE, 0x04, 0x00, 0x04,
			       0x00, 0x00, 0x00, 0x00 };
		if (tls_send_all(udp, ok, sizeof(ok)) != (SSIZE_T)sizeof(ok))
		{
			rdpeudp_free(udp);
			return nullptr;
		}
		udp->tunnelEstablished = TRUE;
	}

	WLog_INFO(TAG, "RDP-UDP accepted tunnel %u on :%d", expectedReqId, port);
	return udp;
}
