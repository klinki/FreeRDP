/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP-UDP unit tests ([MS-RDPEUDP], [MS-RDPEUDP2], [MS-RDPEMT])
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

#include <stdio.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/stream.h>

#include "../rdpeudp.h"

static int test_fec_header(void)
{
	RdpUdpFecHeader h = { 0x12345678, 128, RDPUDP_FLAG_SYN | RDPUDP_FLAG_SYNEX };
	wStream* s = Stream_New(nullptr, 16);
	if (!s)
		return -1;
	if (!rdpeudp_write_fec_header(s, &h))
	{
		Stream_Release(s);
		return -1;
	}
	Stream_SealLength(s);
	Stream_SetPosition(s, 0);

	RdpUdpFecHeader out = { 0 };
	if (!rdpeudp_read_fec_header(s, &out))
	{
		Stream_Release(s);
		return -1;
	}
	Stream_Release(s);

	if ((out.snSourceAck != h.snSourceAck) || (out.receiveWindow != h.receiveWindow) ||
	    (out.flags != h.flags))
	{
		(void)fprintf(stderr, "fec header mismatch\n");
		return -1;
	}
	return 0;
}

static int test_syn_roundtrip(void)
{
	BYTE correlation[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	BYTE cookie[16] = { 0 };
	for (int i = 0; i < 16; i++)
		cookie[i] = (BYTE)i;
	BYTE hash[32] = { 0 };
	for (int i = 0; i < 32; i++)
		hash[i] = (BYTE)(0xA0 + i);

	wStream* s =
	    rdpeudp_build_syn(0xAABBCCDD, 1232, 1232, 0, FALSE, correlation,
	                      RDPUDP_PROTOCOL_VERSION_3, hash);
	if (!s)
		return -1;

	const size_t len = Stream_Length(s);
	BYTE* buf = Stream_Buffer(s);

	RdpUdpFecHeader h = { 0 };
	RdpUdpSynPayload syn = { 0 };
	BYTE corrOut[16] = { 0 };
	RdpUdpSynExPayload ex = { 0 };
	const BOOL rc = rdpeudp_parse_syn(buf, len, &h, &syn, corrOut, &ex);
	Stream_Release(s);
	if (!rc)
	{
		(void)fprintf(stderr, "syn parse failed\n");
		return -1;
	}
	if (syn.initialSeq != 0xAABBCCDD)
	{
		(void)fprintf(stderr, "syn seq mismatch\n");
		return -1;
	}
	if ((syn.upstreamMtu != 1232) || (syn.downstreamMtu != 1232))
	{
		(void)fprintf(stderr, "syn mtu mismatch\n");
		return -1;
	}
	if (memcmp(corrOut, correlation, 16) != 0)
	{
		(void)fprintf(stderr, "correlation mismatch\n");
		return -1;
	}
	if (!(h.flags & RDPUDP_FLAG_SYN) || !(h.flags & RDPUDP_FLAG_SYNEX) ||
	    !(h.flags & RDPUDP_FLAG_CORRELATIONID))
	{
		(void)fprintf(stderr, "syn flags mismatch 0x%04x\n", h.flags);
		return -1;
	}
	if (!(ex.synexFlags & RDPUDP_SYNEX_VERSION_VALID) ||
	    (ex.version != RDPUDP_PROTOCOL_VERSION_3) || !ex.haveCookieHash)
	{
		(void)fprintf(stderr, "synex mismatch\n");
		return -1;
	}
	if (memcmp(ex.cookieHash, hash, 32) != 0)
	{
		(void)fprintf(stderr, "cookie hash mismatch\n");
		return -1;
	}
	return 0;
}

static int test_v2_protect(void)
{
	/* Build a minimal v2 layout: header(2) + ACK(7) */
	wStream* s = Stream_New(nullptr, 32);
	if (!s)
		return -1;
	Stream_Write_UINT16(s, 0x5001); /* logWindow=5, flags=ACK */
	Stream_Write_UINT16(s, 0x1234);
	Stream_Write_UINT8(s, 0x01);
	Stream_Write_UINT8(s, 0x02);
	Stream_Write_UINT8(s, 0x03);
	Stream_Write_UINT8(s, 0x00);
	Stream_Write_UINT8(s, 0x00);

	const size_t layoutLen = Stream_GetPosition(s);
	BYTE layout[32] = { 0 };
	memcpy(layout, Stream_Buffer(s), layoutLen);

	if (!rdpeudp2_protect(s, FALSE))
	{
		Stream_Release(s);
		return -1;
	}
	const size_t wireLen = Stream_Length(s);
	BYTE* wire = Stream_Buffer(s);
	BYTE tmp[64] = { 0 };
	if (wireLen > sizeof(tmp))
	{
		Stream_Release(s);
		return -1;
	}
	memcpy(tmp, wire, wireLen);

	BOOL dummy = TRUE;
	size_t off = 0;
	if (!rdpeudp2_unprotect(tmp, wireLen, &dummy, &off))
	{
		Stream_Release(s);
		return -1;
	}
	if (dummy || (off != 1))
	{
		(void)fprintf(stderr, "unprotect dummy/off mismatch\n");
		Stream_Release(s);
		return -1;
	}
	if (memcmp(tmp + off, layout, layoutLen) != 0)
	{
		(void)fprintf(stderr, "v2 protect roundtrip mismatch\n");
		Stream_Release(s);
		return -1;
	}
	Stream_Release(s);

	/* dummy roundtrip */
	s = Stream_New(nullptr, 32);
	if (!s)
		return -1;
	Stream_Write_UINT16(s, 0x5004);
	Stream_Write_UINT16(s, 0x0001);
	Stream_Write_UINT16(s, 0x0002);
	Stream_Write(s, "hi", 2);
	if (!rdpeudp2_protect(s, TRUE))
	{
		Stream_Release(s);
		return -1;
	}
	{
		const size_t wl = Stream_Length(s);
		BYTE t2[64] = { 0 };
		memcpy(t2, Stream_Buffer(s), wl);
		dummy = FALSE;
		if (!rdpeudp2_unprotect(t2, wl, &dummy, &off) || !dummy)
		{
			(void)fprintf(stderr, "dummy flag not preserved\n");
			Stream_Release(s);
			return -1;
		}
	}
	Stream_Release(s);
	return 0;
}

static int test_tunnel(void)
{
	BYTE cookie[16] = { 0 };
	for (int i = 0; i < 16; i++)
		cookie[i] = (BYTE)(i + 1);

	wStream* req = rdpemt_build_create_request(0x11223344, cookie);
	if (!req)
		return -1;
	const size_t reqlen = Stream_Length(req);
	BYTE* reqbuf = Stream_Buffer(req);

	BYTE action = 0xFF;
	UINT16 plen = 0;
	UINT8 hlen = 0;
	if (!rdpemt_parse_header(reqbuf, reqlen, &action, &plen, &hlen))
	{
		(void)fprintf(stderr, "tunnel req header parse failed\n");
		Stream_Release(req);
		return -1;
	}
	if ((action != RDPTUNNEL_ACTION_CREATEREQUEST) || (plen != 24) || (hlen != 4))
	{
		(void)fprintf(stderr, "tunnel req header mismatch %u %u %u\n", action, plen, hlen);
		Stream_Release(req);
		return -1;
	}
	Stream_Release(req);

	/* create response S_OK */
	BYTE resp[8] = { (BYTE)RDPTUNNEL_ACTION_CREATERESPONSE, 0x04, 0x00, 0x04,
		             0x00, 0x00, 0x00, 0x00 };
	UINT32 hr = 0xFFFFFFFFu;
	if (!rdpemt_parse_create_response(resp, sizeof(resp), &hr) || (hr != 0))
	{
		(void)fprintf(stderr, "tunnel resp parse failed\n");
		return -1;
	}

	/* data roundtrip header */
	const char* msg = "hello-udp-tunnel";
	wStream* d = rdpemt_build_data((const BYTE*)msg, strlen(msg));
	if (!d)
		return -1;
	if (!rdpemt_parse_header(Stream_Buffer(d), Stream_Length(d), &action, &plen, &hlen) ||
	    (action != RDPTUNNEL_ACTION_DATA) || (plen != strlen(msg)) || (hlen != 4))
	{
		(void)fprintf(stderr, "tunnel data header mismatch\n");
		Stream_Release(d);
		return -1;
	}
	Stream_Release(d);
	return 0;
}

static int test_ackvec(void)
{
	/* Pattern: received, lost, received x5, lost x10 (RLE), received */
	BOOL rec[20] = { TRUE,  FALSE, TRUE, TRUE, TRUE, TRUE, TRUE,
		             FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
		             TRUE,  TRUE,  TRUE };
	wStream* s = rdpeudp_build_ackvec(0x1000, rec, 20, FALSE, 0, 0);
	if (!s)
	{
		(void)fprintf(stderr, "ackvec build failed\n");
		return -1;
	}
	const size_t len = Stream_Length(s);
	BYTE* buf = Stream_Buffer(s);
	UINT16 base = 0;
	BOOL* out = nullptr;
	size_t count = 0;
	if (!rdpeudp_parse_ackvec(buf, len, &base, &out, &count))
	{
		(void)fprintf(stderr, "ackvec parse failed\n");
		Stream_Release(s);
		return -1;
	}
	Stream_Release(s);
	if ((base != 0x1000) || (count < 20))
	{
		(void)fprintf(stderr, "ackvec base/count mismatch %04x %zu\n", base, count);
		free(out);
		return -1;
	}
	for (size_t i = 0; i < 20; i++)
	{
		if (!!out[i] != !!rec[i])
		{
			(void)fprintf(stderr, "ackvec bit %zu mismatch\n", i);
			free(out);
			return -1;
		}
	}
	free(out);
	return 0;
}

static int test_channel_packet(void)
{
	const char* payload = "egfx-frame-123";
	wStream* s = rdpeudp_build_channel_packet(5, 100, 0x03, (const BYTE*)payload,
	                                           strlen(payload));
	if (!s)
		return -1;
	UINT16 cid = 0;
	UINT32 tot = 0;
	UINT32 flags = 0;
	const BYTE* chunk = nullptr;
	size_t clen = 0;
	if (!rdpeudp_parse_channel_packet(Stream_Buffer(s), Stream_Length(s), &cid, &tot,
	                                   &flags, &chunk, &clen))
	{
		(void)fprintf(stderr, "channel parse failed\n");
		Stream_Release(s);
		return -1;
	}
	if ((cid != 5) || (tot != 100) || (flags != 0x03) || (clen != strlen(payload)) ||
	    (memcmp(chunk, payload, clen) != 0))
	{
		(void)fprintf(stderr, "channel mismatch\n");
		Stream_Release(s);
		return -1;
	}
	Stream_Release(s);
	return 0;
}

static int test_mtu(void)
{
	UINT16 up = 0;
	UINT16 down = 0;
	rdpeudp_compute_mtu(1232, 1232, 1232, 1232, &up, &down);
	if ((up != 1232) || (down != 1232))
	{
		(void)fprintf(stderr, "mtu 1232 mismatch %u %u\n", up, down);
		return -1;
	}
	rdpeudp_compute_mtu(1232, 1232, 1132, 1132, &up, &down);
	if ((up != 1132) || (down != 1132))
	{
		(void)fprintf(stderr, "mtu 1132 mismatch %u %u\n", up, down);
		return -1;
	}
	rdpeudp_compute_mtu(1232, 1232, 1200, 1150, &up, &down);
	if ((up != 1150) || (down != 1200))
	{
		(void)fprintf(stderr, "mtu mixed mismatch %u %u\n", up, down);
		return -1;
	}
	const UINT16 p1232 = rdpeudp_payload_for_mtu(1232);
	const UINT16 p1132 = rdpeudp_payload_for_mtu(1132);
	if ((p1232 != 1132) || (p1132 != 1032))
	{
		(void)fprintf(stderr, "payload mismatch %u %u\n", p1232, p1132);
		return -1;
	}
	if (!(p1132 < p1232) || (p1132 < 800))
	{
		(void)fprintf(stderr, "payload range bad\n");
		return -1;
	}
	return 0;
}

static int test_autodetect_framing(void)
{
	/* MS-RDPEMT 2.2.1.1.1: autodetect PDUs travel in Tunnel DATA subheaders. */
	const BYTE pdu[] = { 0x06, 0x00, 0x34, 0x12, 0x01, 0x00 };
	wStream* sub = rdpemt_build_subheader(RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ, pdu,
	                                      sizeof(pdu));
	if (!sub)
	{
		(void)fprintf(stderr, "subheader build failed\n");
		return -1;
	}
	const size_t subLen = Stream_Length(sub);
	/* SubHeaderLength(1)=2, SubHeaderType(1)=0x00, SubHeaderData(6)=PDU. */
	if ((subLen != 8) || (Stream_Buffer(sub)[0] != 2) ||
	    (Stream_Buffer(sub)[1] != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) ||
	    (memcmp(Stream_Buffer(sub) + 2, pdu, sizeof(pdu)) != 0))
	{
		(void)fprintf(stderr, "subheader bytes mismatch\n");
		Stream_Release(sub);
		return -1;
	}

	/* Tunnel DATA with subheader + empty HigherLayerData. */
	wStream* td = rdpemt_build_tunnel_data(Stream_Buffer(sub), subLen, nullptr, 0);
	Stream_Release(sub);
	if (!td)
	{
		(void)fprintf(stderr, "tunnel data build failed\n");
		return -1;
	}
	BYTE action = 0xFF;
	UINT16 plen = 0xFFFF;
	UINT8 hlen = 0;
	if (!rdpemt_decode_header(Stream_Buffer(td), Stream_Length(td), &action, &plen,
	                           &hlen))
	{
		(void)fprintf(stderr, "tunnel data decode failed\n");
		Stream_Release(td);
		return -1;
	}
	if ((action != RDPTUNNEL_ACTION_DATA) || (plen != 0) || (hlen != 4 + subLen))
	{
		(void)fprintf(stderr, "tunnel data header mismatch %u %u %u\n", action, plen,
		              hlen);
		Stream_Release(td);
		return -1;
	}
	/* Full PDU must validate (header + subheaders + payload present). */
	if (!rdpemt_parse_header(Stream_Buffer(td), Stream_Length(td), &action, &plen,
	                           &hlen))
	{
		(void)fprintf(stderr, "tunnel data full parse failed\n");
		Stream_Release(td);
		return -1;
	}
	/* Header-only decode must succeed where full parse requires payload. */
	{
		BYTE hdr4[4] = { 0 };
		memcpy(hdr4, Stream_Buffer(td), 4);
		BYTE a2 = 0xFF;
		UINT16 p2 = 0xFFFF;
		UINT8 h2 = 0;
		if (!rdpemt_decode_header(hdr4, sizeof(hdr4), &a2, &p2, &h2))
		{
			(void)fprintf(stderr, "header-only decode failed\n");
			Stream_Release(td);
			return -1;
		}
		if ((a2 != RDPTUNNEL_ACTION_DATA) || (p2 != plen) || (h2 != hlen))
		{
			(void)fprintf(stderr, "header-only decode mismatch\n");
			Stream_Release(td);
			return -1;
		}
	}
	/* Iterate subheaders back to the PDU. */
	{
		const BYTE* tdb = Stream_Buffer(td);
		const size_t tdLen = Stream_Length(td);
		const BYTE* subStart = tdb + 4;
		const size_t subAvail = (size_t)hlen - 4;
		size_t off = 0;
		BYTE stype = 0xFF;
		const BYTE* sdata = nullptr;
		size_t sdataLen = 0;
		if (!rdpemt_next_subheader(subStart, subAvail, &off, &stype, &sdata,
		                           &sdataLen))
		{
			(void)fprintf(stderr, "subheader iterate failed\n");
			Stream_Release(td);
			return -1;
		}
		if ((stype != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) ||
		    (sdataLen != sizeof(pdu)) || (memcmp(sdata, pdu, sdataLen) != 0) ||
		    (off != subAvail))
		{
			(void)fprintf(stderr, "subheader content mismatch\n");
			Stream_Release(td);
			return -1;
		}
		WINPR_UNUSED(tdLen);
	}
	Stream_Release(td);

	/* Autodetect PDU length helper: 0x08 base + payloadLength. */
	{
		BYTE bwPayload[8 + 16] = { 0x08, 0x00, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
		                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		if (rdpemt_autodetect_pdu_length(bwPayload, sizeof(bwPayload)) !=
		    sizeof(bwPayload))
		{
			(void)fprintf(stderr, "autodetect pdu length mismatch\n");
			return -1;
		}
		if (rdpemt_autodetect_pdu_length(bwPayload, 7) != 0)
		{
			(void)fprintf(stderr, "autodetect truncated should be 0\n");
			return -1;
		}
	}
	return 0;
}

static int test_v2_data_ackvec_order(void)
{
	/* Fixture per MS-RDPEUDP2 2.2.1 order: Header, DataHeader, ACKVEC, DataBody.
	 * flags = DATA|ACKVEC, logWindow=5. DataSeq=0x1111, ACKVEC base=0x2222 with
	 * one bitmap byte (0x01 = first seq received), ChannelSeq=0x3333, body "AB". */
	const BYTE ackvecRaw[] = { 0x22, 0x22, 0x01, 0x01 };
	const BYTE body[] = { 'A', 'B' };
	RdpUdp2Layout enc = WINPR_C_ARRAY_INIT;
	enc.flags = (UINT16)(RDPUDP2_FLAG_DATA | RDPUDP2_FLAG_ACKVEC);
	enc.logWindow = 5;
	enc.hasDataHeader = TRUE;
	enc.dataSeq = 0x1111;
	enc.hasAckvec = TRUE;
	enc.ackvec = ackvecRaw;
	enc.ackvecLen = sizeof(ackvecRaw);
	enc.hasDataBody = TRUE;
	enc.channelSeq = 0x3333;
	enc.dataBody = body;
	enc.dataBodyLen = sizeof(body);

	wStream* s = rdpeudp2_encode_layout(&enc);
	if (!s)
	{
		(void)fprintf(stderr, "encode_layout failed\n");
		return -1;
	}
	/* Expected layout bytes (LE): header 0x500C, dataSeq, ackvec, channelSeq, body. */
	const BYTE expected[] = { 0x0C, 0x50, 0x11, 0x11, 0x22, 0x22,
	                            0x01, 0x01, 0x33, 0x33, 'A',  'B' };
	if ((Stream_Length(s) != sizeof(expected)) ||
	    (memcmp(Stream_Buffer(s), expected, sizeof(expected)) != 0))
	{
		(void)fprintf(stderr, "encode_layout order mismatch\n");
		Stream_Release(s);
		return -1;
	}

	/* Parse back and verify split: DataHeader before ACKVEC before DataBody.
	 * Validate before releasing s (dec points into its buffer). */
	RdpUdp2Layout dec = WINPR_C_ARRAY_INIT;
	if (!rdpeudp2_parse_layout(Stream_Buffer(s), Stream_Length(s), &dec))
	{
		(void)fprintf(stderr, "parse_layout failed\n");
		Stream_Release(s);
		return -1;
	}
	{
		BOOL ok = dec.hasDataHeader && dec.hasAckvec && dec.hasDataBody &&
		          (dec.dataSeq == 0x1111) && (dec.channelSeq == 0x3333) &&
		          (dec.dataBodyLen == 2) && (memcmp(dec.dataBody, "AB", 2) == 0) &&
		          (dec.ackvecLen == sizeof(ackvecRaw)) &&
		          (memcmp(dec.ackvec, ackvecRaw, sizeof(ackvecRaw)) == 0);
		Stream_Release(s);
		s = nullptr;
		if (!ok)
		{
			(void)fprintf(stderr, "parse_layout data mismatch\n");
			return -1;
		}
	}
	/* Old buggy order (DataHeader, DataBody, ACKVEC) must NOT parse as valid
	 * DATA+ACKVEC with same semantics: channelSeq would be misread. */
	{
		const BYTE buggy[] = { 0x0C, 0x50, 0x11, 0x11, 0x33, 0x33, 'A',
	                             'B',  0x22, 0x22, 0x01, 0x01 };
		RdpUdp2Layout bad = WINPR_C_ARRAY_INIT;
		if (rdpeudp2_parse_layout(buggy, sizeof(buggy), &bad))
		{
			/* Parses structurally, but DataBody must be trailing "22 22 01 01"
			 * region, not "AB": proves order matters (channelSeq differs). */
			if (bad.hasDataBody && (bad.channelSeq == 0x3333))
			{
				(void)fprintf(stderr, "buggy order unexpectedly matches\n");
				return -1;
			}
		}
	}
	return 0;
}

static int test_v2_encode_protect_roundtrip(void)
{
	/* Production path (R1): encode_layout -> seek to length -> protect ->
	 * unprotect -> parse_layout. The encoder rewinds to 0; the protector
	 * requires cursor at end. Missing seek produced 8 zero bytes on the wire. */
	const BYTE ackvecRaw[] = { 0x22, 0x22, 0x01, 0x01 };
	const BYTE body[] = { 'A', 'B' };
	RdpUdp2Layout enc = WINPR_C_ARRAY_INIT;
	enc.flags = (UINT16)(RDPUDP2_FLAG_DATA | RDPUDP2_FLAG_ACKVEC);
	enc.logWindow = 5;
	enc.hasDataHeader = TRUE;
	enc.dataSeq = 0x1111;
	enc.hasAckvec = TRUE;
	enc.ackvec = ackvecRaw;
	enc.ackvecLen = sizeof(ackvecRaw);
	enc.hasDataBody = TRUE;
	enc.channelSeq = 0x3333;
	enc.dataBody = body;
	enc.dataBodyLen = sizeof(body);

	wStream* s = rdpeudp2_encode_layout(&enc);
	if (!s)
	{
		(void)fprintf(stderr, "encode for protect failed\n");
		return -1;
	}
	/* Mirror production send path: cursor must be at end before protect. */
	if (!Stream_SetPosition(s, Stream_Length(s)))
	{
		(void)fprintf(stderr, "seek to length failed\n");
		Stream_Release(s);
		return -1;
	}
	if (!rdpeudp2_protect(s, FALSE))
	{
		(void)fprintf(stderr, "protect failed\n");
		Stream_Release(s);
		return -1;
	}
	const size_t wireLen = Stream_Length(s);
	if (wireLen < 8)
	{
		(void)fprintf(stderr, "protected too short\n");
		Stream_Release(s);
		return -1;
	}
	BYTE* wire = Stream_Buffer(s);
	/* Regression check: protected packet must not be all zeros. */
	{
		BOOL allZero = TRUE;
		for (size_t i = 0; i < wireLen; i++)
		{
			if (wire[i] != 0)
			{
				allZero = FALSE;
				break;
			}
		}
		if (allZero)
		{
			(void)fprintf(stderr, "protected packet all zeros (R1)\n");
			Stream_Release(s);
			return -1;
		}
	}
	BYTE tmp[128] = { 0 };
	if (wireLen > sizeof(tmp))
	{
		Stream_Release(s);
		return -1;
	}
	memcpy(tmp, wire, wireLen);
	Stream_Release(s);
	s = nullptr;

	BOOL dummy = TRUE;
	size_t off = 0;
	if (!rdpeudp2_unprotect(tmp, wireLen, &dummy, &off) || dummy || (off != 1))
	{
		(void)fprintf(stderr, "unprotect failed\n");
		return -1;
	}
	RdpUdp2Layout dec = WINPR_C_ARRAY_INIT;
	if (!rdpeudp2_parse_layout(tmp + off, wireLen - off, &dec))
	{
		(void)fprintf(stderr, "parse after unprotect failed\n");
		return -1;
	}
	if (!dec.hasDataHeader || !dec.hasAckvec || !dec.hasDataBody ||
	    (dec.dataSeq != 0x1111) || (dec.channelSeq != 0x3333) ||
	    (dec.dataBodyLen != 2) || (memcmp(dec.dataBody, "AB", 2) != 0))
	{
		(void)fprintf(stderr, "roundtrip data mismatch\n");
		return -1;
	}
	return 0;
}

static int test_tunnel_split(void)
{
	/* R3: Tunnel DATA framing must support incremental reassembly. Build one
	 * PDU with a subheader (6-byte autodetect PDU) + 5-byte HigherLayerData,
	 * then verify split reads: 2-byte header prefix is incomplete (not corrupt),
	 * 4-byte header decodes to total, 6-byte prefix parses as incomplete,
	 * full 17 bytes parse and dispatch. */
	const BYTE pdu[] = { 0x06, 0x00, 0x34, 0x12, 0x01, 0x00 };
	const BYTE hl[] = { 'H', 'E', 'L', 'L', 'O' };
	wStream* sub = rdpemt_build_subheader(RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ, pdu,
	                                      sizeof(pdu));
	if (!sub)
		return -1;
	wStream* td = rdpemt_build_tunnel_data(Stream_Buffer(sub), Stream_Length(sub), hl,
	                                       sizeof(hl));
	Stream_Release(sub);
	if (!td)
	{
		(void)fprintf(stderr, "split: build failed\n");
		return -1;
	}
	const size_t total = Stream_Length(td);
	const BYTE* buf = Stream_Buffer(td);
	if (total != 4 + 8 + 5)
	{
		(void)fprintf(stderr, "split: total %zu != 17\n", total);
		Stream_Release(td);
		return -1;
	}
	/* 2-byte split header: decode must fail as incomplete (needs 4). */
	{
		BYTE a = 0;
		UINT16 pl = 0;
		UINT8 hl2 = 0;
		if (rdpemt_decode_header(buf, 2, &a, &pl, &hl2))
		{
			(void)fprintf(stderr, "split: 2-byte decode should fail\n");
			Stream_Release(td);
			return -1;
		}
	}
	/* 4-byte header decodes to hlen=12 plen=5 without needing the rest. */
	BYTE action = 0xFF;
	UINT16 plen = 0xFFFF;
	UINT8 hlen = 0;
	if (!rdpemt_decode_header(buf, 4, &action, &plen, &hlen))
	{
		(void)fprintf(stderr, "split: 4-byte decode failed\n");
		Stream_Release(td);
		return -1;
	}
	if ((action != RDPTUNNEL_ACTION_DATA) || (plen != 5) || (hlen != 12))
	{
		(void)fprintf(stderr, "split: header mismatch\n");
		Stream_Release(td);
		return -1;
	}
	/* 6-byte prefix (header + 2 subheader bytes) is incomplete: full parse fails. */
	if (rdpemt_parse_header(buf, 6, &action, &plen, &hlen))
	{
		(void)fprintf(stderr, "split: 6-byte parse should fail\n");
		Stream_Release(td);
		return -1;
	}
	/* Full PDU parses; subheader iterates; payload matches. */
	if (!rdpemt_parse_header(buf, total, &action, &plen, &hlen))
	{
		(void)fprintf(stderr, "split: full parse failed\n");
		Stream_Release(td);
		return -1;
	}
	{
		size_t off = 0;
		BYTE st = 0xFF;
		const BYTE* sd = nullptr;
		size_t sdl = 0;
		if (!rdpemt_next_subheader(buf + 4, (size_t)hlen - 4, &off, &st, &sd, &sdl))
		{
			(void)fprintf(stderr, "split: subheader iterate failed\n");
			Stream_Release(td);
			return -1;
		}
		if ((st != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) || (sdl != sizeof(pdu)) ||
		    (memcmp(sd, pdu, sdl) != 0) || (off != (size_t)hlen - 4))
		{
			(void)fprintf(stderr, "split: subheader content mismatch\n");
			Stream_Release(td);
			return -1;
		}
		if (memcmp(buf + hlen, hl, sizeof(hl)) != 0)
		{
			(void)fprintf(stderr, "split: payload mismatch\n");
			Stream_Release(td);
			return -1;
		}
	}
	Stream_Release(td);
	return 0;
}

static int test_soft_sync_offers(void)
{
	/* Request with list offering UDPFECR for DVC 7; lossy-only; no-list. */
	const BYTE reqFecr[] = { 0x80, 0x00, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
	                           0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00 };
	const BYTE reqLossy[] = { 0x80, 0x00, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
	                            0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00 };
	const BYTE reqNoList[] = { 0x80, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00 };
	const BYTE rspFecr[] = { 0x90, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
	const BYTE rspEmpty[] = { 0x90, 0x00, 0x00, 0x00, 0x00, 0x00 };
	const BYTE rspLossy[] = { 0x90, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00 };
	if (!rdpeudp_soft_sync_request_offers_udp(reqFecr, sizeof(reqFecr)))
	{
		(void)fprintf(stderr, "softsync req fecr should offer\n");
		return -1;
	}
	if (rdpeudp_soft_sync_request_offers_udp(reqLossy, sizeof(reqLossy)))
	{
		(void)fprintf(stderr, "softsync req lossy should not offer\n");
		return -1;
	}
	if (!rdpeudp_soft_sync_request_offers_udp(reqNoList, sizeof(reqNoList)))
	{
		(void)fprintf(stderr, "softsync req nolist should offer\n");
		return -1;
	}
	if (!rdpeudp_soft_sync_response_offers_udp(rspFecr, sizeof(rspFecr)))
	{
		(void)fprintf(stderr, "softsync rsp fecr should offer\n");
		return -1;
	}
	if (rdpeudp_soft_sync_response_offers_udp(rspEmpty, sizeof(rspEmpty)))
	{
		(void)fprintf(stderr, "softsync rsp empty should not offer\n");
		return -1;
	}
	if (rdpeudp_soft_sync_response_offers_udp(rspLossy, sizeof(rspLossy)))
	{
		(void)fprintf(stderr, "softsync rsp lossy should not offer\n");
		return -1;
	}
	return 0;
}

int TestRdpeUdp(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (test_fec_header() != 0)
	{
		(void)fprintf(stderr, "test_fec_header FAILED\n");
		return -1;
	}
	if (test_syn_roundtrip() != 0)
	{
		(void)fprintf(stderr, "test_syn_roundtrip FAILED\n");
		return -1;
	}
	if (test_v2_protect() != 0)
	{
		(void)fprintf(stderr, "test_v2_protect FAILED\n");
		return -1;
	}
	if (test_tunnel() != 0)
	{
		(void)fprintf(stderr, "test_tunnel FAILED\n");
		return -1;
	}
	if (test_ackvec() != 0)
	{
		(void)fprintf(stderr, "test_ackvec FAILED\n");
		return -1;
	}
	if (test_channel_packet() != 0)
	{
		(void)fprintf(stderr, "test_channel_packet FAILED\n");
		return -1;
	}
	if (test_mtu() != 0)
	{
		(void)fprintf(stderr, "test_mtu FAILED\n");
		return -1;
	}
	if (test_autodetect_framing() != 0)
	{
		(void)fprintf(stderr, "test_autodetect_framing FAILED\n");
		return -1;
	}
	if (test_v2_data_ackvec_order() != 0)
	{
		(void)fprintf(stderr, "test_v2_data_ackvec_order FAILED\n");
		return -1;
	}
	if (test_v2_encode_protect_roundtrip() != 0)
	{
		(void)fprintf(stderr, "test_v2_encode_protect_roundtrip FAILED\n");
		return -1;
	}
	if (test_tunnel_split() != 0)
	{
		(void)fprintf(stderr, "test_tunnel_split FAILED\n");
		return -1;
	}
	if (test_soft_sync_offers() != 0)
	{
		(void)fprintf(stderr, "test_soft_sync_offers FAILED\n");
		return -1;
	}

	(void)printf("TestRdpeUdp passed\n");
	return 0;
}
