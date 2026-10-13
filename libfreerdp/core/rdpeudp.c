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
#include <winpr/path.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>
#include <winpr/winsock.h>

#include <freerdp/log.h>

#include <freerdp/utils/drdynvc.h>

#include <openssl/bio.h>
#include <openssl/sha.h>

#include "udp.h"
#include "rdpeudp.h"
#include "performance.h"
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
	/* Prefix byte MSB-first: C(3)=shortLen | B(4)=type | A(1)=0.
	 * Normal DATA wire capture (MS client): 0xE0; dummy: 0xF0.
	 * A previous revision had the nibbles mirrored ((type<<3)|shortLen =
	 * 0x07), which decodes as reserved packet type 3: Windows silently
	 * drops every UDP2 datagram while the v1 handshake still succeeds. */
	return (BYTE)(((shortLen & 0x07) << 5) | ((type & 0x0F) << 1));
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
	/* Prefix byte MSB-first: C(3)=shortLen | B(4)=type | A(1)=0, so the
	 * packet type lives in bits 4:1 (mask 0x1E, Data=0, Dummy=8).
	 * Same convention as tools/wireshark/rdp-udp.lua. */
	const BYTE typeSpec = (BYTE)((prefix >> 1) & 0x0F);
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
	if (!data || (dataLen < 2))
		return nullptr;
	if (dataLen > 4096)
		return nullptr;
	if ((subHeaderType != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) &&
	    (subHeaderType != RDP_TUNNEL_SUBHEADER_AUTODETECT_RSP))
		return nullptr;
	/* Overlaid model (MS-RDPEMT 2.2.1.1.1 as observed on the wire): the
	 * subheader IS the PDU's own RDPBCGR header start. SubHeaderLength is
	 * the total PDU length including these framing bytes, SubHeaderType
	 * equals the PDU's headerTypeId. E.g. a Bandwidth Measure Start is
	 * emitted as its own 6 bytes [06 00 ...], a Network Characteristics
	 * Result as its 18 bytes [12 00 ...]. A previous revision wrote a
	 * constant Length=2 plus a full PDU copy (duplicated header); Windows
	 * parses the overlaid form. */
	if (data[1] != subHeaderType)
		return nullptr;
	if (data[0] != dataLen)
		return nullptr;
	if (rdpemt_autodetect_pdu_length(data, dataLen) != dataLen)
		return nullptr;
	wStream* s = Stream_New(nullptr, dataLen);
	if (!s)
		return nullptr;
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
	/* Overlaid model: the subheader header IS the PDU's own RDPBCGR
	 * header start. SubHeaderLength is the total PDU length including
	 * these framing bytes (live: an 18-byte region holds exactly one
	 * [12 00 ...] Network Characteristics Result); SubHeaderType equals
	 * the PDU's headerTypeId. The returned data is the FULL PDU. */
	const BYTE subLen = subheaders[off];
	const BYTE subType = subheaders[off + 1];
	if (subLen < 2)
		return FALSE;
	if ((subType != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) &&
	    (subType != RDP_TUNNEL_SUBHEADER_AUTODETECT_RSP))
		return FALSE;
	if ((size_t)subLen > subheadersLen - off)
		return FALSE;
	const BYTE* pdu = subheaders + off;
	const size_t avail = subheadersLen - off;
	const size_t pduLen = rdpemt_autodetect_pdu_length(pdu, avail);
	if ((pduLen == 0) || (pduLen != subLen))
		return FALSE;
	if (subHeaderType)
		*subHeaderType = subType;
	if (subData)
		*subData = pdu;
	if (subDataLen)
		*subDataLen = pduLen;
	*offset = off + subLen;
	return TRUE;
}

/* Soft-Sync validation lives in the shared drdynvc utils so the DVC managers
 * and the multitransport layer always agree (U1/T2). Thin wrappers below
 * preserve the rdpeudp_* API used by multitransport, channels, and tests. */

BOOL rdpeudp_soft_sync_request_offers_udp(const BYTE* pdu, size_t len)
{
	return drdynvc_soft_sync_request_offers_udp(pdu, len);
}

BOOL rdpeudp_soft_sync_request_udp_dvcs(const BYTE* pdu, size_t len, UINT32* outIds,
                                        size_t maxIds, size_t* countOut)
{
	return drdynvc_soft_sync_request_udp_dvcs(pdu, len, outIds, maxIds, countOut);
}

BOOL rdpeudp_soft_sync_response_offers_udp(const BYTE* pdu, size_t len)
{
	return drdynvc_soft_sync_response_offers_udp(pdu, len);
}

/* ------------------------------------------------------------------ */
/* ACK vector codec ([MS-RDPEUDP2] 2.2.1.2.6)                           */
/* ------------------------------------------------------------------ */

typedef BOOL (*RdpUdpAckVecState)(const void* context, size_t index);

typedef struct
{
	BYTE coded[127];
	size_t length;
	size_t covered;
	size_t highestReceived;
} RdpUdpAckVecEncoding;

static BOOL rdpeudp_encode_ackvec_states(const void* context, RdpUdpAckVecState state, size_t count,
                                         RdpUdpAckVecEncoding* out)
{
	if (!context || !state || !out || !count || count > 0x8000)
		return FALSE;
	memset(out, 0, sizeof(*out));
	out->highestReceived = SIZE_MAX;

	/* Bitmap mode always covers exactly 7 seqs; RLE covers exact run.
	 * Greedy: RLE for runs >=4, else full 7-bit bitmaps (pad last). */
	size_t i = 0;
	while (i < count && out->length < sizeof(out->coded))
	{
		const BOOL received = state(context, i);
		size_t run = 1;
		while ((i + run) < count && run < 63 && state(context, i + run) == received)
			run++;

		if (run >= 4)
		{
			const size_t r = (run > 63) ? 63 : run;
			out->coded[out->length++] = (BYTE)(0x80 | (received ? 0x40 : 0x00) | (r & 0x3F));
			if (received)
				out->highestReceived = i + r - 1;
			i += r;
		}
		else
		{
			BYTE b = 0;
			for (size_t n = 0; n < 7; n++)
			{
				if ((i + n) < count && state(context, i + n))
				{
					b |= (BYTE)(1u << n);
					out->highestReceived = i + n;
				}
			}
			out->coded[out->length++] = b;
			i += 7;
			if (i > count)
				i = count;
		}
	}
	out->covered = i;
	return TRUE;
}

static wStream* rdpeudp_ackvec_stream(UINT16 baseSeq, const RdpUdpAckVecEncoding* encoded,
                                      BOOL withTs, UINT32 ts, BYTE sendGap)
{
	wStream* s = Stream_New(nullptr, encoded->length + 8);
	if (!s)
		return nullptr;
	Stream_Write_UINT16(s, baseSeq);
	Stream_Write_UINT8(s, (BYTE)((encoded->length & 0x7F) | (withTs ? 0x80 : 0x00)));
	if (withTs)
	{
		Stream_Write_UINT8(s, (BYTE)(ts & 0xFF));
		Stream_Write_UINT8(s, (BYTE)((ts >> 8) & 0xFF));
		Stream_Write_UINT8(s, (BYTE)((ts >> 16) & 0xFF));
		Stream_Write_UINT8(s, sendGap);
	}
	Stream_Write(s, encoded->coded, encoded->length);
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
	{
		Stream_Release(s);
		return nullptr;
	}
	return s;
}

static BOOL rdpeudp_ackvec_array_state(const void* context, size_t index)
{
	const BOOL* received = context;
	return received[index];
}

wStream* rdpeudp_build_ackvec(UINT16 baseSeq, const BOOL* received, size_t count, BOOL withTs,
                              UINT32 ts, BYTE sendGap)
{
	RdpUdpAckVecEncoding encoded = { 0 };
	if (!rdpeudp_encode_ackvec_states(received, rdpeudp_ackvec_array_state, count, &encoded))
		return nullptr;
	return rdpeudp_ackvec_stream(baseSeq, &encoded, withTs, ts, sendGap);
}

/* Validate the vector before packet processing can update liveness, AOA or
 * channel receipt. Keep this allocation-free and shared with the decoder. */
static BOOL rdpeudp_ackvec_count(const BYTE* data, size_t len, size_t* count)
{
	if (!data || (len < 3) || !count)
		return FALSE;
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
	/* Seven-bit coded size and six-bit run lengths bound this to 127*63
	 * packets, including valid windows larger than the former local 1024 cap. */
	if (total == 0)
		return FALSE;
	*count = total;
	return TRUE;
}

BOOL rdpeudp_parse_ackvec(const BYTE* data, size_t len, UINT16* baseSeq, BOOL** received,
                          size_t* count)
{
	size_t total = 0;
	if (!baseSeq || !received || !count || !rdpeudp_ackvec_count(data, len, &total))
		return FALSE;
	const UINT16 base = (UINT16)(data[0] | ((UINT16)data[1] << 8));
	const BYTE b2 = data[2];
	const BYTE csize = (BYTE)(b2 & 0x7F);
	const BOOL haveTs = (b2 & 0x80) != 0;
	const BYTE* vec = data + 3 + (haveTs ? 4 : 0);

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

/* Length of one DYNVC PDU at data (MS-RDPEDYC 2.2, header byte
 * [cbId(2)|Sp/Pri(2)|Cmd(4)] + ChannelId of cbChId width). Live servers put
 * RAW DVC PDUs in Tunnel DATA HigherLayerData with no outer envelope
 * (observed: CREATEs `18 02/07/08 <name>\0`, one PDU per Tunnel DATA).
 * Returns FALSE when the bytes do not hold exactly one complete PDU of a
 * supported command (CREATE/CLOSE/DATA_FIRST/DATA/SOFT-SYNC incl. compressed
 * DATA forms; CAPABILITY PDUs go through their dedicated handlers).
 * DATA_FIRST beyond the present bytes and unknown commands fail visibly. */
BOOL rdpeudp_dvc_pdu_length(const BYTE* data, size_t len, BOOL isServer, size_t* pduLenOut)
{
	UINT8 cmd = 0;
	UINT8 cbChId = 0;
	size_t idLen = 0;
	size_t off = 0;
	if (!data || (len < 1))
		return FALSE;
	cmd = (UINT8)((data[0] >> 4) & 0x0F);
	cbChId = (UINT8)(data[0] & 0x03);
	idLen = (cbChId == 0) ? 1 : ((cbChId == 1) ? 2 : 4);
	if (len < 1 + idLen)
		return FALSE;
	off = 1 + idLen;
	switch (cmd)
	{
		case 0x01: /* CREATE_REQUEST (client receives) vs CREATE_RESPONSE
		              (server receives): same command nibble, direction
		              decides the layout (N4). Request carries a
		              NUL-terminated name; response carries a 4-byte status. */
			if (!isServer)
			{
				size_t nameLen = 0;
				while (off + nameLen < len)
				{
					if (data[off + nameLen] == 0)
					{
						if (pduLenOut)
							*pduLenOut = off + nameLen + 1;
						return TRUE;
					}
					nameLen++;
				}
				return FALSE; /* name runs past HigherLayerData */
			}
			if (len < off + 4)
				return FALSE;
			if (pduLenOut)
				*pduLenOut = off + 4;
			return TRUE;
		case 0x04: /* CLOSE_REQUEST: header + ChannelId only */
			if (pduLenOut)
				*pduLenOut = off;
			return TRUE;
		case 0x02: /* DATA_FIRST (+0x06 compressed): header + ChannelId +
		              Length + first bytes. Length width follows the middle
		              bits exactly like drdynvc_read_variable_uint (0->1B,
		              1->2B, else 4B) and counts message DATA bytes after the
		              Length field (dvcman allocates exactly this and appends
		              across fragments until full). A first fragment carries
		              only part of it (more fragments follow in later Tunnel
		              DATAs); a complete message may share its HigherLayer
		              with trailing PDUs. */
		case 0x06:
		{
			const size_t lenLen =
			    ((((data[0] >> 2) & 0x03) == 0)
			         ? 1
			         : ((((data[0] >> 2) & 0x03) == 1) ? 2 : 4));
			if (len < off + lenLen)
				return FALSE;
			UINT32 total = 0;
			for (size_t li = 0; li < lenLen; li++)
				total |= ((UINT32)data[off + li] << (8 * li));
			const size_t dataOff = off + lenLen;
			if (total == 0)
				return FALSE;
			if (total <= len - dataOff)
			{
				/* Complete here; trailers may follow in this HigherLayer. */
				if (pduLenOut)
					*pduLenOut = dataOff + total;
			}
			else
			{
				/* First fragment: rest arrives later; consume all here. */
				if (pduLenOut)
					*pduLenOut = len;
			}
			return TRUE;
		}
		case 0x03: /* DATA (+0x07 compressed): header + ChannelId + rest */
		case 0x07:
			if (pduLenOut)
				*pduLenOut = len;
			return TRUE;
		case 0x08: /* SOFT_SYNC_REQUEST (+0x09 response): header + Pad +
		              Length(u32: total size of Length and everything after)
		              + body. Length framing is trusted for SPLITTING only;
		              content is strictly validated downstream by
		              drdynvc_order_recv via the shared parsers, which reject
		              garbage loudly. */
		case 0x09:
		{
			if (len < 6)
				return FALSE;
			const UINT32 bodyLen =
			    (UINT32)data[2] | ((UINT32)data[3] << 8) |
			    ((UINT32)data[4] << 16) | ((UINT32)data[5] << 24);
			if (bodyLen > 65535)
				return FALSE;
			const size_t total = (size_t)6 + bodyLen;
			if (total > len)
				return FALSE;
			if (pduLenOut)
				*pduLenOut = total;
			return TRUE;
		}
		default:
			return FALSE;
	}
}

/* ------------------------------------------------------------------ */
/* reliable RDPEUDP2 transport                                          */
/* ------------------------------------------------------------------ */

#define RDPEUDP2_WINDOW_MAX 64
#if defined(WITH_RDP_BENCH)
#define RDPEUDP2_SENT_STORAGE 128
#else
#define RDPEUDP2_SENT_STORAGE RDPEUDP2_WINDOW_MAX
#endif
#define RDPEUDP2_RECEIPT_WINDOW 32768u
#define RDPEUDP2_INITIAL_EPOCH_THRESHOLD 128u
#define RDPEUDP2_RECV_PAGE_SHIFT 8
#define RDPEUDP2_RECV_PAGE_SIZE (1u << RDPEUDP2_RECV_PAGE_SHIFT)
#define RDPEUDP2_RECV_PAGE_COUNT (1u << (16 - RDPEUDP2_RECV_PAGE_SHIFT))
#define RDPEUDP2_RECV_CHUNK_LIMIT 0x8000u
#define RDPEUDP2_RECV_MAX_FORWARD 0x7FFFu
#define RDPEUDP2_RECV_STREAM_RESERVE 65536u
#define RDPEUDP2_SEND_QUEUE_BYTES 32768
#define RDPEUDP2_FAST_LOSS_EVIDENCE 3
#define RDPEUDP2_REORDER_GRACE_MS 25
#define RDPEUDP2_DEFAULT_DELAYED_ACKS 8
#define RDPEUDP2_MAX_DELAYED_ACKS 15
#define RDPEUDP_RECOVERY_TRACE_CAPACITY 262144u
#define RDPEUDP_RECOVERY_TRACE_MAX_CAPACITY 1048576u
#define RDPEUDP_RECOVERY_TRACE_WIRE 134u

#if defined(WITH_RDP_BENCH)
typedef struct RdpBenchQueueObserver RdpBenchQueueObserver;
typedef struct RdpBenchLossTrace RdpBenchLossTrace;
#endif

typedef struct
{
	UINT16 dataSeq;
	UINT16 channelSeq;
	BOOL lossReported;
	BYTE* data;
	size_t len;
	UINT64 sentTs;
	UINT32 retries;
	UINT16 aliases[RDPEUDP_MAX_RETRIES + 1];
	size_t aliasCount;
	UINT64 firstSentTs;
	UINT32 rtoMs;
	/* Evidence belongs to the latest transmission attempt, not its aliases.
	 * Three distinct later receipts and a reorder grace distinguish a hole
	 * from the trailing bitmap padding and ordinary reordered arrivals. */
	BOOL gapActive;
	UINT64 gapSince;
	UINT16 gapPositiveSeq[RDPEUDP2_FAST_LOSS_EVIDENCE];
	BYTE gapPositiveCount;
} UdpSentEntry;

typedef struct
{
	BOOL occupied;
	UINT16 channelSeq;
	BYTE* data;
	size_t len;
} UdpRecvSlot;

typedef struct
{
	UdpRecvSlot slots[RDPEUDP2_RECV_PAGE_SIZE];
	size_t count;
} UdpRecvPage;

typedef struct
{
	UINT64 arrivalNs;
	UINT32 channel;
	UINT16 seq;
	BYTE received;
	BYTE arrivalValid;
} UdpRecvReceipt;

typedef struct
{
	UINT64 timeNs;
	UINT32 kind;
	UINT16 dataSeq;
	UINT16 channelSeq;
	UINT16 expectedChannel;
	UINT16 recvBase;
	UINT16 flags;
	UINT16 aoa;
	UINT16 oldBase;
	UINT16 newBase;
	UINT16 wireLength;
	UINT32 detail;
	UINT32 status;
	UINT32 bytes;
	UINT32 bufferedChunks;
	UINT32 ownedBytes;
	BYTE hash[8];
	BYTE wire[RDPEUDP_RECOVERY_TRACE_WIRE];
} UdpRecoveryEvent;

typedef struct
{
	char* path;
	char* armPath;
	UdpRecoveryEvent* events;
	size_t capacity;
	size_t count;
	size_t dropped;
	UINT64 startNs;
	UINT64 armNs;
	UINT64 nextArmProbeNs;
	UINT64 wallRawNs;
	UINT64 wallUnix100ns;
	UINT64 wallBracketNs;
	BOOL armed;
	BOOL invalid;
	BOOL flushed;
} UdpRecoveryTrace;

enum
{
	UDP_RECOVERY_RX_DATA = 1,
	UDP_RECOVERY_RX_AOA,
	UDP_RECOVERY_AUTO_SLIDE,
	UDP_RECOVERY_WINDOW_PROGRESS,
	UDP_RECOVERY_TX_ACK,
	UDP_RECOVERY_TX_DATA,
	UDP_RECOVERY_CHANNEL_PROGRESS,
	UDP_RECOVERY_GAP,
	UDP_RECOVERY_FAILURE,
	UDP_RECOVERY_ARM
};

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

	/* v2 reliable state ([MS-RDPEUDP2] reference: seq starts at 1, no ACK
	 * until data received). Fixed 1-start (V1): never rebase from first
	 * arrival; buffer reordered chunks. */
	UINT16 nextDataSeq;
	UINT16 nextChannelSeq;
	UINT16 expectedChannelSeq;
	BOOL haveRecvData;    /* Set on any in-window DATA record; sole reader is the
	                       * initial-epoch rule below (first far-ahead DATA adopts
	                       * the peer epoch). ACK emission and base validity for
	                       * steady state use haveRealData/haveSeenAoa. */
	BOOL haveSeenAoa;     /* TRUE once any AOA-bearing packet arrived (epoch known) */
	BOOL haveRealData;    /* TRUE once NON-DUMMY data arrived: gate for emitting
	                       * ACKs. Probes flip haveRecvData (base marches) but must
	                       * not trigger ACK emission on their own. */
	UINT16 lastAckSent;   /* last arrival (diagnostic only; ACK uses base-1, V2) */
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
	 * Sequence-tagged slots hold actual receipts, never inferred receipts. */
	UINT16 recvDataBase;
	/* Sequence-tagged ring entries retain actual receipt across a full
	 * unambiguous half-space. Moving the lower edge never copies the ring. */
	UdpRecvReceipt recvReceipts[RDPEUDP2_RECEIPT_WINDOW];
	size_t recvDataCount;
	UINT16 recvDataHighest;
	UINT64 recvInOrderArrivalNs;
	BOOL recvInOrderArrivalValid;
	UINT64 recvLatestArrivalNs;
	BOOL ackPending;
	size_t ackUnackedPackets;
	UINT16 ackedSeq;
	BOOL ackedGap;
	UINT64 ackDueNs;
	UINT64 ackFirstArrivalNs;
	BYTE peerAckMax;
	UINT32 peerAckTimeoutMs;
	BOOL testArrivalClockEnabled;
	UINT64 testArrivalClockNs;
	/* Process channel labels in DataSeq order. UINT32_MAX marks a dummy.
	 * Before the first wrap, each missing datagram could introduce at most
	 * one new channel label. The upper bound saturates at UINT16_MAX. */
	UINT16 recvChannelUpperBound;
	BOOL recvChannelZeroUncertain;
	/* Learned only from an unambiguous wrap, and scoped to this transport. */
	BOOL peerSkipsChannelZero;
	BOOL peerUsesChannelZero;
	BOOL recvGapActive;
	UINT64 recvGapSince;
	BOOL recvFailed;
	BOOL sendFailed;
	/* Socketless fixtures can drive time and inspect the actual ACK wire bytes. */
	BOOL testClockEnabled;
	UINT64 testClockMs;
	RdpUdpTestSendCallback testSend;
	void* testSendContext;
	BOOL needAoa;
	BYTE overhead;
	BYTE delayAckMax;
	UINT16 delayAckTimeoutMs;

	UdpSentEntry sent[RDPEUDP2_SENT_STORAGE];
	size_t sentCount;
	/* Owned bytes include queued payload and logical in-flight chunks. */
	wStream* sendQueue;
	size_t sendPos;
	size_t sendBytes;
	BOOL sendPumpActive;
	UINT16 sendAoa;
	BOOL sendAoaValid;
	UINT64 sendWakeGeneration;
	UINT64 sendWakeConsumed;
#if defined(WITH_RDP_BENCH)
	/* Private experiments never alter the normal-build queue bound. */
	size_t benchSendQueueBytes;
	size_t benchMaxInFlightPackets;
	size_t benchPeakInFlightPackets;
	size_t benchPeakActiveSpan;
	BYTE benchOriginalLogWindow;
	BOOL benchOriginalLogWindowSaved;
	RdpBenchQueueObserver* benchQueueObserver;
	RdpBenchLossTrace* benchLossTrace;
	BOOL benchTimerRecovery;
	/* Private campaign deadline only. Production peer/retry policy is unchanged. */
	UINT64 benchDeadlineNs;
	BOOL benchDeadlineReached;
#endif

	/* Full channel identities, allocated only for occupied 256-channel pages.
	 * Forward labels cover at most one unambiguous half of the sequence space.
	 * Payload ownership (unordered plus unread stream) is bounded separately. */
	UdpRecvPage* recvPages[RDPEUDP2_RECV_PAGE_COUNT];
	size_t recvBufferedChunks;
	size_t recvBufferedBytes;
	size_t recvBufferedPages;
	size_t recvPageAllocations;
	UINT16 recvHighestChannelSeq;
	size_t recvPeakOwnedBytes;
	size_t recvPeakBufferedChunks;
	size_t recvPeakBufferedSpan;
	/* Socketless regression drivers do not change normal receive limits. */
	size_t testRecvChunkLimit;
	size_t testRecvByteLimit;
	BOOL testRecvAllocFailureEnabled;
	size_t testRecvAllocSuccesses;
	/* Diagnostic only: a bounded opt-in ledger, never packet-by-packet I/O. */
	UdpRecoveryTrace* recoveryTrace;
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
	UINT32 srttMs;
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
#if defined(WITH_RDP_BENCH)
	if (udp && udp->benchDeadlineNs && (winpr_GetTickCount64NS() >= udp->benchDeadlineNs))
	{
		/* The benchmark is single-owner; record why a blocked call stopped. */
		((rdpUdpTransport*)udp)->benchDeadlineReached = TRUE;
		return TRUE;
	}
#endif
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

static UINT64 rdpeudp_recv_now(const rdpUdpTransport* udp)
{
	return udp->testClockEnabled ? udp->testClockMs : udp_now_ms();
}

static size_t rdpeudp2_local_protocol_capacity_locked(const rdpUdpTransport* udp)
{
	const BYTE log =
	    udp->logWindow < RDPUDP2_LOCAL_MAX_LOGWINDOW ? udp->logWindow : RDPUDP2_LOCAL_MAX_LOGWINDOW;
	return ((size_t)1 << log) - 1;
}

static size_t rdpeudp2_peer_protocol_capacity_locked(const rdpUdpTransport* udp)
{
	const BYTE log = udp->peerLogWindow < RDPUDP2_PROTOCOL_MAX_LOGWINDOW
	                     ? udp->peerLogWindow
	                     : RDPUDP2_PROTOCOL_MAX_LOGWINDOW;
	return ((size_t)1 << log) - 1;
}

static size_t rdpeudp2_usable_sent_storage_locked(const rdpUdpTransport* udp)
{
#if defined(WITH_RDP_BENCH)
	if (udp->benchMaxInFlightPackets)
		return ARRAYSIZE(udp->sent) - 1;
#else
	WINPR_UNUSED(udp);
#endif
	/* Extra benchmark storage never enlarges an association without opt-in. */
	return RDPEUDP2_WINDOW_MAX - 1;
}

static size_t rdpeudp2_protocol_window_locked(const rdpUdpTransport* udp)
{
	size_t limit = rdpeudp2_local_protocol_capacity_locked(udp);
	const size_t peer = rdpeudp2_peer_protocol_capacity_locked(udp);
	const size_t storage = rdpeudp2_usable_sent_storage_locked(udp);
	if (peer < limit)
		limit = peer;
	return storage < limit ? storage : limit;
}

static size_t rdpeudp2_send_window_locked(const rdpUdpTransport* udp)
{
	size_t limit = rdpeudp2_protocol_window_locked(udp);
#if defined(WITH_RDP_BENCH)
	if (udp->benchMaxInFlightPackets && udp->benchMaxInFlightPackets < limit)
		limit = udp->benchMaxInFlightPackets;
#endif
	return limit;
}

static UINT16 rdpeudp2_send_base_locked(const rdpUdpTransport* udp, size_t excluded);

#include "rdp_bench_queue_private.inc"
#include "rdp_bench_loss_private.inc"

static size_t rdpeudp2_send_queue_limit(const rdpUdpTransport* udp)
{
#if defined(WITH_RDP_BENCH)
	if (udp->benchSendQueueBytes)
		return udp->benchSendQueueBytes;
#else
	WINPR_UNUSED(udp);
#endif
	return RDPEUDP2_SEND_QUEUE_BYTES;
}

static UINT64 rdpeudp_recv_now_ns(const rdpUdpTransport* udp)
{
	if (udp->testArrivalClockEnabled)
		return udp->testArrivalClockNs;
	return udp->testClockEnabled ? udp->testClockMs * 1000000ULL : winpr_GetTickCount64NS();
}

static const UdpRecvReceipt* rdpeudp_recv_receipt_locked(const rdpUdpTransport* udp, UINT16 seq)
{
	return &udp->recvReceipts[seq & (RDPEUDP2_RECEIPT_WINDOW - 1)];
}

static BOOL rdpeudp_recv_data_seen_locked(const rdpUdpTransport* udp, UINT16 seq)
{
	const UdpRecvReceipt* receipt = rdpeudp_recv_receipt_locked(udp, seq);
	return receipt->received && receipt->seq == seq;
}

static size_t rdpeudp_recv_data_span_locked(const rdpUdpTransport* udp)
{
	return udp->recvDataCount ? (size_t)(UINT16)(udp->recvDataHighest - udp->recvDataBase) + 1 : 0;
}

static BOOL rdpeudp2_has_recv_gap_locked(const rdpUdpTransport* udp)
{
	return udp->recvDataCount > 0;
}

static UINT64 rdpeudp2_arrival_locked(const rdpUdpTransport* udp, UINT16 seq, BOOL* valid)
{
	const UdpRecvReceipt* receipt = rdpeudp_recv_receipt_locked(udp, seq);
	*valid = receipt->arrivalValid && receipt->seq == seq;
	return *valid ? receipt->arrivalNs : 0;
}

static BYTE rdpeudp2_ack_gap(UINT64 arrival, UINT64 now)
{
	const UINT64 gap = now >= arrival ? (now - arrival) / 1000000ULL : 0;
	return gap > 254 ? 254 : (BYTE)gap; /* 255 is invalid on wire. */
}

static BOOL rdpeudp2_service_send(rdpUdpTransport* udp);
static BOOL rdpeudp2_service_ack(rdpUdpTransport* udp);
static void rdpeudp2_update_send_aoa_locked(rdpUdpTransport* udp);

static size_t rdpeudp_recv_chunk_limit_locked(const rdpUdpTransport* udp)
{
	return udp->testRecvChunkLimit ? udp->testRecvChunkLimit : RDPEUDP2_RECV_CHUNK_LIMIT;
}

static size_t rdpeudp_recv_byte_limit_locked(const rdpUdpTransport* udp)
{
	return udp->testRecvByteLimit ? udp->testRecvByteLimit
	                              : (size_t)RDPEUDP2_RECV_CHUNK_LIMIT * udp->negotiatedDownMtu;
}

static size_t rdpeudp_recv_pending_stream_locked(const rdpUdpTransport* udp)
{
	return Stream_Length(udp->recvStream) - udp->recvPos;
}

static size_t rdpeudp_recv_span_locked(const rdpUdpTransport* udp)
{
	return udp->recvBufferedChunks
	           ? (size_t)(UINT16)(udp->recvHighestChannelSeq - udp->expectedChannelSeq) + 1
	           : 0;
}

static UdpRecvSlot* rdpeudp_recv_slot_locked(const rdpUdpTransport* udp, UINT16 channel)
{
	UdpRecvPage* page = udp->recvPages[channel >> RDPEUDP2_RECV_PAGE_SHIFT];
	return page ? &page->slots[channel & (RDPEUDP2_RECV_PAGE_SIZE - 1)] : nullptr;
}

static void rdpeudp_recovery_clock_mapping(UdpRecoveryTrace* trace)
{
	const UINT64 before = winpr_GetTickCount64NS();
	FILETIME wall = { 0 };
	GetSystemTimeAsFileTime(&wall);
	const UINT64 after = winpr_GetTickCount64NS();
	trace->wallRawNs = before + (after - before) / 2;
	trace->wallBracketNs = after - before;
	trace->wallUnix100ns =
	    (((UINT64)wall.dwHighDateTime << 32) | wall.dwLowDateTime) - 116444736000000000ULL;
}

static void rdpeudp_recovery_trace_init(rdpUdpTransport* udp)
{
	const char* path = getenv("RDPEUDP_RECOVERY_TRACE");
	if (!path || !*path)
		return;
	UdpRecoveryTrace* trace = calloc(1, sizeof(*trace));
	if (!trace)
	{
		WLog_ERR(TAG, "UDP recovery trace allocation failed");
		return;
	}
	trace->path = _strdup(path);
	if (!trace->path)
	{
		free(trace);
		WLog_ERR(TAG, "UDP recovery trace path allocation failed");
		return;
	}
	trace->capacity = RDPEUDP_RECOVERY_TRACE_CAPACITY;
	const char* armPath = getenv("RDPEUDP_RECOVERY_TRACE_ARM");
	if (armPath && *armPath)
	{
		trace->armPath = _strdup(armPath);
		if (!trace->armPath)
			trace->invalid = TRUE;
	}
	trace->armed = !armPath || !*armPath;
	const char* limit = getenv("RDPEUDP_RECOVERY_TRACE_LIMIT");
	if (limit && *limit)
	{
		char* end = nullptr;
		const unsigned long requested = strtoul(limit, &end, 10);
		if (end && !*end && requested > 0 && requested <= RDPEUDP_RECOVERY_TRACE_MAX_CAPACITY)
			trace->capacity = (size_t)requested;
		else
			trace->invalid = TRUE;
	}
	trace->events = calloc(trace->capacity, sizeof(*trace->events));
	if (!trace->events)
		trace->invalid = TRUE;
	trace->startNs = winpr_GetTickCount64NS();
	if (trace->armed && trace->events)
	{
		rdpeudp_recovery_clock_mapping(trace);
		trace->armNs = trace->startNs;
		UdpRecoveryEvent* arm = &trace->events[trace->count++];
		arm->timeNs = trace->armNs;
		arm->kind = UDP_RECOVERY_ARM;
		arm->expectedChannel = udp->expectedChannelSeq;
		arm->recvBase = arm->oldBase = arm->newBase = udp->recvDataBase;
		arm->bytes = (UINT32)RDPEUDP2_RECEIPT_WINDOW;
		const size_t bits = RDPEUDP2_RECEIPT_WINDOW < sizeof(arm->wire) * 8
		                        ? RDPEUDP2_RECEIPT_WINDOW
		                        : sizeof(arm->wire) * 8;
		arm->status = (UINT32)bits;
		arm->detail = (UINT32)udp->recvDataCount;
		arm->wireLength = (UINT16)((bits + 7) / 8);
	}
	udp->recoveryTrace = trace;
}

static UdpRecoveryEvent* rdpeudp_recovery_event_locked(rdpUdpTransport* udp, UINT32 kind)
{
	UdpRecoveryTrace* trace = udp->recoveryTrace;
	if (!trace || trace->flushed)
		return nullptr;
	const UINT64 now = rdpeudp_recv_now_ns(udp);
	if (!trace->armed)
	{
		if (now < trace->nextArmProbeNs)
			return nullptr;
		trace->nextArmProbeNs = now + 1000000000ULL;
		if (!trace->armPath || !PathFileExistsA(trace->armPath))
			return nullptr;
		trace->armed = TRUE;
		trace->armNs = now;
		rdpeudp_recovery_clock_mapping(trace);
		UdpRecoveryEvent* arm = rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_ARM);
		if (arm)
		{
			arm->oldBase = arm->newBase = udp->recvDataBase;
			const size_t bits = RDPEUDP2_RECEIPT_WINDOW < sizeof(arm->wire) * 8
			                        ? RDPEUDP2_RECEIPT_WINDOW
			                        : sizeof(arm->wire) * 8;
			arm->bytes = (UINT32)RDPEUDP2_RECEIPT_WINDOW;
			arm->status = (UINT32)bits;
			arm->detail = (UINT32)udp->recvDataCount;
			arm->wireLength = (UINT16)((bits + 7) / 8);
			size_t seen = 0;
			for (size_t i = 0; i < bits; i++)
			{
				if (rdpeudp_recv_data_seen_locked(udp, (UINT16)(udp->recvDataBase + i)))
				{
					arm->wire[i / 8] |= (BYTE)(1u << (i % 8));
					seen++;
				}
			}
			/* A partial initial state cannot establish later absences. */
			if (seen != udp->recvDataCount)
				trace->invalid = TRUE;
		}
		WLog_INFO(TAG, "UDP recovery trace armed at %" PRIu64 " ns", now);
	}
	if (!trace->events || trace->count >= trace->capacity)
	{
		trace->dropped++;
		trace->invalid = TRUE;
		return nullptr;
	}
	UdpRecoveryEvent* event = &trace->events[trace->count++];
	event->timeNs = rdpeudp_recv_now_ns(udp);
	event->kind = kind;
	event->expectedChannel = udp->expectedChannelSeq;
	event->recvBase = udp->recvDataBase;
	event->bufferedChunks = (UINT32)udp->recvBufferedChunks;
	event->ownedBytes = (UINT32)(udp->recvBufferedBytes + rdpeudp_recv_pending_stream_locked(udp));
	return event;
}

static BOOL rdpeudp_recovery_hash(UdpRecoveryEvent* event, const BYTE* data, size_t length)
{
	if (!event)
		return TRUE;
	BYTE digest[SHA256_DIGEST_LENGTH] = { 0 };
	if (!SHA256(data, length, digest))
		return FALSE;
	memcpy(event->hash, digest, sizeof(event->hash));
	return TRUE;
}

static UdpRecoveryEvent* rdpeudp_recovery_window_locked(rdpUdpTransport* udp, UINT32 kind,
                                                        UINT16 newBase, size_t advance)
{
	UdpRecoveryEvent* event = rdpeudp_recovery_event_locked(udp, kind);
	if (!event)
		return nullptr;
	event->oldBase = udp->recvDataBase;
	event->newBase = newBase;
	event->bytes = (UINT32)advance;
	const size_t window = RDPEUDP2_RECEIPT_WINDOW;
	for (size_t i = 0; i < advance; i++)
		event->detail +=
		    i >= window || !rdpeudp_recv_data_seen_locked(udp, (UINT16)(udp->recvDataBase + i))
		        ? 1u
		        : 0u;
	const size_t bits = window < sizeof(event->wire) * 8 ? window : sizeof(event->wire) * 8;
	event->wireLength = (UINT16)((bits + 7) / 8);
	event->status = (UINT32)bits;
	for (size_t i = 0; i < bits; i++)
	{
		if (rdpeudp_recv_data_seen_locked(udp, (UINT16)(udp->recvDataBase + i)))
			event->wire[i / 8] |= (BYTE)(1u << (i % 8));
	}
	return event;
}

static void rdpeudp_recovery_hex(FILE* file, const BYTE* bytes, size_t count)
{
	for (size_t i = 0; i < count; i++)
		(void)fprintf(file, "%02x", bytes[i]);
}

static void rdpeudp_recovery_flush_locked(rdpUdpTransport* udp, const char* reason)
{
	static const char* names[] = {
		"unknown", "rx_data", "rx_aoa",           "auto_slide", "window_progress",
		"tx_ack",  "tx_data", "channel_progress", "gap",        "failure",
		"arm"
	};
	UdpRecoveryTrace* trace = udp->recoveryTrace;
	if (!trace || trace->flushed)
		return;
	trace->flushed = TRUE;
	/* Never replace a previous diagnostic, even across a reconnect. */
	FILE* file = fopen(trace->path, "wx");
	if (!file)
	{
		WLog_ERR(TAG, "UDP recovery trace could not create %s", trace->path);
		return;
	}
	const UINT64 wallResolution =
#if defined(_WIN32)
	    16000000ULL;
#else
	    1000000ULL;
#endif
	(void)fprintf(
	    file,
	    "{\"type\":\"metadata\",\"schema_version\":1,\"clock\":\"winpr_GetTickCount64NS\","
	    "\"start_ns\":%" PRIu64 ",\"arm_ns\":%" PRIu64 ",\"armed\":%s,"
	    "\"capacity\":%zu,\"count\":%zu,\"dropped\":%zu,"
	    "\"valid\":%s,\"reason\":\"%s\",\"receive_window\":%zu,"
	    "\"hash\":\"sha256-first-8-bytes\",\"payloads_recorded\":false,"
	    "\"clock_mapping\":{\"raw_ns\":%" PRIu64 ",\"unix_100ns\":%" PRIu64
	    ",\"bracket_ns\":%" PRIu64 ",\"wall_resolution_ns\":%" PRIu64
	    ",\"max_uncertainty_ns\":%" PRIu64 "}}\n",
	    trace->startNs, trace->armNs, trace->armed ? "true" : "false", trace->capacity,
	    trace->count, trace->dropped, trace->invalid || !trace->armed ? "false" : "true", reason,
	    RDPEUDP2_RECEIPT_WINDOW, trace->wallRawNs, trace->wallUnix100ns, trace->wallBracketNs,
	    wallResolution, trace->wallBracketNs / 2 + wallResolution);
	for (size_t i = 0; i < trace->count; i++)
	{
		const UdpRecoveryEvent* event = &trace->events[i];
		const char* name = event->kind < ARRAYSIZE(names) ? names[event->kind] : names[0];
		(void)fprintf(
		    file,
		    "{\"type\":\"event\",\"seq\":%zu,\"event\":\"%s\",\"time_ns\":%" PRIu64
		    ",\"data_seq\":%" PRIu16 ",\"channel_seq\":%" PRIu16 ",\"expected_channel\":%" PRIu16
		    ",\"recv_base\":%" PRIu16 ",\"flags\":%" PRIu16 ",\"aoa\":%" PRIu16
		    ",\"old_base\":%" PRIu16 ",\"new_base\":%" PRIu16 ",\"detail\":%" PRIu32
		    ",\"status\":%" PRIu32 ",\"bytes\":%" PRIu32 ",\"buffered_chunks\":%" PRIu32
		    ",\"owned_bytes\":%" PRIu32 ",\"hash_hex\":\"",
		    i, name, event->timeNs, event->dataSeq, event->channelSeq, event->expectedChannel,
		    event->recvBase, event->flags, event->aoa, event->oldBase, event->newBase,
		    event->detail, event->status, event->bytes, event->bufferedChunks, event->ownedBytes);
		rdpeudp_recovery_hex(file, event->hash, sizeof(event->hash));
		(void)fprintf(file, "\",\"wire_hex\":\"");
		rdpeudp_recovery_hex(file, event->wire, event->wireLength);
		(void)fprintf(file, "\"}\n");
	}
	(void)fprintf(file, "{\"type\":\"footer\",\"count\":%zu,\"dropped\":%zu,\"valid\":%s}\n",
	              trace->count, trace->dropped, trace->invalid || !trace->armed ? "false" : "true");
	const BOOL ok = !ferror(file);
	if (fclose(file) != 0 || !ok)
		WLog_ERR(TAG, "UDP recovery trace export failed for %s", trace->path);
}

static void rdpeudp_recovery_failure_locked(rdpUdpTransport* udp, UINT32 reason)
{
	UdpRecoveryEvent* event = rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_FAILURE);
	if (event)
		event->detail = reason;
	rdpeudp_recovery_flush_locked(udp, "transport_failure");
}

static BOOL rdpeudp_recv_alloc_allowed_locked(rdpUdpTransport* udp)
{
	if (!udp->testRecvAllocFailureEnabled)
		return TRUE;
	if (!udp->testRecvAllocSuccesses)
		return FALSE;
	udp->testRecvAllocSuccesses--;
	return TRUE;
}

static void rdpeudp_recv_compact_stream_locked(rdpUdpTransport* udp)
{
	if (!udp->recvPos)
		return;
	const size_t pending = rdpeudp_recv_pending_stream_locked(udp);
	if (pending)
		memmove(Stream_Buffer(udp->recvStream), Stream_Buffer(udp->recvStream) + udp->recvPos,
		        pending);
	(void)Stream_SetPosition(udp->recvStream, pending);
	Stream_SealLength(udp->recvStream);
	udp->recvPos = 0;
}

static void rdpeudp_recv_release_empty_stream_locked(rdpUdpTransport* udp)
{
	if (Stream_Length(udp->recvStream) ||
	    Stream_Capacity(udp->recvStream) <= RDPEUDP2_RECV_STREAM_RESERVE)
		return;
	/* Delivery already succeeded. A best-effort reserve allocation failure
	 * must not turn successfully consumed bytes into a transport failure. */
	wStream* reserve = Stream_New(nullptr, RDPEUDP2_RECV_STREAM_RESERVE);
	if (!reserve)
		return;
	(void)Stream_SetPosition(reserve, 0);
	(void)Stream_SetLength(reserve, 0);
	Stream_Release(udp->recvStream);
	udp->recvStream = reserve;
}

static BOOL rdpeudp_recv_retain_locked(rdpUdpTransport* udp, UINT16 channel, const BYTE* data,
                                       size_t length, BOOL* ackEligible)
{
	*ackEligible = TRUE;
	const UINT16 distance = (UINT16)(channel - udp->expectedChannelSeq);
	if (length > udp->negotiatedDownMtu)
	{
		WLog_ERR(TAG, "UDP channel payload %zu exceeds receive MTU %" PRIu16 "; reconnect required",
		         length, udp->negotiatedDownMtu);
		return FALSE;
	}
	if (distance == RDPEUDP2_RECV_CHUNK_LIMIT)
	{
		/* Full channel-number recovery is ambiguous at exactly half the
		 * sequence space. Keep it unacknowledged so a later retry can enter
		 * the forward window after earlier channel bytes make progress. */
		*ackEligible = FALSE;
		return TRUE;
	}
	if (distance > RDPEUDP2_RECV_MAX_FORWARD)
		return TRUE; /* Already delivered within the backward half-space: duplicate receipt. */
	UdpRecvSlot* slot = rdpeudp_recv_slot_locked(udp, channel);
	if (slot && slot->occupied)
		return TRUE; /* Fresh DataSeq may carry the same retained channel bytes. */
	const size_t pending = rdpeudp_recv_pending_stream_locked(udp);
	const size_t byteLimit = rdpeudp_recv_byte_limit_locked(udp);
	const size_t owned = udp->recvBufferedBytes + pending;
	if (udp->recvBufferedChunks >= rdpeudp_recv_chunk_limit_locked(udp) || owned > byteLimit ||
	    length > byteLimit - owned)
	{
		WLog_ERR(TAG,
		         "UDP receive budget exhausted: chunks=%zu/%zu, bytes=%zu/%zu, span=%zu, "
		         "expected channel=%04" PRIx16 "; reconnect required",
		         udp->recvBufferedChunks, rdpeudp_recv_chunk_limit_locked(udp), owned, byteLimit,
		         rdpeudp_recv_span_locked(udp), udp->expectedChannelSeq);
		return FALSE;
	}
	const size_t pageIndex = channel >> RDPEUDP2_RECV_PAGE_SHIFT;
	UdpRecvPage* page = udp->recvPages[pageIndex];
	if (!page)
	{
		page = rdpeudp_recv_alloc_allowed_locked(udp) ? calloc(1, sizeof(*page)) : nullptr;
		if (!page)
		{
			WLog_ERR(TAG, "UDP receive page allocation failed; reconnect required");
			return FALSE;
		}
		udp->recvPages[pageIndex] = page;
		udp->recvBufferedPages++;
		udp->recvPageAllocations++;
	}
	BYTE* copy = nullptr;
	if (length)
	{
		copy = rdpeudp_recv_alloc_allowed_locked(udp) ? malloc(length) : nullptr;
		if (!copy)
		{
			if (!page->count)
			{
				free(page);
				udp->recvPages[pageIndex] = nullptr;
				udp->recvBufferedPages--;
			}
			WLog_ERR(TAG, "UDP receive payload allocation failed; reconnect required");
			return FALSE;
		}
		memcpy(copy, data, length);
	}
	slot = &page->slots[channel & (RDPEUDP2_RECV_PAGE_SIZE - 1)];
	slot->occupied = TRUE;
	slot->channelSeq = channel;
	slot->data = copy;
	slot->len = length;
	page->count++;
	if (!udp->recvBufferedChunks ||
	    distance > (UINT16)(udp->recvHighestChannelSeq - udp->expectedChannelSeq))
		udp->recvHighestChannelSeq = channel;
	udp->recvBufferedChunks++;
	udp->recvBufferedBytes += length;
	if (owned + length > udp->recvPeakOwnedBytes)
		udp->recvPeakOwnedBytes = owned + length;
	if (udp->recvBufferedChunks > udp->recvPeakBufferedChunks)
		udp->recvPeakBufferedChunks = udp->recvBufferedChunks;
	const size_t span = rdpeudp_recv_span_locked(udp);
	if (span > udp->recvPeakBufferedSpan)
		udp->recvPeakBufferedSpan = span;
	return TRUE;
}

static BOOL rdpeudp_check_stream_health(rdpUdpTransport* udp)
{
	if (!udp)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	const UINT64 now = rdpeudp_recv_now(udp);
	if (!udp->recvFailed && udp->recvGapActive &&
	    (now - udp->recvGapSince >= RDPEUDP_REASSEMBLY_TIMEOUT_MS))
	{
		WLog_ERR(TAG,
		         "UDP reassembly stalled for %" PRIu64 " ms: expected channel %04" PRIx16
		         ", buffered=%zu, DataSeq base=%04" PRIx16 "; reconnect required",
		         now - udp->recvGapSince, udp->expectedChannelSeq, udp->recvBufferedChunks,
		         udp->recvDataBase);
		udp->recvFailed = TRUE;
		rdpeudp_recovery_failure_locked(udp, 1);
	}
	const BOOL healthy = !udp->recvFailed && !udp->sendFailed;
	LeaveCriticalSection(&udp->lock);
	return healthy;
}

BOOL rdpeudp_check_health(rdpUdpTransport* udp)
{
	/* Drain queued feedback before concluding that a peer is silent. */
	return rdpeudp2_service_send(udp) && rdpeudp_check_stream_health(udp) &&
	       rdpeudp2_service_ack(udp);
}

static UINT32 rdpeudp2_initial_rto_locked(const rdpUdpTransport* udp)
{
	const UINT64 adaptive = (UINT64)udp->srttMs * 2;
	const UINT64 rto = adaptive > RDPEUDP_ACK_TIMEOUT_MS ? adaptive : RDPEUDP_ACK_TIMEOUT_MS;
	return (UINT32)(rto > RDPEUDP_MAX_RTO_MS ? RDPEUDP_MAX_RTO_MS : rto);
}

static void rdpeudp2_sample_rtt_locked(rdpUdpTransport* udp, const UdpSentEntry* entry)
{
	/* Karn's rule also excludes a positively ACKed old alias: once any retry
	 * was transmitted there is no unambiguous local departure time. */
	if (entry->retries || (entry->aliasCount != 1))
		return;
	const UINT64 elapsed = rdpeudp_recv_now(udp) - entry->sentTs;
	const UINT32 sample = (UINT32)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
	udp->srttMs = udp->srttMs ? (UINT32)(((UINT64)udp->srttMs * 7 + sample) / 8) : sample;
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
	/* cookieHash = SHA256(securityCookie) interpreted as eight 4-byte words,
	 * each transmitted in network byte order ([MS-RDPEUDP] 2.2.2.9). */
	BYTE digest[SHA256_DIGEST_LENGTH] = { 0 };
	SHA256(cookie, RDPEUDP_COOKIE_LEN, digest);
	for (int i = 0; i < 8; i++)
	{
		out[i * 4 + 0] = digest[i * 4 + 3];
		out[i * 4 + 1] = digest[i * 4 + 2];
		out[i * 4 + 2] = digest[i * 4 + 1];
		out[i * 4 + 3] = digest[i * 4 + 0];
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
	/* MS reference (Rdpeudp2ProtocolHandler) starts both seqs at 1 and sends
	 * DATA-only until data received. 0-start + spurious ACK is ignored by our
	 * own stack but Windows drops it (no ACK after SYN+ACK). */
	udp->nextDataSeq = 1;
	udp->nextChannelSeq = 1;
	udp->expectedChannelSeq = 1;
	udp->haveRecvData = FALSE;
	udp->haveSeenAoa = FALSE;
	udp->haveRealData = FALSE;
	udp->recvDataBase = 1;
	udp->overhead = 50; /* avg RDPUDP2+UDP+IP overhead estimate */
	/* DelayAck hint as observed from the MS client (max=1, timeout=20 ms);
	 * matches our prompt-ACK behavior better than the old 2/50. */
	udp->delayAckMax = 1;
	udp->delayAckTimeoutMs = 20;
	udp->peerAckMax = RDPEUDP2_DEFAULT_DELAYED_ACKS;
	udp->peerAckTimeoutMs = UINT32_MAX;
	udp->ackedSeq = (UINT16)(udp->recvDataBase - 1);
	udp->lastRecvTs = udp->lastSendTs = udp->lastKeepaliveTs = udp_now_ms();
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
	rdpeudp_recovery_trace_init(udp);
	return udp;
}

void rdpeudp_free(rdpUdpTransport* udp)
{
	if (udp && udp->connected) performance_udp_connected(udp->context, FALSE);
	if (!udp)
		return;

	EnterCriticalSection(&udp->lock);
	rdpeudp_recovery_flush_locked(udp, "teardown");
	if (udp->tunnelEstablished && udp->sockfd >= 0)
	{
		/* One receipt for an established association, including normal client
		 * shutdown. Keep packet processing free of diagnostic output. */
		WLog_INFO(TAG,
		          "UDP receive retention: peak_chunks=%zu peak_span=%zu peak_owned_bytes=%zu "
		          "page_allocations=%zu buffered_chunks=%zu buffered_bytes=%zu "
		          "unread_stream_bytes=%zu byte_limit=%zu allocated_pages=%zu recv_failed=%d",
		          udp->recvPeakBufferedChunks, udp->recvPeakBufferedSpan, udp->recvPeakOwnedBytes,
		          udp->recvPageAllocations, udp->recvBufferedChunks, udp->recvBufferedBytes,
		          rdpeudp_recv_pending_stream_locked(udp), rdpeudp_recv_byte_limit_locked(udp),
		          udp->recvBufferedPages, udp->recvFailed);
	}
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
	if (udp->sendQueue)
		Stream_Release(udp->sendQueue);
#if defined(WITH_RDP_BENCH)
	free(udp->benchQueueObserver);
	udp->benchQueueObserver = nullptr;
	rdp_bench_loss_free(udp);
#endif
	for (size_t i = 0; i < ARRAYSIZE(udp->recvPages); i++)
	{
		UdpRecvPage* page = udp->recvPages[i];
		if (!page)
			continue;
		for (size_t n = 0; n < ARRAYSIZE(page->slots); n++)
			free(page->slots[n].data);
		free(page);
	}
	if (udp->recoveryTrace)
	{
		free(udp->recoveryTrace->events);
		free(udp->recoveryTrace->armPath);
		free(udp->recoveryTrace->path);
		free(udp->recoveryTrace);
	}
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
	if ((out->flags & (RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_ACKVEC)) ==
	    (RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_ACKVEC))
		return FALSE;
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
		size_t decodedCount = 0;
		if (!rdpeudp_ackvec_count(p, need, &decodedCount))
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

/* Caller holds the transport lock through encoding and committing this immutable
 * ACK snapshot. Socket callbacks can receive another packet after that snapshot. */
static void rdpeudp2_ack_fields_locked(rdpUdpTransport* udp, UINT16 seq, size_t room,
                                       RdpUdp2Layout* out, BYTE* additions)
{
	const UINT64 now = rdpeudp_recv_now_ns(udp);
	const UINT64 arrival = udp->recvInOrderArrivalValid ? udp->recvInOrderArrivalNs : now;
	const UINT32 ts = (UINT32)((arrival / 4000ULL) & 0xFFFFFFULL);
	out->hasAck = TRUE;
	out->ackBase = seq;
	out->ackTs[0] = (BYTE)ts;
	out->ackTs[1] = (BYTE)(ts >> 8);
	out->ackTs[2] = (BYTE)(ts >> 16);
	out->ackGap = rdpeudp2_ack_gap(arrival, now);

	UINT64 gaps[RDPEUDP2_MAX_DELAYED_ACKS] = { 0 };
	size_t count = 0;
	const UINT16 acknowledged = (UINT16)(seq - udp->ackedSeq);
	size_t limit = acknowledged > 0 && acknowledged < 0x8000 ? acknowledged - 1 : 0;
	if (limit > udp->peerAckMax)
		limit = udp->peerAckMax;
	if (limit > room)
		limit = room;
	if (limit > ARRAYSIZE(gaps))
		limit = ARRAYSIZE(gaps);
	UINT16 current = seq;
	UINT64 currentArrival = arrival;
	while (udp->recvInOrderArrivalValid && count < limit)
	{
		BOOL valid = FALSE;
		const UINT16 previous = (UINT16)(current - 1);
		const UINT64 previousArrival = rdpeudp2_arrival_locked(udp, previous, &valid);
		if (!valid)
			break; /* No arrival to invent for an AckOfAcks-retired hole. */
		gaps[count++] =
		    currentArrival > previousArrival ? (currentArrival - previousArrival) / 1000ULL : 0;
		current = previous;
		currentArrival = previousArrival;
	}
	BYTE scale = 0;
	for (size_t i = 0; i < count; i++)
	{
		while (scale < 15 && (gaps[i] >> scale) > 255)
			scale++;
	}
	for (size_t i = 0; i < count; i++)
	{
		const UINT64 gap = gaps[i] >> scale;
		additions[i] = gap > 255 ? 255 : (BYTE)gap;
	}
	out->ackScale = scale;
	out->ackNumDelayed = (BYTE)count;
	out->ackDelayed = additions;
	out->ackDelayedLen = count;
}

static void rdpeudp2_ack_committed_locked(rdpUdpTransport* udp, BOOL gap, UINT16 seq)
{
	udp->ackPending = FALSE;
	udp->ackUnackedPackets = 0;
	udp->ackDueNs = 0;
	udp->ackFirstArrivalNs = 0;
	udp->ackedGap = gap;
	if (!gap)
		udp->ackedSeq = seq;
}

static wStream* rdpeudp2_build_packet_ex(rdpUdpTransport* udp, UINT16 flags, UINT16 ackBase,
                                         UINT16 ackvecBase, wStream* ackvecBody, UINT16 aoa,
                                         BYTE overhead, BYTE delayMax, UINT16 delayTimeout,
                                         UINT16 dataSeq, UINT16 channelSeq, const BYTE* payload,
                                         size_t payloadLen, BOOL dummy)
{
	WINPR_UNUSED(ackvecBase);
	RdpUdp2Layout in = WINPR_C_ARRAY_INIT;
	BYTE additions[RDPEUDP2_MAX_DELAYED_ACKS] = { 0 };
	in.flags = (UINT16)(flags & RDPUDP2_FLAGS_MASK);
	in.logWindow = (UINT8)(udp->logWindow & 0x0F);
	if (flags & RDPUDP2_FLAG_ACK)
	{
		/* Existing 100-byte payload margin also accommodates all 15 arrival gaps. */
		const size_t fixed = 3 + 7 + ((flags & RDPUDP2_FLAG_OVERHEAD) ? 1 : 0) +
		                     ((flags & RDPUDP2_FLAG_DELAYACK) ? 3 : 0) +
		                     ((flags & RDPUDP2_FLAG_AOA) ? 2 : 0) +
		                     ((flags & RDPUDP2_FLAG_DATA) ? 4 + payloadLen : 0);
		const size_t room = udp->negotiatedUpMtu > fixed ? udp->negotiatedUpMtu - fixed : 0;
		rdpeudp2_ack_fields_locked(udp, ackBase, room, &in, additions);
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
	if (flags & RDPUDP2_FLAG_ACK)
		rdpeudp2_ack_committed_locked(udp, FALSE, ackBase);
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
	EnterCriticalSection(&udp->lock);
	UINT16 aoa = udp->sendAoaValid ? udp->sendAoa : udp->nextDataSeq;
	BYTE overhead = udp->overhead;
	/* MS reference sends minimal DATA [+ACK if data received]; OVERHEAD and
	 * DELAYACK are advisory and caused interop suspicion in captures
	 * (first 2 pkts 0x0145 vs rest 0x0005, server silent). Drop opportunistic
	 * piggyback until basic DATA/ACK interops. */
	if (udp->needAoa)
		flags |= RDPUDP2_FLAG_AOA;
	wStream* packet = rdpeudp2_build_packet_ex(udp, flags, ackBase, 0, nullptr, aoa, overhead,
	                                         udp->delayAckMax, udp->delayAckTimeoutMs, dataSeq,
	                                         channelSeq, payload, payloadLen, dummy);
	LeaveCriticalSection(&udp->lock);
	return packet;
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

/** Re-arm socket readiness and the separate manual buffered/send wake.
 * POSIX socket-bound events cannot carry synthetic SetEvent notifications.
 * Reset, then recheck to retain arrivals and writes racing the reset. */
void rdpeudp_update_event(rdpUdpTransport* udp)
{
	if (!udp)
		return;
	HANDLE ev = nullptr;
	HANDLE stateEvent = nullptr;
	int fd = -1;
	EnterCriticalSection(&udp->lock);
	ev = udp->sockEvent;
	stateEvent = udp->udpEvent;
	fd = udp->sockfd;
	const BOOL hasBuffered = (Stream_Length(udp->recvStream) > udp->recvPos);
	LeaveCriticalSection(&udp->lock);
	if (ev && (ev != INVALID_HANDLE_VALUE))
		(void)WSAResetEvent(ev);
	if (!stateEvent || (stateEvent == INVALID_HANDLE_VALUE))
		return;
	(void)ResetEvent(stateEvent);
	/* Recheck: data arrived between drain and reset, or buffered TLS bytes. */
	BOOL readable = fd >= 0 && freerdp_udp_wait_readable(fd, 0);
	EnterCriticalSection(&udp->lock);
	const BOOL buffered = (Stream_Length(udp->recvStream) > udp->recvPos);
	const BOOL sendWake = udp->sendWakeGeneration != udp->sendWakeConsumed;
	LeaveCriticalSection(&udp->lock);
	if (readable || buffered || hasBuffered || sendWake)
		(void)SetEvent(stateEvent);
}

static void rdpeudp_recovery_wire_locked(rdpUdpTransport* udp, const BYTE* wire, size_t length,
                                         UdpRecoveryEvent** ackEvent, UdpRecoveryEvent** dataEvent)
{
	*ackEvent = *dataEvent = nullptr;
	if (!udp->recoveryTrace || !udp->recoveryTrace->armed || udp->recoveryTrace->flushed ||
	    !udp->connected || !udp->useUdp2)
		return;
	BYTE copy[2048];
	if (length > sizeof(copy))
	{
		udp->recoveryTrace->invalid = TRUE;
		udp->recoveryTrace->dropped++;
		return;
	}
	memcpy(copy, wire, length);
	BOOL dummy = FALSE;
	size_t offset = 0;
	if (!rdpeudp2_unprotect(copy, length, &dummy, &offset))
		return;
	const BYTE shortLength = copy[0] >> 5;
	const size_t layoutLength = shortLength < 7 ? shortLength : length - offset;
	RdpUdp2Layout layout = { 0 };
	if (!rdpeudp2_parse_layout(copy + offset, layoutLength, &layout))
		return;
	if (layout.hasAck || layout.hasAckvec)
	{
		UdpRecoveryEvent* event = rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_TX_ACK);
		*ackEvent = event;
		if (event)
		{
			event->status = 2; /* Pending until the socket send completes. */
			event->flags = layout.flags;
			event->aoa = layout.hasAoa ? layout.aoa : 0;
			event->dataSeq = layout.hasDataHeader ? layout.dataSeq : 0;
			event->channelSeq = layout.hasDataBody ? layout.channelSeq : 0;
			event->newBase = layout.hasAck
			                     ? layout.ackBase
			                     : (UINT16)(layout.ackvec[0] | ((UINT16)layout.ackvec[1] << 8));
			const BYTE* body = layout.hasAckvec ? layout.ackvec : copy + offset + 2;
			const size_t count = layout.hasAckvec ? layout.ackvecLen : 7 + layout.ackDelayedLen;
			if (count <= sizeof(event->wire))
			{
				event->wireLength = (UINT16)count;
				memcpy(event->wire, body, count);
			}
			else
				udp->recoveryTrace->invalid = TRUE;
		}
	}
	if (layout.hasDataHeader)
	{
		UdpRecoveryEvent* event = rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_TX_DATA);
		*dataEvent = event;
		if (event)
		{
			event->status = 2;
			event->flags = layout.flags;
			event->aoa = layout.hasAoa ? layout.aoa : 0;
			event->dataSeq = layout.dataSeq;
			event->channelSeq = layout.channelSeq;
			event->bytes = (UINT32)layout.dataBodyLen;
			event->detail = UINT32_MAX;
			for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
			{
				if (udp->sent[i].data && udp->sent[i].dataSeq == layout.dataSeq)
					event->detail = udp->sent[i].retries;
			}
			if (!rdpeudp_recovery_hash(event, layout.dataBody, layout.dataBodyLen))
				udp->recoveryTrace->invalid = TRUE;
		}
	}
}

static BOOL rdpeudp_udp_send_stream(rdpUdpTransport* udp, wStream* s)
{
	WINPR_ASSERT(udp);
	WINPR_ASSERT(s);
	const size_t len = Stream_Length(s);
	const BYTE* data = Stream_Buffer(s);
	if (len == 0)
		return FALSE;
	UdpRecoveryEvent* ackEvent = nullptr;
	UdpRecoveryEvent* dataEvent = nullptr;
	if (udp->recoveryTrace)
	{
		EnterCriticalSection(&udp->lock);
		rdpeudp_recovery_wire_locked(udp, data, len, &ackEvent, &dataEvent);
	}
	const SSIZE_T sent = udp->testSend ? udp->testSend(udp->testSendContext, data, len)
	                                   : freerdp_udp_send(udp->sockfd, data, len);
	if (ackEvent)
		ackEvent->status = sent == (SSIZE_T)len ? 1 : 0;
	if (dataEvent)
		dataEvent->status = sent == (SSIZE_T)len ? 1 : 0;
	if (udp->recoveryTrace)
		LeaveCriticalSection(&udp->lock);
	if (sent > 0) performance_account(udp->context, TRUE, TRUE, (size_t)sent);
	if (sent != (SSIZE_T)len)
		return FALSE;
	performance_udp_stats(udp->context, 1, 0, 0, 0);
	EnterCriticalSection(&udp->lock);
	udp->lastSendTs = rdpeudp_recv_now(udp);
	LeaveCriticalSection(&udp->lock);
	return TRUE;
}

static void rdpeudp_note_loss_report_locked(rdpUdpTransport* udp, UINT16 dseq)
{
    // A negative ACK is an observation, not proof of permanent packet loss.
    // Count it once per outstanding transmission, including across repeated ACK vectors.
    for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
    {
        if (udp->sent[i].data && udp->sent[i].dataSeq == dseq && !udp->sent[i].lossReported)
        {
            udp->sent[i].lossReported = TRUE;
            udp->stats.lostDetected++;
            performance_udp_stats(udp->context, 0, 0, 0, 1);
            break;
        }
    }
}

static void rdpeudp_note_ackvec_gaps_locked(rdpUdpTransport* udp, UINT16 base,
	                                      const BOOL* received, size_t count)
{
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		UdpSentEntry* entry = &udp->sent[i];
		if (!entry->data)
			continue;
		const size_t index = (UINT16)(entry->dataSeq - base);
		if ((index >= count) || received[index])
			continue;
		const UINT16 generated = (UINT16)(udp->nextDataSeq - entry->dataSeq);
		const BYTE previous = entry->gapPositiveCount;
		BOOL laterReceipt = FALSE;
		for (size_t j = index + 1; j < count; j++)
		{
			if (!received[j])
				continue;
			const UINT16 seq = (UINT16)(base + j);
			const UINT16 distance = (UINT16)(seq - entry->dataSeq);
			/* Only generated, later attempts inside the active sequence span
			 * can supply evidence. A future or old-wrap bit cannot do so. */
			if (!distance || (distance >= generated) || (distance >= 0x8000))
				continue;
			laterReceipt = TRUE;
			BOOL duplicate = FALSE;
			for (size_t k = 0; k < entry->gapPositiveCount; k++)
				duplicate |= entry->gapPositiveSeq[k] == seq;
			if (!duplicate && (entry->gapPositiveCount < RDPEUDP2_FAST_LOSS_EVIDENCE))
				entry->gapPositiveSeq[entry->gapPositiveCount++] = seq;
		}
		/* A bitmap's padded FALSE bits are not evidence of packet loss.
		 * Require an actual later receipt before starting either the grace
		 * timer or the diagnostic loss observation. */
		if (!laterReceipt)
			continue;
		if (!entry->gapActive)
		{
			entry->gapActive = TRUE;
			entry->gapSince = rdpeudp_recv_now(udp);
			rdpeudp_note_loss_report_locked(udp, entry->dataSeq);
		}
		if (previous != entry->gapPositiveCount)
			rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_NACK_GAP, entry->dataSeq,
			                            entry->channelSeq, entry->gapPositiveCount,
			                            entry->retries);
	}
}
/* ACKVEC bits are selective; an ACK payload is cumulative. */
static BOOL rdpeudp_sent_alias(const UdpSentEntry* entry, UINT16 seq)
{
	if (entry->dataSeq == seq)
		return TRUE;
	for (size_t i = 0; i < entry->aliasCount; i++)
	{
		if (entry->aliases[i] == seq)
			return TRUE;
	}
	return FALSE;
}

static void rdpeudp_retire_sent_locked(rdpUdpTransport* udp, UdpSentEntry* entry)
{
	if (!entry->data)
		return;
	rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_RETIRE, entry->dataSeq,
	                            entry->channelSeq, entry->len, entry->retries);
	if (udp->sendBytes >= entry->len)
		udp->sendBytes -= entry->len;
	free(entry->data);
	memset(entry, 0, sizeof(*entry));
	if (udp->sentCount > 0)
		udp->sentCount--;
}

static void rdpeudp_ack_single_locked(rdpUdpTransport* udp, UINT16 dseq)
{
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		UdpSentEntry* entry = &udp->sent[i];
		if (entry->data && rdpeudp_sent_alias(entry, dseq))
		{
			rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_ACK_POSITIVE, dseq,
			                            entry->channelSeq, entry->dataSeq, entry->retries);
			rdpeudp2_sample_rtt_locked(udp, entry);
			rdpeudp_retire_sent_locked(udp, entry);
		}
	}
}

static void rdpeudp_ack_cumulative_locked(rdpUdpTransport* udp, UINT16 base)
{
	/* Ignore acknowledgments beyond the last generated DataSeq. Modular
	 * comparisons are unambiguous inside the bounded sender window. */
	if ((INT16)(base - (UINT16)(udp->nextDataSeq - 1)) > 0)
		return;
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		UdpSentEntry* entry = &udp->sent[i];
		if (!entry->data)
			continue;
		/* An AOA can advance the peer over an old LOST attempt. A cumulative
		 * ACK covering that retired alias is not proof of channel delivery.
		 * Only the latest active attempt is eligible for cumulative receipt;
		 * explicit positive ACKVEC bits may still acknowledge older aliases. */
		const BOOL received = (INT16)(base - entry->dataSeq) >= 0;
		if (received)
		{
			rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_ACK_POSITIVE, entry->dataSeq,
			                            entry->channelSeq, base, entry->retries);
			rdpeudp2_sample_rtt_locked(udp, entry);
			rdpeudp_retire_sent_locked(udp, entry);
		}
	}
}

static void rdpeudp_slide_recv_data_locked(rdpUdpTransport* udp, size_t advance)
{
	/* A real received prefix or peer AOA owns retirement. A higher DATA
	 * sequence never permits forgetting a still-unreceived transmission. */
	for (size_t i = 0; i < advance; i++)
	{
		const UINT16 seq = (UINT16)(udp->recvDataBase + i);
		UdpRecvReceipt* receipt = &udp->recvReceipts[seq & (RDPEUDP2_RECEIPT_WINDOW - 1)];
		const BOOL seen = receipt->received && receipt->seq == seq;
		udp->recvInOrderArrivalValid = seen && receipt->arrivalValid;
		udp->recvInOrderArrivalNs = udp->recvInOrderArrivalValid ? receipt->arrivalNs : 0;
		if (!seen)
		{
			if (receipt->seq == seq)
				receipt->arrivalValid = FALSE;
			/* Missing attempts may have introduced a channel label. Preserve
			 * that evidence when deciding whether zero was omitted at wrap. */
			if (udp->recvChannelUpperBound == UINT16_MAX)
				udp->recvChannelZeroUncertain = TRUE;
			else
				udp->recvChannelUpperBound++;
		}
		else
		{
			if (receipt->channel != UINT32_MAX && receipt->channel > udp->recvChannelUpperBound)
				udp->recvChannelUpperBound = (UINT16)receipt->channel;
			receipt->received = FALSE;
			udp->recvDataCount--;
		}
	}
}

static BOOL rdpeudp_note_recv_data_seq_locked(rdpUdpTransport* udp, UINT16 dseq, UINT32 channel)
{
	UINT16 ahead = (UINT16)(dseq - udp->recvDataBase);
	if (!udp->haveRecvData && !udp->haveSeenAoa && ahead >= RDPEUDP2_INITIAL_EPOCH_THRESHOLD &&
	    ahead < RDPEUDP2_RECEIPT_WINDOW)
	{
		/* Keep the established first far-start compatibility rule independent
		 * of the new half-space window. In-window reordering still starts at 1;
		 * a peer AOA, once observed, exclusively establishes the epoch. */
		udp->recvDataBase = dseq;
		udp->ackedSeq = (UINT16)(dseq - 1);
		udp->recvInOrderArrivalValid = FALSE;
		ahead = 0;
	}
	if (ahead == RDPEUDP2_RECEIPT_WINDOW)
		return FALSE; /* Exact half-space cannot distinguish old from ahead. */
	if (ahead > RDPEUDP2_RECV_MAX_FORWARD)
		return TRUE; /* Old receipt; the current truthful prefix remains the ACK. */

	UdpRecvReceipt* receipt = &udp->recvReceipts[dseq & (RDPEUDP2_RECEIPT_WINDOW - 1)];
	if (receipt->received && receipt->seq != dseq)
	{
		WLog_ERR(TAG, "UDP receipt ring contains another active epoch; reconnect required");
		udp->recvFailed = TRUE;
		return FALSE;
	}
	if (!receipt->received)
	{
		receipt->seq = dseq;
		receipt->channel = channel;
		receipt->arrivalNs = rdpeudp_recv_now_ns(udp);
		receipt->arrivalValid = TRUE;
		receipt->received = TRUE;
		if (!udp->recvDataCount || ahead > (UINT16)(udp->recvDataHighest - udp->recvDataBase))
			udp->recvDataHighest = dseq;
		udp->recvDataCount++;
	}
	udp->haveRecvData = TRUE;
	const UINT16 oldBase = udp->recvDataBase;
	while (rdpeudp_recv_data_seen_locked(udp, udp->recvDataBase))
	{
		rdpeudp_slide_recv_data_locked(udp, 1);
		udp->recvDataBase++;
	}
	if (oldBase != udp->recvDataBase)
	{
		UdpRecoveryEvent* progress =
		    rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_WINDOW_PROGRESS);
		if (progress)
		{
			progress->oldBase = oldBase;
			progress->newBase = udp->recvDataBase;
			progress->dataSeq = dseq;
			progress->channelSeq = channel == UINT32_MAX ? 0 : (UINT16)channel;
		}
	}
	udp->lastAckSent = dseq;
	udp->stats.recvPackets++;
	return TRUE;
}

static BOOL rdpeudp_can_skip_channel_zero_locked(const rdpUdpTransport* udp)
{
	/* Some Windows peers send channel ffff -> 0001. Do not reserve zero
	 * globally: a normal peer can send it, lose it, or retransmit it with a
	 * fresh DataSeq. Infer omission only when the DataSeq window is contiguous
	 * and no discarded datagram could have introduced channel zero. An original
	 * zero would then already be buffered. Adjacent DataSeqs alone are not
	 * sufficient: they could both belong to retransmissions. */
	const UdpRecvSlot* next = rdpeudp_recv_slot_locked(udp, 1);
	if ((udp->expectedChannelSeq != 0) || udp->peerUsesChannelZero || !next || !next->occupied ||
	    (next->channelSeq != 1))
		return FALSE;
	/* An old lost datagram can be replaced by a new DataSeq carrying the same
	 * channel bytes. It must not disable a convention already proven here. */
	if (udp->peerSkipsChannelZero)
		return TRUE;
	if (udp->recvChannelZeroUncertain)
		return FALSE;
	if (udp->recvDataCount)
		return FALSE;
	return TRUE;
}

static void rdpeudp_deliver_recv_data_locked(rdpUdpTransport* udp)
{
	const UINT16 previous = udp->expectedChannelSeq;
	const BOOL gapBefore = udp->recvGapActive;
	while (TRUE)
	{
		UdpRecvSlot* slot = rdpeudp_recv_slot_locked(udp, udp->expectedChannelSeq);
		if (!slot || !slot->occupied || (slot->channelSeq != udp->expectedChannelSeq))
		{
			if (rdpeudp_can_skip_channel_zero_locked(udp))
			{
				WLog_WARN(TAG, "Peer omitted channel sequence 0 at wrap; %s, continuing at channel 1",
				          udp->peerSkipsChannelZero ? "previously confirmed peer convention"
				                                    : "no missing DataSeq could contain zero");
				udp->peerSkipsChannelZero = TRUE;
				udp->expectedChannelSeq = 1;
				continue;
			}
			/* Time and buffer pressure cannot prove that channel zero was
			 * omitted. Preserve the gap until it fills or the watchdog fails. */
			break;
		}
		rdpeudp_recv_compact_stream_locked(udp);
		const size_t pendingBytes = rdpeudp_recv_pending_stream_locked(udp);
		if (!Stream_EnsureCapacity(udp->recvStream, pendingBytes + slot->len))
		{
			WLog_ERR(TAG, "UDP receive stream allocation failed; reconnect required");
			udp->recvFailed = TRUE;
			rdpeudp_recovery_failure_locked(udp, 3);
			break;
		}
		if (udp->expectedChannelSeq == 0)
		{
			udp->peerUsesChannelZero = TRUE;
			udp->peerSkipsChannelZero = FALSE;
		}
		if (slot->len > 0)
			Stream_Write(udp->recvStream, slot->data, slot->len);
		Stream_SealLength(udp->recvStream);
		udp->recvBufferedChunks--;
		udp->recvBufferedBytes -= slot->len;
		free(slot->data);
		ZeroMemory(slot, sizeof(*slot));
		const size_t pageIndex = udp->expectedChannelSeq >> RDPEUDP2_RECV_PAGE_SHIFT;
		UdpRecvPage* page = udp->recvPages[pageIndex];
		page->count--;
		udp->expectedChannelSeq++;
		/* Reuse one empty page for ordinary in-order traffic. At a page
		 * boundary it is no longer current and can be released immediately. */
		if (!page->count && pageIndex != (udp->expectedChannelSeq >> RDPEUDP2_RECV_PAGE_SHIFT))
		{
			free(page);
			udp->recvPages[pageIndex] = nullptr;
			udp->recvBufferedPages--;
		}
	}
	/* Stream_Write advances position only; publish bytes via length. */
	Stream_SealLength(udp->recvStream);
	const BOOL pending = udp->recvBufferedChunks > 0;
	if (pending && (!udp->recvGapActive || (previous != udp->expectedChannelSeq)))
		udp->recvGapSince = rdpeudp_recv_now(udp);
	udp->recvGapActive = pending;
	if (previous != udp->expectedChannelSeq)
	{
		UdpRecoveryEvent* progress =
		    rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_CHANNEL_PROGRESS);
		if (progress)
		{
			progress->oldBase = previous;
			progress->newBase = udp->expectedChannelSeq;
		}
	}
	if (gapBefore != udp->recvGapActive || (pending && previous != udp->expectedChannelSeq))
	{
		UdpRecoveryEvent* gap = rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_GAP);
		if (gap)
			gap->detail = pending ? 1 : 0;
	}
}

/* ---- unit-test driver (no sockets; see rdpeudp.h) ----
 * rdpeudp_test_feed runs the production v2 receive block below, shared with
 * rdpeudp_recv_one's socket input. The dummy context is never dereferenced on
 * test paths (state handling and free touch no context members). */
static rdpContext g_rdpeudp_test_ctx;

rdpUdpTransport* rdpeudp_test_new(void)
{
	static const BYTE cookie[RDPEUDP_COOKIE_LEN] = { 0 };
	return rdpeudp_new(&g_rdpeudp_test_ctx, "test.invalid", 0, 0, 0, cookie);
}

void rdpeudp_test_free(rdpUdpTransport* udp)
{
	rdpeudp_free(udp);
}

BOOL rdpeudp_test_feed(rdpUdpTransport* udp, const BYTE* buf, size_t len)
{
	if (!udp || !buf || (len == 0))
		return FALSE;
	if (!rdpeudp_check_stream_health(udp))
		return TRUE; /* Failure is sticky; do not accept or ACK further data. */
	{
		BOOL dummy = FALSE;
		size_t off = 0;
		BYTE tmp[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
		if (len > sizeof(tmp))
			return TRUE; /* ignore oversize */
		memcpy(tmp, buf, len);
		if (!rdpeudp2_unprotect(tmp, len, &dummy, &off))
			return TRUE;
		/* Prefix C records the unpadded layout length for short datagrams.
		 * Padding is not ACKVEC data (or application stream content). */
		size_t layoutLen = len - off;
		const BYTE shortLen = (BYTE)(tmp[0] >> 5);
		if (shortLen < 7)
		{
			if (len != 8)
				return TRUE;
			layoutLen = shortLen;
		}

		const BYTE* p = tmp + off;
		size_t rem = layoutLen;
		if (rem < 2)
			return TRUE;
		const UINT16 header = (UINT16)(p[0] | ((UINT16)p[1] << 8));
		const UINT16 flags = (UINT16)(header & 0x0FFF);
		const BYTE peerLog = (BYTE)((header >> 12) & 0x0F);
		WINPR_UNUSED(flags);
		WINPR_UNUSED(peerLog);
		p += 2;
		rem -= 2;

		RdpUdp2Layout L = WINPR_C_ARRAY_INIT;
		if (!rdpeudp2_parse_layout(tmp + off, layoutLen, &L))
			return TRUE;

		EnterCriticalSection(&udp->lock);
		/* Malformed datagrams cannot keep a dead peer alive. */
		udp->lastRecvTs = rdpeudp_recv_now(udp);
		udp->stats.recvPackets++;
		UdpRecoveryEvent* rxRecovery =
		    L.hasDataHeader ? rdpeudp_recovery_event_locked(udp, UDP_RECOVERY_RX_DATA) : nullptr;
		if (rxRecovery)
		{
			rxRecovery->dataSeq = L.dataSeq;
			rxRecovery->channelSeq = L.hasDataBody ? L.channelSeq : 0;
			rxRecovery->flags = L.flags;
			rxRecovery->aoa = L.hasAoa ? L.aoa : 0;
			rxRecovery->bytes = (UINT32)L.dataBodyLen;
			rxRecovery->detail = dummy ? 4 : 0;
			if (!rdpeudp_recovery_hash(rxRecovery, L.dataBody, L.dataBodyLen))
				udp->recoveryTrace->invalid = TRUE;
		}
		rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_RECEIVE,
		                            L.hasDataHeader ? L.dataSeq : 0,
		                            L.hasDataBody ? L.channelSeq : 0, L.flags, 0);
		/* A peer's receive capacity is independent of our local storage bound.
		 * Windows advertises the full protocol LogWindow of 15. Retain it;
		 * the sender still clamps the effective window to local capacity. */
		if ((L.logWindow <= RDPUDP2_PROTOCOL_MAX_LOGWINDOW) && (L.logWindow >= 2))
			udp->peerLogWindow = L.logWindow;

		if (L.hasAoa)
		{
			const INT16 advance = (INT16)(L.aoa - udp->recvDataBase);
			UdpRecoveryEvent* aoaRecovery = rdpeudp_recovery_window_locked(
			    udp, UDP_RECOVERY_RX_AOA, advance > 0 ? L.aoa : udp->recvDataBase,
			    advance > 0 ? (size_t)advance : 0);
			if (aoaRecovery)
			{
				aoaRecovery->aoa = L.aoa;
				aoaRecovery->dataSeq = L.hasDataHeader ? L.dataSeq : 0;
			}
			if (advance > 0)
			{
				const BOOL zeroUncertain = udp->recvChannelZeroUncertain;
				rdpeudp_slide_recv_data_locked(udp, (size_t)advance);
				udp->recvDataBase = (UINT16)(udp->recvDataBase + advance);
				/* Initial probe numbering does not prove any channel was
				 * used. Later AOA retirement must retain real-gap evidence. */
				if (!udp->haveSeenAoa && !udp->haveRealData)
					udp->recvChannelZeroUncertain = zeroUncertain;
				while (rdpeudp_recv_data_seen_locked(udp, udp->recvDataBase))
				{
					rdpeudp_slide_recv_data_locked(udp, 1);
					udp->recvDataBase++;
				}
			}
			udp->haveSeenAoa = TRUE;
		}


		if (L.hasAck)
		{
			udp->lastAckReceived = L.ackBase;
			udp->needAoa = TRUE;
			rdpeudp_ack_cumulative_locked(udp, L.ackBase);
		}

		if (L.hasOverhead)
			udp->overhead = L.overhead;

		if (L.hasDelayAck)
		{
			udp->peerAckMax =
			    L.delayMax > RDPEUDP2_MAX_DELAYED_ACKS ? RDPEUDP2_MAX_DELAYED_ACKS : L.delayMax;
			udp->peerAckTimeoutMs = L.delayTimeout;
			/* A tighter hint must shorten an existing deadline, never postpone it. */
			if (udp->ackPending)
			{
				const UINT64 oldDue = udp->ackDueNs;
				const UINT64 due = udp->ackFirstArrivalNs + (UINT64)L.delayTimeout * 1000000ULL;
				if (due < udp->ackDueNs)
					udp->ackDueNs = due;
				if (udp->ackUnackedPackets > udp->peerAckMax)
					udp->ackDueNs = rdpeudp_recv_now_ns(udp);
				if (udp->ackDueNs < oldDue)
					udp->sendWakeGeneration++;
			}
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
				rdpeudp_note_ackvec_gaps_locked(udp, base, received, count);
				for (size_t si = 0; si < count; si++)
				{
					if (received[si])
						rdpeudp_ack_single_locked(udp, (UINT16)(base + si));
				}
				free(received);
			}
			else
			{
				/* Fall back to raw scan on parse failure (should not happen). */
				udp->needAoa = TRUE;
			}
		}

		const BOOL channelGapBefore = udp->recvGapActive;
		BOOL ackEligible =
		    !L.hasDataHeader || (UINT16)(L.dataSeq - udp->recvDataBase) != RDPEUDP2_RECEIPT_WINDOW;
		if (!ackEligible && rxRecovery)
			rxRecovery->detail = 5;
		/* DataBody (ChannelSeqNum + Data) follows ACKVEC per spec. */
		if (!dummy && L.hasDataHeader && L.hasDataBody && ackEligible)
		{
			const UINT16 cseq = L.channelSeq;
			const BYTE* dp = L.dataBody;
			const size_t drem = L.dataBodyLen;
			const UINT16 distance = (UINT16)(cseq - udp->expectedChannelSeq);
			const UdpRecvSlot* retained = rdpeudp_recv_slot_locked(udp, cseq);
			const BOOL duplicate = retained && retained->occupied;

			/* Retain channel bytes before marking their DataSeq received. A
			 * full 16-bit index cannot collide with another channel label. */
			if (!rdpeudp_recv_retain_locked(udp, cseq, dp, drem, &ackEligible))
				udp->recvFailed = TRUE;
			if (rxRecovery)
				rxRecovery->detail = udp->recvFailed                        ? 6
				                     : !ackEligible                         ? 5
				                     : distance > RDPEUDP2_RECV_MAX_FORWARD ? 3
				                     : duplicate                            ? 2
				                                                            : 1;
		}

		if (L.hasDataHeader && ackEligible && !udp->recvFailed)
		{
			/* ACK state tracks EVERY received DataSeq, including dummy
			 * (probe/keepalive) packets: dropping whole ranges (e.g. the
			 * MS 100+ probe train) otherwise wedges the cumulative base
			 * forever and every ACK references ancient history. Bodies of
			 * dummies are still never delivered, and their ACK/ACKVEC
			 * control payloads still follow normal UDP-layer processing.
			 * Every peer AOA, including ACK-only packets, advances the
			 * receive boundary monotonically with wrap-aware bitmap
			 * translation above. AOA retires transmission attempts; it
			 * does not discard still-required channel bytes. A DataSeq
			 * alone cannot prove preceding attempts need no ACK. */
			ackEligible = rdpeudp_note_recv_data_seq_locked(
			    udp, L.dataSeq, (!dummy && L.hasDataBody) ? L.channelSeq : UINT32_MAX);
			if (!dummy && ackEligible)
				udp->haveRealData = TRUE;
		}
		if (udp->recvFailed)
			rdpeudp_recovery_failure_locked(udp, 2);

		/* A duplicate or dummy can close the last DataSeq gap at wrap. */
		rdpeudp_deliver_recv_data_locked(udp);
		rdpeudp2_update_send_aoa_locked(udp);
		if (L.hasDataHeader && ackEligible && udp->haveRealData && !udp->recvFailed)
		{
			const UINT64 now = rdpeudp_recv_now_ns(udp);
			udp->recvLatestArrivalNs = now;
			udp->ackUnackedPackets++;
			if (!udp->ackPending)
			{
				const UINT32 delay =
				    udp->peerAckTimeoutMs != UINT32_MAX ? udp->peerAckTimeoutMs : udp->srttMs / 2;
				udp->ackFirstArrivalNs = now;
				udp->ackDueNs = now + (UINT64)delay * 1000000ULL;
			}
			udp->ackPending = TRUE;
			udp->sendWakeGeneration++; /* Arm a shorter deadline after socket draining. */
			/* Prompt receipt of out-of-order packets supplies selective loss evidence;
			 * closing the reported gap is equally urgent. Channel gaps are urgent too. */
			if (rdpeudp2_has_recv_gap_locked(udp) || udp->ackedGap ||
			    (!channelGapBefore && udp->recvGapActive) ||
			    udp->ackUnackedPackets > udp->peerAckMax)
				udp->ackDueNs = now;
		}
		LeaveCriticalSection(&udp->lock);
		(void)rdpeudp2_service_ack(udp);
		if (udp->udpEvent && (udp->udpEvent != INVALID_HANDLE_VALUE))
			(void)SetEvent(udp->udpEvent);
	}

	return TRUE;
}

void rdpeudp_test_set_time(rdpUdpTransport* udp, UINT64 now)
{
	if (!udp)
		return;
	EnterCriticalSection(&udp->lock);
	if (!udp->testClockEnabled)
		udp->lastRecvTs = udp->lastSendTs = udp->lastKeepaliveTs = now;
	udp->testClockEnabled = TRUE;
	udp->testClockMs = now;
	udp->testArrivalClockEnabled = FALSE;
	LeaveCriticalSection(&udp->lock);
}

void rdpeudp_test_set_time_ns(rdpUdpTransport* udp, UINT64 now)
{
	rdpeudp_test_set_time(udp, now / 1000000ULL);
	if (!udp)
		return;
	EnterCriticalSection(&udp->lock);
	udp->testArrivalClockEnabled = TRUE;
	udp->testArrivalClockNs = now;
	LeaveCriticalSection(&udp->lock);
}

UINT64 rdpeudp_test_maintenance_interval(rdpUdpTransport* udp)
{
	return rdpeudp_maintenance_interval(udp);
}

BOOL rdpeudp_test_keepalive(rdpUdpTransport* udp)
{
	return rdpeudp_send_keepalive(udp);
}

void rdpeudp_test_set_send(rdpUdpTransport* udp, RdpUdpTestSendCallback callback, void* context)
{
	if (!udp)
		return;
	/* Configure before feeding packets; the fixture is single-threaded. */
	udp->testSend = callback;
	udp->testSendContext = context;
}

BOOL rdpeudp_test_check_health(rdpUdpTransport* udp)
{
	return rdpeudp_check_health(udp);
}

BOOL rdpeudp_test_recv_state(const rdpUdpTransport* udp, RdpUdpTestRecvState* out)
{
	if (!udp || !out)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&udp->lock);
	out->recvDataBase = udp->recvDataBase;
	out->lastAckSent = udp->lastAckSent;
	out->expectedChannelSeq = udp->expectedChannelSeq;
	out->haveRecvData = udp->haveRecvData;
	out->haveSeenAoa = udp->haveSeenAoa;
	out->haveRealData = udp->haveRealData;
	out->recvStreamLen = Stream_Length(udp->recvStream);
	out->recvBufferedChunks = udp->recvBufferedChunks;
	out->recvBufferedBytes = udp->recvBufferedBytes;
	out->recvBufferedPages = udp->recvBufferedPages;
	out->recvPageAllocations = udp->recvPageAllocations;
	out->recvPendingStreamBytes = rdpeudp_recv_pending_stream_locked(udp);
	out->recvChunkLimit = rdpeudp_recv_chunk_limit_locked(udp);
	out->recvByteLimit = rdpeudp_recv_byte_limit_locked(udp);
	out->recvBufferedSpan = rdpeudp_recv_span_locked(udp);
	out->recvPeakOwnedBytes = udp->recvPeakOwnedBytes;
	out->recvPeakBufferedChunks = udp->recvPeakBufferedChunks;
	out->recvPeakBufferedSpan = udp->recvPeakBufferedSpan;
	out->recvStreamCapacity = Stream_Capacity(udp->recvStream);
	out->recvDataWindowLimit = RDPEUDP2_RECEIPT_WINDOW;
	out->recvDataCount = udp->recvDataCount;
	out->recvDataSpan = rdpeudp_recv_data_span_locked(udp);
	out->recvFailed = udp->recvFailed;
	LeaveCriticalSection((CRITICAL_SECTION*)&udp->lock);
	return TRUE;
}

BOOL rdpeudp_test_configure_recv(rdpUdpTransport* udp, size_t maxChunks, size_t maxBytes)
{
	if (!udp || maxChunks > RDPEUDP2_RECV_CHUNK_LIMIT)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	const size_t naturalBytes = (size_t)RDPEUDP2_RECV_CHUNK_LIMIT * udp->negotiatedDownMtu;
	const BOOL valid = !udp->recvBufferedChunks && !rdpeudp_recv_pending_stream_locked(udp) &&
	                   !udp->haveRealData && !udp->recvFailed && maxBytes <= naturalBytes;
	if (valid)
	{
		udp->testRecvChunkLimit = maxChunks;
		udp->testRecvByteLimit = maxBytes;
	}
	LeaveCriticalSection(&udp->lock);
	return valid;
}

void rdpeudp_test_fail_recv_allocation(rdpUdpTransport* udp, size_t afterSuccesses)
{
	if (!udp)
		return;
	EnterCriticalSection(&udp->lock);
	udp->testRecvAllocFailureEnabled = TRUE;
	udp->testRecvAllocSuccesses = afterSuccesses;
	LeaveCriticalSection(&udp->lock);
}

BOOL rdpeudp_test_recv_data(const rdpUdpTransport* udp, size_t offset, BYTE* data, size_t len)
{
	if (!udp || !data)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&udp->lock);
	const size_t available = Stream_Length(udp->recvStream);
	const BOOL valid = (offset <= available) && (len <= available - offset);
	if (valid && (len > 0))
		memcpy(data, Stream_Buffer(udp->recvStream) + offset, len);
	LeaveCriticalSection((CRITICAL_SECTION*)&udp->lock);
	return valid;
}

BOOL rdpeudp_test_seen(const rdpUdpTransport* udp, size_t i)
{
	BOOL v = FALSE;
	if (!udp)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&udp->lock);
	if (i < RDPEUDP2_RECEIPT_WINDOW)
		v = rdpeudp_recv_data_seen_locked(udp, (UINT16)(udp->recvDataBase + i));
	LeaveCriticalSection((CRITICAL_SECTION*)&udp->lock);
	return v;
}

void rdpeudp_test_set_connected(rdpUdpTransport* udp, BOOL connected)
{
	if (!udp)
		return;
	EnterCriticalSection(&udp->lock);
	udp->connected = connected;
	udp->tunnelEstablished = connected;
	LeaveCriticalSection(&udp->lock);
}

/* Returns TRUE if a datagram was received (even if ignored). */
static BOOL rdpeudp_recv_one(rdpUdpTransport* udp, DWORD timeoutMs, BOOL* haveV1SynAck,
                             RdpUdpFecHeader* v1hdr, RdpUdpSynPayload* v1syn,
                             RdpUdpSynExPayload* v1synex)
{
	BYTE buf[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
	if (timeoutMs && udp->connected && udp->useUdp2)
	{
		const UINT64 interval = rdpeudp_maintenance_interval(udp);
		const UINT64 dueMs = (interval + 999999ULL) / 1000000ULL;
		if (dueMs < timeoutMs)
			timeoutMs = dueMs ? (DWORD)dueMs : 1;
	}
	const SSIZE_T r = freerdp_udp_recv(udp->sockfd, buf, sizeof(buf), timeoutMs);
	if (r > 0) { performance_account(udp->context, TRUE, FALSE, (size_t)r); performance_udp_stats(udp->context, 0, 1, 0, 0); }
	if (r <= 0)
		return FALSE;
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

	/* Otherwise treat as v2 packet through the shared testable path. */
	(void)rdpeudp_test_feed(udp, buf, (size_t)r);

	return TRUE;
}

typedef struct
{
	const rdpUdpTransport* udp;
	UINT16 base;
} RdpUdpRecvAckVec;

static BOOL rdpeudp_ackvec_receipt_state(const void* context, size_t index)
{
	const RdpUdpRecvAckVec* window = context;
	return rdpeudp_recv_data_seen_locked(window->udp, (UINT16)(window->base + index));
}

static BOOL rdpeudp2_send_ack_locked(rdpUdpTransport* udp)
{
	/* Build the complete packet without releasing the lock. Base, bitmap,
	 * timestamps, AOA and count must describe one receive-state snapshot. */
	const BOOL gap = rdpeudp2_has_recv_gap_locked(udp);
	const UINT16 seq = (UINT16)(udp->recvDataBase - 1);
	const UINT16 aoa = udp->sendAoaValid ? udp->sendAoa : udp->nextDataSeq;
	const BOOL withOverhead = (udp->stats.ackSent % 10) == 0;
	const UINT16 flags = (UINT16)((gap ? RDPUDP2_FLAG_ACKVEC : RDPUDP2_FLAG_ACK) |
	                              (udp->needAoa ? RDPUDP2_FLAG_AOA : 0) |
	                              (withOverhead ? RDPUDP2_FLAG_OVERHEAD : 0));
	wStream* vector = nullptr;
	if (gap)
	{
		const RdpUdpRecvAckVec window = { udp, udp->recvDataBase };
		RdpUdpAckVecEncoding encoded = { 0 };
		if (!rdpeudp_encode_ackvec_states(&window, rdpeudp_ackvec_receipt_state,
		                                  rdpeudp_recv_data_span_locked(udp), &encoded))
			return FALSE;
		const UINT64 now = rdpeudp_recv_now_ns(udp);
		BOOL valid = FALSE;
		UINT64 arrival = 0;
		if (encoded.highestReceived != SIZE_MAX)
			arrival = rdpeudp2_arrival_locked(
			    udp, (UINT16)(udp->recvDataBase + encoded.highestReceived), &valid);
		/* The timestamp describes only a positive receipt represented on wire.
		 * A full coded prefix may contain only pending nodes; omit TS then. */
		const UINT32 timestamp = valid ? (UINT32)((arrival / 4000ULL) & 0xFFFFFFULL) : 0;
		vector = rdpeudp_ackvec_stream(udp->recvDataBase, &encoded, valid, timestamp,
		                               rdpeudp2_ack_gap(udp->recvLatestArrivalNs, now));
		if (!vector)
			return FALSE; /* Never fall back to a cumulative ACK over a hole. */
	}
	wStream* packet =
	    rdpeudp2_build_packet_ex(udp, flags, seq, udp->recvDataBase, vector, aoa, udp->overhead,
	                             udp->delayAckMax, udp->delayAckTimeoutMs, 0, 0, nullptr, 0, FALSE);
	if (vector)
		Stream_Release(vector);
	if (!packet)
		return FALSE;
	rdpeudp2_ack_committed_locked(udp, gap, seq);
	const BOOL ok = rdpeudp_udp_send_stream(udp, packet);
	Stream_Release(packet);
	return ok;
}

static BOOL rdpeudp2_service_ack(rdpUdpTransport* udp)
{
	if (!udp)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	BOOL ok = !udp->recvFailed && !udp->sendFailed;
	if (ok && udp->ackPending && udp->haveRealData && ((udp->sockfd >= 0) || udp->testSend) &&
	    rdpeudp_recv_now_ns(udp) >= udp->ackDueNs)
	{
		ok = rdpeudp2_send_ack_locked(udp);
		if (!ok)
			udp->sendFailed = TRUE;
	}
	LeaveCriticalSection(&udp->lock);
	return ok;
}

UINT64 rdpeudp_maintenance_interval(rdpUdpTransport* udp)
{
	if (!udp)
		return 1000000000ULL;
	EnterCriticalSection(&udp->lock);
	UINT64 interval = udp->sendBytes && !udp->sendFailed ? 20000000ULL : 1000000000ULL;
	if (udp->ackPending && !udp->recvFailed && !udp->sendFailed)
	{
		const UINT64 now = rdpeudp_recv_now_ns(udp);
		/* A positive interval keeps the timer armed; check_health services due ACKs. */
		const UINT64 remaining = udp->ackDueNs > now ? udp->ackDueNs - now : 1;
		if (remaining < interval)
			interval = remaining;
	}
	LeaveCriticalSection(&udp->lock);
	return interval;
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

		/* No standalone final ACK is sent: the healthy MS capture shows DATA
		 * following SYN+ACK directly, and this server wedges when the 12 B
		 * ACK is sent (total silence on the wire; MS-compatible order gets
		 * immediate replies). The first DATA itself acknowledges the
		 * handshake. The accept path tolerates a missing final ACK
		 * symmetrically (it also completes on first v2 DATA). */

		if (!udp->connected) performance_udp_connected(udp->context, TRUE);
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

/* One logical payload owns all its transmission aliases. The active window
 * contains only latest attempts; a lost older attempt cannot pin it forever. */
static UINT16 rdpeudp2_send_base_locked(const rdpUdpTransport* udp, size_t excluded)
{
	UINT16 base = udp->nextDataSeq;
	UINT16 distance = 0;
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		if ((i == excluded) || !udp->sent[i].data)
			continue;
		const UINT16 age = (UINT16)(udp->nextDataSeq - udp->sent[i].dataSeq);
		if (age > distance)
		{
			distance = age;
			base = udp->sent[i].dataSeq;
		}
	}
	return base;
}

static void rdpeudp2_update_send_aoa_locked(rdpUdpTransport* udp)
{
	const UINT16 base = rdpeudp2_send_base_locked(udp, ARRAYSIZE(udp->sent));
	if (!udp->sendAoaValid || ((INT16)(base - udp->sendAoa) > 0))
	{
		udp->sendAoa = base;
		udp->sendAoaValid = TRUE;
		if (base != 1 || udp->nextDataSeq != 1)
			udp->needAoa = TRUE;
	}
}

static BOOL rdpeudp2_can_allocate_seq_locked(const rdpUdpTransport* udp, size_t excluded,
                                            BOOL retransmission)
{
	size_t limit = rdpeudp2_protocol_window_locked(udp);
	if (retransmission)
	{
		/* A narrower peer window cannot revoke attempts already on the
		 * wire. Replace the oldest within their existing span so recovery
		 * can drain it; never admit new channel bytes using this allowance. */
		const UINT16 activeBase = rdpeudp2_send_base_locked(udp, ARRAYSIZE(udp->sent));
		const size_t activeSpan = (UINT16)(udp->nextDataSeq - activeBase);
		const size_t advertised = rdpeudp2_local_protocol_capacity_locked(udp);
		const size_t storage = rdpeudp2_usable_sent_storage_locked(udp);
		const size_t localLimit = advertised < storage ? advertised : storage;
		if (activeSpan > limit)
			limit = activeSpan < localLimit ? activeSpan : localLimit;
	}
	const UINT16 base = rdpeudp2_send_base_locked(udp, excluded);
	return (size_t)(UINT16)(udp->nextDataSeq - base) < limit;
}

static BOOL rdpeudp2_emit_sent_locked(rdpUdpTransport* udp, UdpSentEntry* entry,
	                                  UINT64* firstSentNs)
{
	rdpeudp2_update_send_aoa_locked(udp);
	UINT16 flags = RDPUDP2_FLAG_DATA;
	if (udp->ackPending && udp->haveRealData && !rdpeudp2_has_recv_gap_locked(udp))
		flags |= RDPUDP2_FLAG_ACK;
	wStream* packet = rdpeudp2_build_packet(udp, flags, (UINT16)(udp->recvDataBase - 1),
	                                      nullptr, 0, entry->dataSeq, entry->channelSeq,
	                                      entry->data, entry->len, FALSE);
	if (!packet)
		return FALSE;
	/* packet owns its encoded bytes before a socketless callback can ACK and
	 * free entry. Never dereference entry after invoking that callback. */
	if (firstSentNs)
	{
		*firstSentNs = rdp_bench_queue_now_locked(udp);
		rdp_bench_queue_peak_locked(udp, *firstSentNs);
	}
	const UINT16 traceDataSeq = entry->dataSeq;
	const UINT16 traceChannelSeq = entry->channelSeq;
	const size_t traceBytes = entry->len;
	const UINT32 traceRetries = entry->retries;
#if defined(WITH_RDP_BENCH)
	const size_t flightPackets = udp->sentCount;
	const size_t activeSpan =
	    (UINT16)(udp->nextDataSeq - rdpeudp2_send_base_locked(udp, ARRAYSIZE(udp->sent)));
#endif
	const BOOL result = rdpeudp_udp_send_stream(udp, packet);
	if (result)
	{
#if defined(WITH_RDP_BENCH)
		rdp_bench_flight_sent_locked(udp, flightPackets, activeSpan);
#endif
		rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_DATA_SEND, traceDataSeq,
		                            traceChannelSeq, traceBytes, traceRetries);
	}
	Stream_Release(packet);
	return result;
}

static BOOL rdpeudp2_service_send(rdpUdpTransport* udp)
{
	if (!udp)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	if (udp->sendFailed || udp->recvFailed)
		goto fail;
	if (udp->sendPumpActive || !udp->connected || !udp->useUdp2)
	{
		LeaveCriticalSection(&udp->lock);
		return TRUE;
	}
	if (udp_aborted(udp))
		goto fail;
	udp->sendPumpActive = TRUE;
	rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_PUMP_ENTER, 0, 0, udp->sendBytes, 0);
	rdp_bench_queue_peak_locked(udp, rdp_bench_queue_now_locked(udp));
	/* Consume available feedback before deciding a timer expired. No blocking
	 * socket wait occurs while holding the transport lock. */
	if (udp->sockfd >= 0)
	{
		for (size_t i = 0; i < 64 && freerdp_udp_wait_readable(udp->sockfd, 0); i++)
			(void)rdpeudp_recv_one(udp, 0, nullptr, nullptr, nullptr, nullptr);
	}
	if (!rdpeudp_check_stream_health(udp))
		goto pump_fail;
	const UINT64 now = rdpeudp_recv_now(udp);
	if (now - udp->lastRecvTs >= RDPEUDP_PEER_TIMEOUT_MS)
	{
		WLog_ERR(TAG, "UDP peer silent for %" PRIu64 " ms; reconnect required",
		         now - udp->lastRecvTs);
		goto pump_fail;
	}
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		UdpSentEntry* entry = &udp->sent[i];
		if (!entry->data)
			continue;
		const BOOL timerDue = now - entry->sentTs >= entry->rtoMs;
		const BOOL fastDue = rdp_bench_loss_fast_recovery(udp) && entry->gapActive &&
		                     (entry->gapPositiveCount >= RDPEUDP2_FAST_LOSS_EVIDENCE) &&
		                     (now - entry->gapSince >= RDPEUDP2_REORDER_GRACE_MS);
		if (!timerDue && !fastDue)
			continue;
		if (entry->retries >= RDPEUDP_MAX_RETRIES)
		{
			WLog_ERR(TAG,
			         "UDP channel %04" PRIx16 " unacknowledged after %" PRIu32
			         " retransmissions; reconnect required",
			         entry->channelSeq, entry->retries);
			goto pump_fail;
		}
		/* Retiring this old attempt opens its sequence slot, not another
	 * channel slot. Aliases still allow its delayed positive ACK. */
		if (!rdpeudp2_can_allocate_seq_locked(udp, i, TRUE))
			continue;
		rdp_bench_loss_event_locked(udp, timerDue ? RDP_BENCH_LOSS_RETRY_TIMER
		                                        : RDP_BENCH_LOSS_RETRY_FAST,
		                            entry->dataSeq, entry->channelSeq,
		                            now - entry->sentTs, entry->retries + 1);
		entry->retries++;
		/* Both timer and evidence-driven retransmissions consume one bounded
		 * attempt and back off its fallback timer. Fast recovery remains free
		 * to run before that timer when new receipts confirm another hole. */
		entry->rtoMs =
		    entry->rtoMs > RDPEUDP_MAX_RTO_MS / 2 ? RDPEUDP_MAX_RTO_MS : entry->rtoMs * 2;
		entry->dataSeq = udp->nextDataSeq++;
		entry->aliases[entry->aliasCount++] = entry->dataSeq;
		entry->sentTs = now;
		entry->lossReported = FALSE;
		entry->gapActive = FALSE;
		entry->gapSince = 0;
		entry->gapPositiveCount = 0;
		memset(entry->gapPositiveSeq, 0, sizeof(entry->gapPositiveSeq));
		udp->needAoa = TRUE;
		udp->stats.retransmits++;
		if (!rdpeudp2_emit_sent_locked(udp, entry, nullptr))
			goto pump_fail;
		performance_udp_stats(udp->context, 0, 0, 1, 0);
	}
	/* ACK-clocked, bounded pipeline. Allocation follows stream order even
	 * when concurrent writers append while another caller owns the pump. */
	for (size_t budget = 0; budget < ARRAYSIZE(udp->sent); budget++)
	{
		if (!udp->sendQueue || (udp->sendPos >= Stream_Length(udp->sendQueue)))
			break;
		if (udp->sentCount >= rdpeudp2_send_window_locked(udp) ||
		    !rdpeudp2_can_allocate_seq_locked(udp, ARRAYSIZE(udp->sent), FALSE))
			break;
		size_t slot = 0;
		while (slot < ARRAYSIZE(udp->sent) && udp->sent[slot].data)
			slot++;
		if (slot == ARRAYSIZE(udp->sent))
			goto pump_fail;
		UdpSentEntry* entry = &udp->sent[slot];
		const size_t available = Stream_Length(udp->sendQueue) - udp->sendPos;
		const size_t chunk = available < udp->maxPayload ? available : udp->maxPayload;
		if (!chunk)
			goto pump_fail;
		BYTE* copy = malloc(chunk);
		if (!copy)
			goto pump_fail;
		memcpy(copy, Stream_Buffer(udp->sendQueue) + udp->sendPos, chunk);
		memset(entry, 0, sizeof(*entry));
		entry->data = copy;
		entry->len = chunk;
		entry->channelSeq = udp->nextChannelSeq++;
		entry->dataSeq = udp->nextDataSeq++;
		entry->aliases[0] = entry->dataSeq;
		entry->aliasCount = 1;
		entry->firstSentTs = entry->sentTs = now;
		entry->rtoMs = rdpeudp2_initial_rto_locked(udp);
		udp->sentCount++;
		udp->sendPos += chunk;
		UINT64 firstSendNs = 0;
		if (!rdpeudp2_emit_sent_locked(udp, entry, &firstSendNs))
			goto pump_fail;
		rdp_bench_queue_first_send_locked(udp, chunk, firstSendNs);
	}
	if (udp->sendQueue && (udp->sendPos == Stream_Length(udp->sendQueue)))
	{
		(void)Stream_SetPosition(udp->sendQueue, 0);
		(void)Stream_SetLength(udp->sendQueue, 0);
		udp->sendPos = 0;
	}
	rdpeudp2_update_send_aoa_locked(udp);
	udp->sendPumpActive = FALSE;
	rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_PUMP_EXIT, 0, 0, udp->sendBytes, 0);
	LeaveCriticalSection(&udp->lock);
	return rdpeudp2_service_ack(udp);

pump_fail:
	udp->sendPumpActive = FALSE;
	rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_PUMP_EXIT, 0, 0, udp->sendBytes, 0);
fail:
	udp->sendFailed = TRUE;
	rdpeudp_recovery_failure_locked(udp, 4);
	LeaveCriticalSection(&udp->lock);
	return FALSE;
}

/* -2 means capacity refused before accepting any bytes. An accepted write is
 * never retried by the upper layer, including after a later socket failure. */
static SSIZE_T rdpeudp2_enqueue(rdpUdpTransport* udp, const BYTE* data, size_t len,
	                           UINT64* admittedNs)
{
	if (!udp || !data || !len || len > 60000)
		return -1;
	EnterCriticalSection(&udp->lock);
	if (!udp->connected || !udp->useUdp2 || udp->sendFailed || udp->recvFailed || udp_aborted(udp))
	{
		udp->sendFailed = TRUE;
		LeaveCriticalSection(&udp->lock);
		return -1;
	}
	/* Normal builds own at most 32 KiB. A single larger valid BIO
	 * write can be accepted atomically only after preceding bytes drain. */
	const size_t queueLimit = rdpeudp2_send_queue_limit(udp);
	const size_t limit = len > queueLimit ? len : queueLimit;
	if (udp->sendBytes > limit - len)
	{
		rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_ENQUEUE_BLOCK, 0, 0, len, 0);
		LeaveCriticalSection(&udp->lock);
		return -2;
	}
	if (!udp->sendQueue)
	{
		udp->sendQueue = Stream_New(nullptr, 65536);
		if (!udp->sendQueue)
		{
			udp->sendFailed = TRUE;
			LeaveCriticalSection(&udp->lock);
			return -1;
		}
		(void)Stream_SetLength(udp->sendQueue, 0);
	}
	const size_t pending = Stream_Length(udp->sendQueue) - udp->sendPos;
	if (pending)
		memmove(Stream_Buffer(udp->sendQueue), Stream_Buffer(udp->sendQueue) + udp->sendPos, pending);
	(void)Stream_SetPosition(udp->sendQueue, pending);
	(void)Stream_SetLength(udp->sendQueue, pending);
	udp->sendPos = 0;
	if (!Stream_EnsureRemainingCapacity(udp->sendQueue, len))
	{
		udp->sendFailed = TRUE;
		LeaveCriticalSection(&udp->lock);
		return -1;
	}
	Stream_Write(udp->sendQueue, data, len);
	Stream_SealLength(udp->sendQueue);
	udp->sendBytes += len;
	rdp_bench_loss_event_locked(udp, RDP_BENCH_LOSS_ENQUEUE_ACCEPT, 0, 0, len, 0);
	udp->sendWakeGeneration++;
	const UINT64 accepted = rdp_bench_queue_now_locked(udp);
	rdp_bench_queue_accepted_locked(udp, len, accepted);
	rdp_bench_queue_peak_locked(udp, accepted);
	if (admittedNs)
		*admittedNs = accepted;
	LeaveCriticalSection(&udp->lock);
	return (SSIZE_T)len;
}

static SSIZE_T rdpeudp2_send_reliable(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	if (!udp || !data || !len || len > 60000)
		return -1;
	rdp_bench_loss_event(udp, RDP_BENCH_LOSS_ADMISSION_BEGIN, 0, 0, len, 0);
	UINT64 admittedNs = 0;
#if defined(WITH_RDP_BENCH)
	EnterCriticalSection(&udp->lock);
	const UINT64 callStartNs = rdp_bench_queue_now_locked(udp);
	LeaveCriticalSection(&udp->lock);
	BOOL capacityBlocked = FALSE;
#endif
	SSIZE_T result = -2;
	while ((result = rdpeudp2_enqueue(udp, data, len, &admittedNs)) == -2)
	{
#if defined(WITH_RDP_BENCH)
		capacityBlocked = TRUE;
#endif
		if (!rdpeudp2_service_send(udp) || !rdpeudp2_service_ack(udp))
			goto fail;
		rdp_bench_loss_event(udp, RDP_BENCH_LOSS_WAIT_BEGIN, 0, 0, 10, 0);
		(void)rdpeudp_recv_one(udp, 10, nullptr, nullptr, nullptr, nullptr);
		rdp_bench_loss_event(udp, RDP_BENCH_LOSS_WAIT_END, 0, 0, 10, 0);
	}
#if defined(WITH_RDP_BENCH)
	if (result > 0)
	{
		EnterCriticalSection(&udp->lock);
		rdp_bench_queue_admission_locked(udp, callStartNs, admittedNs, capacityBlocked);
		LeaveCriticalSection(&udp->lock);
	}
#endif
	if (result < 0 || !rdpeudp2_service_send(udp) || !rdpeudp2_service_ack(udp))
		goto fail;
	rdp_bench_loss_event(udp, RDP_BENCH_LOSS_ADMISSION_END, 0, 0, len, 0);
	/* TLS handshake waits on socket readiness before the steady-state timer
	 * exists. Drain setup writes synchronously, but with the same pipeline. */
	if (!udp->tunnelEstablished)
	{
		while (TRUE)
		{
			EnterCriticalSection(&udp->lock);
			const BOOL pending = udp->sendBytes > 0;
			LeaveCriticalSection(&udp->lock);
			if (!pending)
				break;
			if (!rdpeudp2_service_send(udp) || !rdpeudp2_service_ack(udp))
				goto fail;
			(void)rdpeudp_recv_one(udp, 10, nullptr, nullptr, nullptr, nullptr);
		}
	}
	/* Wake the main loop so it can arm active-send maintenance even when
	 * every first datagram/ACK is lost and no socket read event arrives. */
	if (udp->tunnelEstablished && rdpeudp_has_pending_send(udp))
	{
		if (udp->udpEvent)
			(void)SetEvent(udp->udpEvent);
	}
	return result;
fail:
	EnterCriticalSection(&udp->lock);
	udp->sendFailed = TRUE;
	LeaveCriticalSection(&udp->lock);
	return -1;
}

BOOL rdpeudp_test_configure_send(rdpUdpTransport* udp, UINT16 payload, BYTE logWindow,
                                 BYTE peerLogWindow, UINT16 nextDataSeq, UINT16 nextChannelSeq)
{
	if (!udp || !payload || payload > RDPUDP2_MAX_PAYLOAD || logWindow < 2 ||
	    logWindow > RDPUDP2_LOCAL_MAX_LOGWINDOW || peerLogWindow < 2 ||
	    peerLogWindow > RDPUDP2_PROTOCOL_MAX_LOGWINDOW)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	const BOOL empty = !udp->sendBytes && !udp->sentCount;
	if (empty)
	{
		udp->connected = udp->useUdp2 = udp->tunnelEstablished = TRUE;
		udp->maxPayload = payload;
		udp->logWindow = logWindow;
		udp->peerLogWindow = peerLogWindow;
		udp->nextDataSeq = nextDataSeq;
		udp->nextChannelSeq = nextChannelSeq;
		udp->sendAoa = nextDataSeq;
		udp->sendAoaValid = TRUE;
	}
	LeaveCriticalSection(&udp->lock);
	return empty;
}

SSIZE_T rdpeudp_test_send_data(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	const SSIZE_T result = rdpeudp2_enqueue(udp, data, len, nullptr);
	return result > 0 && !rdpeudp2_service_send(udp) ? -1 : result;
}

#if defined(WITH_RDP_BENCH)
SSIZE_T rdpeudp_test_queue_enqueue(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	return rdpeudp2_enqueue(udp, data, len, nullptr);
}
#endif

SSIZE_T rdpeudp_test_write_data(rdpUdpTransport* udp, const BYTE* data, size_t len)
{
	return rdpeudp2_send_reliable(udp, data, len);
}

BOOL rdpeudp_test_attach_socket(rdpUdpTransport* udp, int fd)
{
	if (!udp || fd < 0 || udp->sockfd >= 0)
		return FALSE;
	if (!udp->udpEvent)
		udp->udpEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!udp->udpEvent)
		return FALSE;
	udp->sockfd = fd;
	return rdpeudp_attach_socket_event(udp);
}

BOOL rdpeudp_test_service_send(rdpUdpTransport* udp)
{
	return rdpeudp2_service_send(udp);
}

BOOL rdpeudp_has_pending_send(rdpUdpTransport* udp)
{
	return rdpeudp_send_snapshot(udp, nullptr);
}

BOOL rdpeudp_send_snapshot(rdpUdpTransport* udp, UINT64* generation)
{
	if (!udp)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	const BOOL pending = udp->sendBytes > 0 && !udp->sendFailed && !udp->recvFailed;
	if (generation)
		*generation = udp->sendWakeGeneration;
	LeaveCriticalSection(&udp->lock);
	return pending;
}

void rdpeudp_consume_send_wake(rdpUdpTransport* udp, UINT64 generation)
{
	if (!udp)
		return;
	EnterCriticalSection(&udp->lock);
	udp->sendWakeConsumed = generation;
	LeaveCriticalSection(&udp->lock);
}

BOOL rdpeudp_test_send_snapshot(rdpUdpTransport* udp, UINT64* generation)
{
	return rdpeudp_send_snapshot(udp, generation);
}

void rdpeudp_test_consume_send_wake(rdpUdpTransport* udp, UINT64 generation)
{
	rdpeudp_consume_send_wake(udp, generation);
}

void rdpeudp_test_update_event(rdpUdpTransport* udp)
{
	rdpeudp_update_event(udp);
}

void rdpeudp_test_get_events(rdpUdpTransport* udp, HANDLE* socketEvent, HANDLE* stateEvent)
{
	if (socketEvent)
		*socketEvent = rdpeudp_get_event(udp);
	if (stateEvent)
		*stateEvent = rdpeudp_get_state_event(udp);
}

BOOL rdpeudp_test_send_state(rdpUdpTransport* udp, RdpUdpTestSendState* state)
{
	if (!udp || !state)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	memset(state, 0, sizeof(*state));
	state->queuedBytes = udp->sendBytes;
	state->inFlight = udp->sentCount;
	state->nextDataSeq = udp->nextDataSeq;
	state->nextChannelSeq = udp->nextChannelSeq;
	state->aoa = udp->sendAoa;
	state->sendFailed = udp->sendFailed;
	state->localLogWindow = udp->logWindow;
	state->peerLogWindow = udp->peerLogWindow;
	state->sendWindow = rdpeudp2_send_window_locked(udp);
	state->lossReports = udp->stats.lostDetected;
	state->srttMs = udp->srttMs;
	state->initialRtoMs = rdpeudp2_initial_rto_locked(udp);
	state->lastRecvTs = udp->lastRecvTs;
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		if (!udp->sent[i].data)
			continue;
		state->aliasCount += udp->sent[i].aliasCount;
		if (udp->sent[i].rtoMs > state->maxRtoMs)
			state->maxRtoMs = udp->sent[i].rtoMs;
		if (udp->sent[i].retries > state->maxRetries)
			state->maxRetries = udp->sent[i].retries;
	}
	LeaveCriticalSection(&udp->lock);
	return TRUE;
}

void rdpeudp_test_set_abort(rdpUdpTransport* udp, HANDLE event)
{
	rdpeudp_set_abort_event(udp, event);
}

static SSIZE_T rdpeudp2_recv_reliable(rdpUdpTransport* udp, BYTE* buffer, size_t len,
                                      DWORD timeoutMs)
{
	WINPR_ASSERT(udp);
	if (!udp->connected || !rdpeudp2_service_send(udp))
		return -1;

	const UINT64 deadline = udp_now_ms() + timeoutMs;
	while (TRUE)
	{
		if (udp_aborted(udp) || !rdpeudp_check_health(udp))
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
				rdpeudp_recv_release_empty_stream_locked(udp);
			}
			LeaveCriticalSection(&udp->lock);
			return (SSIZE_T)copy;
		}
		LeaveCriticalSection(&udp->lock);

		/* Even a nonblocking read must inspect bytes already reassembled.
		 * Once empty, return WANT_READ without waiting inside the TLS lock. */
		const UINT64 now = udp_now_ms();
		if (now >= deadline)
			break;
		DWORD step = (DWORD)(deadline - now);
		if (step > 50)
			step = 50;
		(void)rdpeudp_recv_one(udp, step, nullptr, nullptr, nullptr, nullptr);
	}

	errno = EAGAIN;
	return 0;
}

SSIZE_T rdpeudp_test_read_data(rdpUdpTransport* udp, BYTE* data, size_t length)
{
	if (!udp || !data || !length)
		return -1;
	return rdpeudp2_recv_reliable(udp, data, length, 0);
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
	BIO_clear_retry_flags(bio);
	const SSIZE_T rc =
	    rdpeudp2_send_reliable(ptr->udp, (const BYTE*)buf, (size_t)size);
	/* Writes are copied before acceptance. Failure after acceptance is
	 * terminal: repeating this BIO write could duplicate TLS bytes. */
	return rc > 0 ? (int)rc : -1;
}

static int rdpeudp_bio_read(BIO* bio, char* buf, int size)
{
	RdpUdpBio* ptr = (RdpUdpBio*)BIO_get_data(bio);
	if (!ptr || !ptr->udp || !buf || (size <= 0))
		return 0;
	BIO_clear_flags(bio, BIO_FLAGS_READ);
	/* Setup retains its bounded synchronous reads. Established transport
	 * reads are nonblocking so a pending TLS record can be drained without
	 * waiting for another datagram while holding the TLS BIO's lock. */
	EnterCriticalSection(&ptr->udp->lock);
	const DWORD timeout = ptr->udp->tunnelEstablished ? 0 : 100;
	LeaveCriticalSection(&ptr->udp->lock);
	const SSIZE_T rc = rdpeudp2_recv_reliable(ptr->udp, (BYTE*)buf, (size_t)size, timeout);
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
			/* Return the transport socket-readiness event, NOT the BIO's
			 * private hEvent (created never signaled, never bound: TLS
			 * setup (pollAndHandshake) waits on this event INFINITE, so a
			 * dead handle freezes the handshake mid-flight once early APC
			 * luck runs out — observed as zero parses despite arrivals).
			 * sockEvent is WSA-bound at connect/accept time and tracks
			 * socket readability. Steady-state mainloop polling includes
			 * the separate manual wake for buffered bytes and queued writes.
			 * Same pattern as the TCP BIO (tcp.c). */
			{
				HANDLE ev = nullptr;
				if (ptr->udp)
				{
					EnterCriticalSection(&ptr->udp->lock);
					ev = ptr->udp->sockEvent;
					LeaveCriticalSection(&ptr->udp->lock);
				}
				if (!ev || (ev == INVALID_HANDLE_VALUE))
					ev = ptr->hEvent; /* pre-connect fallback */
				*((HANDLE*)arg2) = ev;
			}
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

BIO* rdpeudp_test_new_bio(rdpUdpTransport* udp)
{
	if (!udp)
		return nullptr;
	BIO* bio = BIO_new(BIO_s_rdpeudp());
	if (bio)
	{
		RdpUdpBio* ptr = BIO_get_data(bio);
		ptr->udp = udp;
	}
	return bio;
}

BOOL rdpeudp_test_attach_tls_bio(rdpUdpTransport* udp, BIO* bio)
{
	if (!udp || !bio || udp->tls || !udp->connected)
		return FALSE;
	rdpTls* tls = calloc(1, sizeof(*tls));
	if (!tls)
		return FALSE;
	tls->bio = bio;
	udp->tls = tls;
	udp->tlsBio = bio;
	return TRUE;
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
	if (!udp || !udp->tunnelEstablished || !rdpeudp_check_health(udp))
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

		/* 3. Always attempt the established nonblocking BIO read. OpenSSL
		 * may own complete records after the UDP byte stream and socket are
		 * empty; checking only those lower layers strands batched DVC PDUs.
		 * WANT_READ ends this drain immediately, including partial records. */
		{
			UINT64 now = udp_now_ms();
			DWORD chunkWait = 0;
			if (deadline > now)
			{
				UINT64 left = deadline - now;
				chunkWait = (left > 50) ? 50 : (DWORD)left;
			}
			/* tls_recv_some makes one nonblocking established BIO_read. */
			const SSIZE_T got = tls_recv_some(udp, tmp, sizeof(tmp));
			if (got < 0)
				return -1;
			if (got == 0)
			{
				if (udp_now_ms() >= deadline)
					return 0;
				/* Blocking callers wait outside the TLS BIO lock. The ordinary
				 * event-loop drain returns after one WANT_READ result. */
				if (timeoutMs == 0)
					return 0;
				int fd = -1;
				EnterCriticalSection(&udp->lock);
				fd = udp->sockfd;
				LeaveCriticalSection(&udp->lock);
				if (fd < 0)
					return 0;
				if (chunkWait > 0)
					(void)freerdp_udp_wait_readable(fd, chunkWait);
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

int rdpeudp_test_tunnel_recv_full(rdpUdpTransport* udp, BYTE* subBuf, size_t subBufLen,
	                             size_t* subLenOut, BYTE* payloadBuf, size_t payloadBufLen,
	                             size_t* payloadLenOut, DWORD timeoutMs)
{
	return rdpeudp_tunnel_recv_full(udp, subBuf, subBufLen, subLenOut, payloadBuf, payloadBufLen,
	                               payloadLenOut, timeoutMs);
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

void rdpeudp_test_set_context(rdpUdpTransport* udp, rdpContext* context)
{
    if (udp && context) udp->context = context;
}
BOOL rdpeudp_test_seed_sent(rdpUdpTransport* udp, UINT16 dataSeq, UINT16 channelSeq)
{
    if (!udp) return FALSE;
    EnterCriticalSection(&udp->lock);
    for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
    {
        if (!udp->sent[i].data)
        {
            BYTE* data = calloc(1, 1);
            if (!data) { LeaveCriticalSection(&udp->lock); return FALSE; }
            memset(&udp->sent[i], 0, sizeof(udp->sent[i]));
            udp->sent[i].data = data;
            udp->sent[i].len = 1;
            udp->sent[i].dataSeq = dataSeq;
            udp->sent[i].channelSeq = channelSeq;
            udp->sent[i].aliases[0] = dataSeq;
            udp->sent[i].aliasCount = 1;
            udp->sent[i].firstSentTs = udp->sent[i].sentTs = rdpeudp_recv_now(udp);
			udp->sent[i].rtoMs = rdpeudp2_initial_rto_locked(udp);
			udp->nextDataSeq = (UINT16)(dataSeq + 1);
			udp->sendBytes++;
			udp->sentCount++;
            LeaveCriticalSection(&udp->lock);
            return TRUE;
        }
    }
    LeaveCriticalSection(&udp->lock);
    return FALSE;
}
BOOL rdpeudp_test_get_stats(const rdpUdpTransport* udp, RdpUdpStats* stats)
{
    return rdpeudp_get_stats(udp, stats);
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

HANDLE rdpeudp_get_state_event(rdpUdpTransport* udp)
{
	if (!udp)
		return nullptr;
	EnterCriticalSection(&udp->lock);
	const HANDLE ev = udp->udpEvent;
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
	/* An idle peer still needs liveness before its first real DATA. Send a
	 * dummy AOA control without consuming DataSeq or inventing an ACK(0). */
	if (!udp->haveRealData)
	{
		const UINT16 aoa = udp->sendAoaValid ? udp->sendAoa : udp->nextDataSeq;
		wStream* packet = rdpeudp2_build_packet_ex(udp, RDPUDP2_FLAG_AOA, 0, 0, nullptr, aoa, 0, 0,
		                                           0, 0, 0, nullptr, 0, TRUE);
		const BOOL ok = packet && rdpeudp_udp_send_stream(udp, packet);
		if (packet)
			Stream_Release(packet);
		if (ok)
			udp->lastKeepaliveTs = rdpeudp_recv_now(udp);
		LeaveCriticalSection(&udp->lock);
		return ok;
	}
	const BOOL rc = rdpeudp2_send_ack_locked(udp);
	if (rc)
		udp->lastKeepaliveTs = rdpeudp_recv_now(udp);
	LeaveCriticalSection(&udp->lock);
	if (rc)
		WLog_DBG(TAG, "RDP-UDP keepalive sent");
	return rc;
}

BOOL rdpeudp_check_keepalive(rdpUdpTransport* udp, DWORD idleMs)
{
	if (!udp || !udp->connected || !rdpeudp_check_health(udp))
		return FALSE;
	const UINT64 now = rdpeudp_recv_now(udp);
	BOOL need = FALSE;
	EnterCriticalSection(&udp->lock);
	if (udp->useUdp2 && (idleMs > RDPEUDP_KEEPALIVE_MS))
		idleMs = RDPEUDP_KEEPALIVE_MS;
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
	/* V1/V2: same fixed 1-start as client (was zeroed by calloc). */
	udp->nextDataSeq = 1;
	udp->nextChannelSeq = 1;
	udp->expectedChannelSeq = 1;
	udp->haveRecvData = FALSE;
	udp->haveSeenAoa = FALSE;
	udp->haveRealData = FALSE;
	udp->recvDataBase = 1;
	udp->overhead = 50;
	/* Same DelayAck hint as client (MS-observed max=1, timeout=20 ms). */
	udp->delayAckMax = 1;
	udp->delayAckTimeoutMs = 20;
	udp->peerAckMax = RDPEUDP2_DEFAULT_DELAYED_ACKS;
	udp->peerAckTimeoutMs = UINT32_MAX;
	udp->ackedSeq = (UINT16)(udp->recvDataBase - 1);
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
	rdpeudp_recovery_trace_init(udp);

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

	/* 3. Wait for handshake completion: standalone final ACK (v1 ACK, no
	 * SYN), or first v2 DATA for MS-compatible peers that skip the ACK
	 * (N2). Peek to classify, consume only validated non-DATA: consuming
	 * a queued DATA into buf and dropping it on v1-parse failure (plus
	 * losing the race on every retransmit) wedges this wait even though
	 * valid DATA arrived. A completing v2 DATA datagram itself always
	 * stays queued for TLS to read normally. Anything else at queue head
	 * (P1: dummy probe, duplicate, ACK-only, unclassifiable) is drained
	 * so it cannot head-block a completing packet behind it. Draining
	 * never advances ACK state, so even a misdrained DATA is simply
	 * retransmitted by the peer under a new DataSeq; wedging is worse. */
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
			if (freerdp_udp_wait_readable(udp->sockfd, 100))
			{
				/* Peek wide enough for non-minimal first-DATA headers
				 * (ACKVEC etc.); truncation must bias toward leaving
				 * the datagram queued, never toward draining DATA. */
				BYTE peek[512] = { 0 };
				const SSIZE_T pr = recv(udp->sockfd, (char*)peek, sizeof(peek),
				                        MSG_PEEK);
				if (pr > 8)
				{
					/* v1 final ACK? Parse the peeked bytes without
					 * consuming them. */
					RdpUdpFecHeader h = { 0 };
					wStream sb = { 0 };
					wStream* s = Stream_StaticConstInit(&sb, peek, (size_t)pr);
					if (s && rdpeudp_read_fec_header(s, &h) &&
					    (h.flags & RDPUDP_FLAG_ACK) && !(h.flags & RDPUDP_FLAG_SYN) &&
					    (h.snSourceAck == udp->initialSeq))
					{
						/* Consume the ACK now that it is validated. */
						BYTE drop[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
						(void)freerdp_udp_recv(udp->sockfd, drop, sizeof(drop), 0);
						ok = TRUE;
						break;
					}
					/* v2 DATA (peers that skip the standalone ACK)?
					 * The datagram itself stays queued for TLS.
					 * NOTE: pass wire bytes straight to unprotect (it undoes
					 * the byte 0/7 swap itself); pre-swapping here would
					 * double-swap back to an invalid prefix. */
					{
						const size_t plen = ((size_t)pr < sizeof(peek))
						                        ? (size_t)pr
						                        : sizeof(peek);
						BOOL dummy = FALSE;
						size_t poff = 0;
						RdpUdp2Layout pl = WINPR_C_ARRAY_INIT;
						if (rdpeudp2_unprotect(peek, plen, &dummy, &poff) && !dummy &&
						    rdpeudp2_parse_layout(peek + poff, plen - poff, &pl) &&
						    pl.hasDataHeader)
						{
							ok = TRUE;
							break;
						}
					}
				}
				/* P1: queue head does not complete the handshake — drain
				 * one datagram and keep waiting for the completer behind
				 * it. Each drain strictly shrinks the queue, so this
				 * always makes progress within the deadline. */
				{
					BYTE drop[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
					(void)freerdp_udp_recv(udp->sockfd, drop, sizeof(drop), 0);
				}
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
	if (!udp->connected) performance_udp_connected(udp->context, TRUE);
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

/* Private adapter shares the production sender, receiver and socket path. */
#if defined(WITH_RDP_BENCH)
#include "rdp_bench_private.inc"
#endif
