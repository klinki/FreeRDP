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
	const BYTE pdu[] = { 0x06, 0x00, 0x34, 0x12, 0x01, 0x00 };
	wStream* s = rdpeudp_build_autodetect_packet(TRUE, 0x1000, pdu, sizeof(pdu));
	if (!s)
		return -1;
	BOOL isReq = FALSE;
	UINT16 sec = 0;
	const BYTE* out = nullptr;
	size_t outLen = 0;
	if (!rdpeudp_parse_autodetect_packet(Stream_Buffer(s), Stream_Length(s), &isReq, &sec,
	                                     &out, &outLen))
	{
		(void)fprintf(stderr, "autodetect parse failed\n");
		Stream_Release(s);
		return -1;
	}
	if (!isReq || (sec != 0x1000) || (outLen != sizeof(pdu)) || (memcmp(out, pdu, outLen) != 0))
	{
		(void)fprintf(stderr, "autodetect mismatch\n");
		Stream_Release(s);
		return -1;
	}
	Stream_Release(s);

	/* Channel ptype must be distinct */
	BYTE pt = 0xFF;
	BYTE chbuf[16] = { 0x00, 0x05, 0x00 };
	if (!rdpeudp_parse_tunnel_ptype(chbuf, sizeof(chbuf), &pt) || (pt != 0x00))
	{
		(void)fprintf(stderr, "ptype channel mismatch\n");
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

	(void)printf("TestRdpeUdp passed\n");
	return 0;
}
