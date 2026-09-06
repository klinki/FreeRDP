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
#include <freerdp/utils/drdynvc.h>

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
	/* Absolute prefix check (not just roundtrip): normal DATA must be 0xE0
	 * (shortLen=7 | type=0). The mirrored 0x07 decodes as reserved packet
	 * type 3 and is silently dropped by Windows although v1 still works. */
	if (wireLen < 8 || (wire[7] != 0xE0))
	{
		(void)fprintf(stderr, "DATA prefix must be 0xE0, got 0x%02x\n",
		              (wireLen >= 8) ? wire[7] : 0);
		Stream_Release(s);
		return -1;
	}
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
		if ((wl < 8) || (Stream_Buffer(s)[7] != 0xF0))
		{
			(void)fprintf(stderr, "dummy prefix must be 0xF0\n");
			Stream_Release(s);
			return -1;
		}
	}
	Stream_Release(s);

	/* Interop fixture: first DATA of a healthy MS-client session on the
	 * wire (prefix + layout for flags DATA|AOA|DELAYACK, dseq=100, cseq=1).
	 * Our parser must accept it exactly; our encoder must emit the same
	 * prefix byte for normal DATA. */
	{
		static const BYTE msWire[] = {
			0x00, 0x14, 0xF3, 0x01, 0x14, 0x00, 0x64, 0xE0, 0x64, 0x00, 0x01, 0x00,
			0xAA, 0xBB
		};
		BYTE ms[sizeof(msWire)] = { 0 };
		memcpy(ms, msWire, sizeof(msWire));
		BOOL msDummy = TRUE;
		size_t msOff = 0;
		if (!rdpeudp2_unprotect(ms, sizeof(ms), &msDummy, &msOff) || msDummy ||
		    (msOff != 1))
		{
			(void)fprintf(stderr, "MS DATA prefix rejected\n");
			return -1;
		}
		RdpUdp2Layout msLayout = { 0 };
		if (!rdpeudp2_parse_layout(ms + msOff, sizeof(ms) - msOff, &msLayout))
		{
			(void)fprintf(stderr, "MS DATA layout rejected\n");
			return -1;
		}
		/* Bit 0x200 is set by the MS client but unnamed in the spec;
		 * assert the meaningful fields, not exact flag equality. */
		if (!msLayout.hasDataHeader || !msLayout.hasDelayAck || !msLayout.hasAoa ||
		    msLayout.hasAck || msLayout.hasOverhead || msLayout.hasAckvec ||
		    (msLayout.logWindow != 15) || (msLayout.dataSeq != 100) ||
		    !msLayout.hasDataBody || (msLayout.channelSeq != 1))
		{
			(void)fprintf(stderr, "MS DATA fields mismatch\n");
			return -1;
		}
	}
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
	/* MS-RDPEMT 2.2.1.1.1: autodetect PDUs travel in Tunnel DATA subheaders,
	 * overlaid: the subheader IS the PDU's own header start ([totalLen][type]
	 * + remainder), not a separate Length=2 prefix. */
	const BYTE pdu[] = { 0x06, 0x00, 0x34, 0x12, 0x01, 0x00 };
	wStream* sub = rdpemt_build_subheader(RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ, pdu,
	                                      sizeof(pdu));
	if (!sub)
	{
		(void)fprintf(stderr, "subheader build failed\n");
		return -1;
	}
	const size_t subLen = Stream_Length(sub);
	/* Overlaid emit: identical to the complete 6-byte PDU. */
	if ((subLen != sizeof(pdu)) || (memcmp(Stream_Buffer(sub), pdu, sizeof(pdu)) != 0))
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

	/* Live-VM fixture: 18-byte subheader region exactly as received
	 * (subLen == PDU total; REQ network-characteristics result,
	 * requestType 0x08C0, baseRTT=1, bandwidth=512, averageRTT=360).
	 * The old separate-framing parser rejected this (headerLength 0x00
	 * at data offset); the overlaid model consumes it exactly. */
	{
		static const BYTE live[] = { 0x12, 0x00, 0x00, 0x00, 0xC0, 0x08, 0x01, 0x00,
			                           0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x68, 0x01,
			                           0x00, 0x00 };
		size_t off = 0;
		BYTE stype = 0xFF;
		const BYTE* sdata = nullptr;
		size_t sdataLen = 0;
		if (!rdpemt_next_subheader(live, sizeof(live), &off, &stype, &sdata, &sdataLen))
		{
			(void)fprintf(stderr, "live subheader rejected\n");
			return -1;
		}
		if ((stype != RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ) || (sdataLen != 18) ||
		    (sdata != live) || (off != sizeof(live)))
		{
			(void)fprintf(stderr, "live subheader content mismatch\n");
			return -1;
		}
		/* Trailing garbage must not parse as a second subheader. */
		if (rdpemt_next_subheader(live, sizeof(live), &off, &stype, &sdata, &sdataLen))
		{
			(void)fprintf(stderr, "live trailing subheader should fail\n");
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
	if (total != 4 + 6 + 5)
	{
		(void)fprintf(stderr, "split: total %zu != 15\n", total);
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
	/* 4-byte header decodes to hlen=10 plen=5 without needing the rest. */
	BYTE action = 0xFF;
	UINT16 plen = 0xFFFF;
	UINT8 hlen = 0;
	if (!rdpemt_decode_header(buf, 4, &action, &plen, &hlen))
	{
		(void)fprintf(stderr, "split: 4-byte decode failed\n");
		Stream_Release(td);
		return -1;
	}
	if ((action != RDPTUNNEL_ACTION_DATA) || (plen != 5) || (hlen != 10))
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
	/* S3 negatives: FLUSHED cleared, truncated length, truncated second tunnel. */
	{
		/* Same as reqFecr but Flags=0x02 (no TCP_FLUSHED). */
		BYTE noFlush[] = { 0x80, 0x00, 0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00,
		                     0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00 };
		if (rdpeudp_soft_sync_request_offers_udp(noFlush, sizeof(noFlush)))
		{
			(void)fprintf(stderr, "softsync no-flush should not offer\n");
			return -1;
		}
	}
	{
		/* Declared Length (0x12) exceeds supplied bytes (truncated by 4). */
		BYTE trunc[16] = { 0 };
		memcpy(trunc, reqFecr, sizeof(trunc));
		if (rdpeudp_soft_sync_request_offers_udp(trunc, sizeof(trunc)))
		{
			(void)fprintf(stderr, "softsync truncated should not offer\n");
			return -1;
		}
	}
	{
		/* Two tunnels declared, only the first (UDPFECR) present. */
		const BYTE two[] = { 0x80, 0x00, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00,
		                       0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00 };
		if (rdpeudp_soft_sync_request_offers_udp(two, sizeof(two)))
		{
			(void)fprintf(stderr, "softsync short-tunnels should not offer\n");
			return -1;
		}
	}
	{
		/* Valid response with one trailing byte must be rejected. */
		BYTE trail[11] = { 0 };
		memcpy(trail, rspFecr, sizeof(rspFecr));
		trail[sizeof(rspFecr)] = 0xAA;
		if (rdpeudp_soft_sync_response_offers_udp(trail, sizeof(trail)))
		{
			(void)fprintf(stderr, "softsync trailing should not offer\n");
			return -1;
		}
	}
	/* T1: 257-ID request installs the full map (no 256 fixed-cap fallback);
	 * unlisted DVC 999 must not be treated as migrated. */
	{
		static BYTE big[10 + 6 + 257 * 4];
		/* Header(1) + Pad(1) + Length(4) + Flags(2) + Tunnels(2) = 10, then
		 * one UDPFECR list: type(4) + count(2) + 257 ids. Length = 8 + 6 + 1028. */
		const UINT32 llen = 8 + 6 + 257 * 4;
		size_t o = 0;
		big[o++] = 0x80;
		big[o++] = 0x00;
		big[o++] = (BYTE)(llen & 0xFF);
		big[o++] = (BYTE)((llen >> 8) & 0xFF);
		big[o++] = (BYTE)((llen >> 16) & 0xFF);
		big[o++] = (BYTE)((llen >> 24) & 0xFF);
		big[o++] = 0x03;
		big[o++] = 0x00;
		big[o++] = 0x01;
		big[o++] = 0x00;
		big[o++] = 0x01;
		big[o++] = 0x00;
		big[o++] = 0x00;
		big[o++] = 0x00;
		big[o++] = 0x01;
		big[o++] = 0x01;
		for (UINT32 i = 1; i <= 257; i++)
		{
			big[o++] = (BYTE)(i & 0xFF);
			big[o++] = (BYTE)((i >> 8) & 0xFF);
			big[o++] = (BYTE)((i >> 16) & 0xFF);
			big[o++] = (BYTE)((i >> 24) & 0xFF);
		}
		if (o != sizeof(big))
		{
			(void)fprintf(stderr, "softsync big fixture size\n");
			return -1;
		}
		if (!rdpeudp_soft_sync_request_offers_udp(big, sizeof(big)))
		{
			(void)fprintf(stderr, "softsync 257 should offer\n");
			return -1;
		}
		{
			static UINT32 ids[300] = { 0 };
			size_t count = 0;
			if (!rdpeudp_soft_sync_request_udp_dvcs(big, sizeof(big), ids, 300, &count) ||
			    (count != 257) || (ids[0] != 1) || (ids[256] != 257))
			{
				(void)fprintf(stderr, "softsync 257 extract mismatch %zu\n", count);
				return -1;
			}
			BOOL listed999 = FALSE;
			for (size_t i = 0; i < count; i++)
			{
				if (ids[i] == 999)
					listed999 = TRUE;
			}
			if (listed999)
			{
				(void)fprintf(stderr, "softsync 999 must be unlisted\n");
				return -1;
			}
		}
		/* First fragment alone (10-byte prefix, no lists yet) authorizes nothing. */
		if (rdpeudp_soft_sync_request_offers_udp(big, 10))
		{
			(void)fprintf(stderr, "softsync fragment should not offer\n");
			return -1;
		}
	}
	/* Trailing byte covered by a bumped Length must be rejected. */
	{
		BYTE extra[sizeof(reqFecr) + 1] = { 0 };
		memcpy(extra, reqFecr, sizeof(reqFecr));
		extra[2]++; /* Length 0x12 -> 0x13 to cover the extra byte */
		extra[sizeof(reqFecr)] = 0xAA;
		if (rdpeudp_soft_sync_request_offers_udp(extra, sizeof(extra)))
		{
			(void)fprintf(stderr, "softsync req trailing should not offer\n");
			return -1;
		}
	}
	/* S2: extraction honors lists (DVC 7 on UDPFECR migrates, DVC 9 on lossy does not). */
	{
		const BYTE reqTwo[] = {
			0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x07, 0x00, 0x00, 0x00,
			0x08, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00,
			0x09, 0x00, 0x00, 0x00
		};
		UINT32 ids[8] = { 0 };
		size_t count = 0;
		if (!rdpeudp_soft_sync_request_udp_dvcs(reqTwo, sizeof(reqTwo), ids, 8, &count))
		{
			(void)fprintf(stderr, "softsync extract failed\n");
			return -1;
		}
		if ((count != 2) || (ids[0] != 7) || (ids[1] != 8))
		{
			(void)fprintf(stderr, "softsync extract ids mismatch %zu\n", count);
			return -1;
		}
		/* Lossy-only request extracts nothing (valid parse, no offer). */
		{
			UINT32 ids2[8] = { 0 };
			size_t count2 = 99;
			if (rdpeudp_soft_sync_request_udp_dvcs(reqLossy, sizeof(reqLossy), ids2, 8,
			                                     &count2))
			{
				(void)fprintf(stderr, "softsync lossy extract should fail\n");
				return -1;
			}
		}
	}
	return 0;
}

static int test_tunnel_consume(void)
{
	/* S1: exercise the same consume helper the transport loop uses, fed in
	 * uneven fragments mimicking timeouts between arrivals, with two
	 * back-to-back PDUs to check alignment is preserved. */
	const BYTE pduA[] = { 0x06, 0x00, 0x34, 0x12, 0x01, 0x00 };
	const BYTE hlA[] = { 'H', 'E', 'L', 'L', 'O' };
	const BYTE hlB[] = { 'B', 'Y', 'E' };
	wStream* sub = rdpemt_build_subheader(RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ, pduA,
	                                      sizeof(pduA));
	if (!sub)
		return -1;
	wStream* tdA = rdpemt_build_tunnel_data(Stream_Buffer(sub), Stream_Length(sub), hlA,
	                                        sizeof(hlA));
	Stream_Release(sub);
	wStream* tdB = rdpemt_build_tunnel_data(nullptr, 0, hlB, sizeof(hlB));
	if (!tdA || !tdB)
	{
		if (tdA)
			Stream_Release(tdA);
		if (tdB)
			Stream_Release(tdB);
		(void)fprintf(stderr, "consume: build failed\n");
		return -1;
	}
	const size_t lenA = Stream_Length(tdA);
	const size_t lenB = Stream_Length(tdB);
	static BYTE wire[131072];
	if (lenA + lenB > sizeof(wire))
	{
		Stream_Release(tdA);
		Stream_Release(tdB);
		return -1;
	}
	memcpy(wire, Stream_Buffer(tdA), lenA);
	memcpy(wire + lenA, Stream_Buffer(tdB), lenB);
	Stream_Release(tdA);
	Stream_Release(tdB);
	tdA = nullptr;
	tdB = nullptr;
	const size_t total = lenA + lenB;

	static BYTE stream[131072];
	size_t have = 0;
	size_t pos = 0;
	int pdus = 0;
	static const size_t frags[] = { 2, 4, 3, 5, 7, 1024 };
	size_t fi = 0;
	while ((pos < total) || (have > 0))
	{
		/* Drain every complete PDU currently buffered. */
		while (TRUE)
		{
			size_t consumed = 0;
			BYTE subOut[512] = { 0 };
			BYTE payOut[1024] = { 0 };
			size_t subOutLen = 0;
			size_t payOutLen = 0;
			const int cr = rdpemt_tunnel_consume(stream, have, &consumed, subOut,
			                                     sizeof(subOut), &subOutLen, payOut,
			                                     sizeof(payOut), &payOutLen);
			if (cr == 0)
				break; /* need more bytes */
			if (cr < 0)
			{
				(void)fprintf(stderr, "consume: corrupt\n");
				return -1;
			}
			pdus++;
			if (pdus == 1)
			{
				if ((payOutLen != sizeof(hlA)) || (memcmp(payOut, hlA, payOutLen) != 0))
				{
					(void)fprintf(stderr, "consume: PDU A payload mismatch\n");
					return -1;
				}
			}
			else if (pdus == 2)
			{
				if ((payOutLen != sizeof(hlB)) || (memcmp(payOut, hlB, payOutLen) != 0))
				{
					(void)fprintf(stderr, "consume: PDU B payload mismatch\n");
					return -1;
				}
			}
			else
			{
				(void)fprintf(stderr, "consume: too many PDUs\n");
				return -1;
			}
			memmove(stream, stream + consumed, have - consumed);
			have -= consumed;
		}
		if (pos >= total)
			break;
		/* Timeout between fragments: append the next uneven chunk. */
		size_t take = frags[fi % (sizeof(frags) / sizeof(frags[0]))];
		fi++;
		if (take > total - pos)
			take = total - pos;
		if (have + take > sizeof(stream))
		{
			(void)fprintf(stderr, "consume: overflow\n");
			return -1;
		}
		memcpy(stream + have, wire + pos, take);
		have += take;
		pos += take;
	}
	if (pdus != 2)
	{
		(void)fprintf(stderr, "consume: got %d PDUs, want 2\n", pdus);
		return -1;
	}
	if (have != 0)
	{
		(void)fprintf(stderr, "consume: trailing bytes\n");
		return -1;
	}
	/* Corrupt action byte must report -1, not consume. */
	{
		const BYTE bad[8] = { 0x0F, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00 };
		size_t consumed = 99;
		if (rdpemt_tunnel_consume(bad, sizeof(bad), &consumed, nullptr, 0, nullptr,
		                            nullptr, 0, nullptr) != -1)
		{
			(void)fprintf(stderr, "consume: corrupt should fail\n");
			return -1;
		}
	}
	return 0;
}


static int test_soft_sync_shared_parity(void)
{
	/* U1: the DVC handler, server, and core hooks all call the shared utils
	 * parser, so these verdicts ARE the handler's verdicts. Every fixture
	 * must agree across both API layers, including the no-list trailing
	 * reproducer that the old duplicated handler accepted. */
	static const BYTE reqFecr[] = { 0x80, 0x00, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00,
	                                0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	                                0x07, 0x00, 0x00, 0x00 };
	static const BYTE reqNoList[] = { 0x80, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00,
	                                  0x01, 0x00 };
	static const BYTE reqLossy[] = { 0x80, 0x00, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00,
	                                 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00,
	                                 0x07, 0x00, 0x00, 0x00 };
	static const BYTE reqNoListTrailing[] = { 0x80, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01,
	                                          0x00, 0x01, 0x00, 0xFF };
	static const BYTE reqListedTrailing[] = { 0x80, 0x00, 0x13, 0x00, 0x00, 0x00, 0x03,
	                                          0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
	                                          0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0xAA };
	static const BYTE rspFecr[] = { 0x90, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	                                0x00, 0x00 };
	static const BYTE rspEmpty[] = { 0x90, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct
	{
		const BYTE* req;
		size_t reqLen;
		BOOL reqValid;
		BOOL reqOffers;
	} reqCases[] = {
		{ reqFecr, sizeof(reqFecr), TRUE, TRUE },
		{ reqNoList, sizeof(reqNoList), TRUE, TRUE },
		{ reqLossy, sizeof(reqLossy), TRUE, FALSE },
		{ reqNoListTrailing, sizeof(reqNoListTrailing), FALSE, FALSE },
		{ reqListedTrailing, sizeof(reqListedTrailing), FALSE, FALSE },
	};
	struct
	{
		const BYTE* rsp;
		size_t rspLen;
		BOOL rspOffers;
	} rspCases[] = {
		{ rspFecr, sizeof(rspFecr), TRUE },
		{ rspEmpty, sizeof(rspEmpty), FALSE },
	};
	for (size_t i = 0; i < ARRAYSIZE(reqCases); i++)
	{
		const BOOL uv = drdynvc_soft_sync_request_validate(reqCases[i].req,
		                                                            reqCases[i].reqLen);
		const BOOL uo = drdynvc_soft_sync_request_offers_udp(reqCases[i].req,
		                                                              reqCases[i].reqLen);
		/* Core wrappers must agree exactly (same implementation). */
		if (uv != reqCases[i].reqValid || uo != reqCases[i].reqOffers)
		{
			(void)fprintf(stderr, "softsync parity req %zu: utils valid=%d offers=%d\n", i,
			              uv, uo);
			return -1;
		}
	}
	for (size_t i = 0; i < ARRAYSIZE(rspCases); i++)
	{
		const BOOL uo = drdynvc_soft_sync_response_offers_udp(rspCases[i].rsp,
		                                                              rspCases[i].rspLen);
		if (uo != rspCases[i].rspOffers)
		{
			(void)fprintf(stderr, "softsync parity rsp %zu: utils offers=%d\n", i, uo);
			return -1;
		}
		if (rdpeudp_soft_sync_response_offers_udp(rspCases[i].rsp, rspCases[i].rspLen) != uo)
		{
			(void)fprintf(stderr, "softsync parity rsp %zu: wrapper mismatch\n", i);
			return -1;
		}
	}
	return 0;
}

static int test_dvc_pdu_length(void)
{
	/* Live CREATEs: `18 02/07/08 <name>\0`, one PDU each (VM + DavidPC). */
	static const BYTE createCoreIn[] = {
		0x18, 0x02, 'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ':',
		':', 'W', 'i', 'n', 'd', 'o', 'w', 's', ':', ':', 'R', 'D', 'S', ':', ':',
		'C', 'o', 'r', 'e', 'I', 'n', 'p', 'u', 't', 0x00
	};
	static const BYTE createGfx[] = {
		0x18, 0x07, 'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ':',
		':', 'W', 'i', 'n', 'd', 'o', 'w', 's', ':', ':', 'R', 'D', 'S', ':', ':',
		'G', 'r', 'a', 'p', 'h', 'i', 'c', 's', 0x00
	};
	size_t len = 0;
	if (!rdpeudp_dvc_pdu_length(createCoreIn, sizeof(createCoreIn), FALSE, &len) ||
	    (len != sizeof(createCoreIn)))
	{
		(void)fprintf(stderr, "live CREATE CoreInput rejected\n");
		return -1;
	}
	if (!rdpeudp_dvc_pdu_length(createGfx, sizeof(createGfx), FALSE, &len) ||
	    (len != sizeof(createGfx)))
	{
		(void)fprintf(stderr, "live CREATE Graphics rejected\n");
		return -1;
	}
	/* Synthetic CLOSE (header Cmd=4 + 1-byte id) and DATA_FIRST exact-fit. */
	{
		static const BYTE closePdu[] = { 0x44, 0x09 };
		if (!rdpeudp_dvc_pdu_length(closePdu, sizeof(closePdu), FALSE, &len) || (len != 2))
		{
			(void)fprintf(stderr, "CLOSE rejected\n");
			return -1;
		}
		/* DATA_FIRST total == present (exact fit): header + id + len(9) + 3 data. */
		static const BYTE firstPdu[] = { 0x20, 0x05, 0x09, 0x00, 0x00, 0x00,
			                               'A', 'B', 'C' };
		if (!rdpeudp_dvc_pdu_length(firstPdu, sizeof(firstPdu), FALSE, &len) || (len != 9))
		{
			(void)fprintf(stderr, "exact DATA_FIRST rejected\n");
			return -1;
		}
		/* Plain DATA runs to end of buffer. */
		static const BYTE dataPdu[] = { 0x30, 0x0A, 'x', 'y' };
		if (!rdpeudp_dvc_pdu_length(dataPdu, sizeof(dataPdu), FALSE, &len) || (len != 4))
		{
			(void)fprintf(stderr, "DATA rejected\n");
			return -1;
		}
	}
	/* Negatives: truncated header/id, unterminated CREATE, zero-length
	 * DATA_FIRST, unknown command. A fragmented DATA_FIRST (total beyond
	 * present) is ACCEPTED consuming all (see below), so it is not here. */
	{
		static const BYTE trunc[] = { 0x18 };
		static const BYTE noNul[] = { 0x18, 0x02, 'A', 'B' };
		static const BYTE zeroTotal[] = { 0x20, 0x05, 0x00 };
		static const BYTE unknown[] = { 0xF8, 0x01 };
		if (rdpeudp_dvc_pdu_length(trunc, sizeof(trunc), FALSE, &len) ||
		    rdpeudp_dvc_pdu_length(noNul, sizeof(noNul), FALSE, &len) ||
		    rdpeudp_dvc_pdu_length(zeroTotal, sizeof(zeroTotal), FALSE, &len) ||
		    rdpeudp_dvc_pdu_length(unknown, sizeof(unknown), FALSE, &len))
		{
			(void)fprintf(stderr, "negative DVC fixture accepted\n");
			return -1;
		}
	}
	/* Fragmented DATA_FIRST (total 0x20=32 > 4 present bytes): accepted,
	 * consuming all present bytes; completion tracked by the caller. */
	{
		static const BYTE fragFirst[] = { 0x20, 0x05, 0x20, 0x00, 0x00, 0x00, 'A' };
		if (!rdpeudp_dvc_pdu_length(fragFirst, sizeof(fragFirst), FALSE, &len) ||
		    (len != sizeof(fragFirst)))
		{
			(void)fprintf(stderr, "fragmented DATA_FIRST rejected\n");
			return -1;
		}
	}
	/* CREATE responses (server-side direction, N4): same command nibble as
	 * requests but header + ChannelId + 4-byte status. Success and failure
	 * statuses both take 6 bytes here (1-byte id); truncation fails. */
	{
		static const BYTE rspOk[] = { 0x10, 0x07, 0x00, 0x00, 0x00, 0x00 };
		static const BYTE rspFail[] = { 0x10, 0x07, 0x01, 0x00, 0x00, 0x80 };
		static const BYTE rspTrunc[] = { 0x10, 0x07, 0x00, 0x00, 0x00 };
		if (!rdpeudp_dvc_pdu_length(rspOk, sizeof(rspOk), TRUE, &len) || (len != 6))
		{
			(void)fprintf(stderr, "CREATE success response rejected\n");
			return -1;
		}
		if (!rdpeudp_dvc_pdu_length(rspFail, sizeof(rspFail), TRUE, &len) || (len != 6))
		{
			(void)fprintf(stderr, "CREATE failure response rejected\n");
			return -1;
		}
		if (rdpeudp_dvc_pdu_length(rspTrunc, sizeof(rspTrunc), TRUE, &len))
		{
			(void)fprintf(stderr, "truncated CREATE response accepted\n");
			return -1;
		}
	}
	return 0;
}

/* Q2 transport integration: real receiver (rdpeudp_test_feed runs the
 * production v2 receive block) + sender model (production
 * rdpeudp2_encode_layout + rdpeudp2_protect) + retransmit-as-new-dseq model.
 * Covers the MS probe epoch, the adversarial far-ahead AOA case and its
 * recovery, and gap preservation — all against real receive state. */
typedef struct
{
	UINT16 dseq;
	UINT16 cseq;
	const BYTE* body;
	size_t bodyLen;
	BOOL hasAoa;
	UINT16 aoa;
	BOOL dummy;
	BOOL withBody;
} RxScriptPkt;

static wStream* rx_emit(const RxScriptPkt* pkt)
{
	RdpUdp2Layout in = { 0 };
	in.logWindow = 5;
	if (pkt->hasAoa)
	{
		in.flags |= RDPUDP2_FLAG_AOA;
		in.hasAoa = TRUE;
		in.aoa = pkt->aoa;
	}
	if (pkt->withBody)
	{
		in.flags |= RDPUDP2_FLAG_DATA;
		in.hasDataHeader = TRUE;
		in.dataSeq = pkt->dseq;
		in.hasDataBody = TRUE;
		in.channelSeq = pkt->cseq;
		in.dataBody = pkt->body;
		in.dataBodyLen = pkt->bodyLen;
	}
	wStream* s = rdpeudp2_encode_layout(&in);
	if (!s)
		return nullptr;
	if (!Stream_SetPosition(s, Stream_Length(s)) || !rdpeudp2_protect(s, pkt->dummy))
	{
		Stream_Release(s);
		return nullptr;
	}
	return s;
}

static int rx_feed(rdpUdpTransport* udp, const RxScriptPkt* pkt)
{
	wStream* s = rx_emit(pkt);
	if (!s)
		return -1;
	const BOOL ok = rdpeudp_test_feed(udp, Stream_Buffer(s), Stream_Length(s));
	Stream_Release(s);
	return ok ? 0 : -1;
}

static int rx_state(rdpUdpTransport* udp, RdpUdpTestRecvState* out)
{
	return rdpeudp_test_recv_state(udp, out) ? 0 : -1;
}

#define RX_CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			(void)fprintf(stderr, "rx_integration FAILED line %d: %s\n", __LINE__, #cond); \
			rdpeudp_test_free(udp); \
			return -1; \
		} \
	} while (0)

static int test_rx_integration(void)
{
	rdpUdpTransport* udp = nullptr;
	RdpUdpTestRecvState st = { 0 };
	static const BYTE b1[] = { 'A', 'B' };
	static const BYTE b2[] = { 'C', 'D' };

	/* S1: MS-like probe epoch (100..145, AOA=100) then DATA 146. */
	udp = rdpeudp_test_new();
	if (!udp)
		return -1;
	for (UINT16 d = 100; d <= 145; d++)
	{
		/* Dummy prefix + DATA header/AOA + empty body: the state path
		 * under test only sees (dataSeq, aoa, dummy); the empty body is
		 * never delivered. A headerless probe could not move the DataSeq
		 * window at all. */
		const RxScriptPkt probe = { d, 0, nullptr, 0, TRUE, 100, TRUE, TRUE };
		if (rx_feed(udp, &probe) != 0)
		{
			rdpeudp_test_free(udp);
			return -1;
		}
	}
	{
		const RxScriptPkt data = { 146, 1, b1, sizeof(b1), TRUE, 100, FALSE, TRUE };
		RX_CHECK(rx_feed(udp, &data) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK(st.haveSeenAoa && (st.recvDataBase == 147));
		RX_CHECK((st.expectedChannelSeq == 2) && (st.recvStreamLen == sizeof(b1)));
		RX_CHECK(st.haveRealData);
	}
	rdpeudp_test_free(udp);

	/* S2: adversarial DATA=300/AOA=1, then retransmit-as-new recovery. */
	udp = rdpeudp_test_new();
	if (!udp)
		return -1;
	{
		const RxScriptPkt adv = { 300, 1, b2, sizeof(b2), TRUE, 1, FALSE, TRUE };
		RX_CHECK(rx_feed(udp, &adv) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		/* Slide, not snap: base advances past nothing electively. */
		RX_CHECK(st.recvDataBase == 173);
		RX_CHECK(rdpeudp_test_seen(udp, 127) && !rdpeudp_test_seen(udp, 0));
		/* ChannelSeq delivery is independent of the DataSeq gap. */
		RX_CHECK((st.expectedChannelSeq == 2) && (st.recvStreamLen == sizeof(b2)));
		/* Real ACK codec on real state: gaps from 173, 300 marked,
		 * 1..172 unrepresentable (reviewer-corrected facts). */
		BOOL rec[128] = { 0 };
		for (size_t i = 0; i < 128; i++)
			rec[i] = rdpeudp_test_seen(udp, i);
		wStream* av = rdpeudp_build_ackvec(st.recvDataBase, rec, 128, FALSE, 0, 0);
		RX_CHECK(av != nullptr);
		{
			UINT16 pbase = 0;
			BOOL* pvec = nullptr;
			size_t pcount = 0;
			const BOOL pok = rdpeudp_parse_ackvec(Stream_Buffer(av), Stream_Length(av),
			                                      &pbase, &pvec, &pcount);
			RX_CHECK(pok && (pbase == 173) && (pcount > 127) && pvec && pvec[127]);
			for (size_t i = 0; i < 127; i++)
				RX_CHECK(!pvec[i]);
			free(pvec);
		}
		Stream_Release(av);
	}
	/* Sender retransmits onward under fresh DataSeqs; base must creep
	 * monotonically (no stall) and every body must deliver in order. */
	{
		UINT16 cseq = 2;
		size_t wantLen = sizeof(b2);
		static BYTE rb[1] = { 'x' };
		for (UINT16 d = 301; d <= 309; d++)
		{
			const RxScriptPkt rtx = { d, cseq, rb, sizeof(rb), TRUE, 1, FALSE, TRUE };
			RX_CHECK(rx_feed(udp, &rtx) == 0);
			cseq++;
			wantLen += sizeof(rb);
		}
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK(st.recvDataBase == 182);
		RX_CHECK((st.expectedChannelSeq == cseq) && (st.recvStreamLen == wantLen));
	}
	rdpeudp_test_free(udp);

	/* S3: N3 synthetic on real code — DATA 2/AOA=1 first keeps the gap. */
	udp = rdpeudp_test_new();
	if (!udp)
		return -1;
	{
		static const BYTE g1[] = { 'G', 'H' };
		static const BYTE g2[] = { 'I', 'J' };
		const RxScriptPkt first = { 2, 1, g1, sizeof(g1), TRUE, 1, FALSE, TRUE };
		const RxScriptPkt second = { 1, 2, g2, sizeof(g2), TRUE, 1, FALSE, TRUE };
		RX_CHECK(rx_feed(udp, &first) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK(st.recvDataBase == 1);
		RX_CHECK(st.recvStreamLen == sizeof(g1));
		RX_CHECK(rx_feed(udp, &second) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK(st.recvDataBase == 3);
		RX_CHECK((st.expectedChannelSeq == 3) &&
		         (st.recvStreamLen == sizeof(g1) + sizeof(g2)));
	}
	rdpeudp_test_free(udp);

	/* S4: mild reorder with AOA advance, then fill. */
	udp = rdpeudp_test_new();
	if (!udp)
		return -1;
	{
		static const BYTE h1[] = { 'K', 'L' };
		static const BYTE h2[] = { 'M', 'N' };
		const RxScriptPkt ahead = { 6, 1, h1, sizeof(h1), TRUE, 5, FALSE, TRUE };
		const RxScriptPkt fill = { 5, 2, h2, sizeof(h2), TRUE, 5, FALSE, TRUE };
		RX_CHECK(rx_feed(udp, &ahead) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK(st.recvDataBase == 5);
		RX_CHECK(rx_feed(udp, &fill) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK(st.recvDataBase == 7);
		RX_CHECK(st.recvStreamLen == sizeof(h1) + sizeof(h2));
	}
	rdpeudp_test_free(udp);

	/* S5: probeless far jump without AOA keeps the initial epoch snap. */
	udp = rdpeudp_test_new();
	if (!udp)
		return -1;
	{
		static const BYTE k1[] = { 'O', 'P' };
		const RxScriptPkt far = { 150, 1, k1, sizeof(k1), FALSE, 0, FALSE, TRUE };
		RX_CHECK(rx_feed(udp, &far) == 0);
		RX_CHECK(rx_state(udp, &st) == 0);
		RX_CHECK((st.recvDataBase == 151) && !st.haveSeenAoa);
		RX_CHECK(st.recvStreamLen == sizeof(k1));
	}
	rdpeudp_test_free(udp);
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
	if (test_tunnel_consume() != 0)
	{
		(void)fprintf(stderr, "test_tunnel_consume FAILED\n");
		return -1;
	}
	if (test_soft_sync_shared_parity() != 0)
	{
		(void)fprintf(stderr, "test_soft_sync_shared_parity FAILED\n");
		return -1;
	}
	if (test_dvc_pdu_length() != 0)
	{
		(void)fprintf(stderr, "test_dvc_pdu_length FAILED\n");
		return -1;
	}
	if (test_rx_integration() != 0)
	{
		(void)fprintf(stderr, "test_rx_integration FAILED\n");
		return -1;
	}

	(void)printf("TestRdpeUdp passed\n");
	return 0;
}

