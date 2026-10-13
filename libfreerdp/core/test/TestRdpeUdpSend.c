/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Deterministic regressions for the production RDP-UDP2 reliable sender
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
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <freerdp/transport_io.h>

#include "../rdpeudp.h"
#include "../udp.h"

#define SEND_TEST_TIME 1000
#define SEND_TEST_CAPTURE_COUNT 256

typedef struct
{
	UINT16 dataSeq;
	UINT16 channelSeq;
	BYTE logWindow;
	UINT64 time;
	size_t len;
	BYTE payload[RDPUDP2_MAX_PAYLOAD];
} SendPacket;

typedef struct
{
	rdpUdpTransport* sender;
	rdpUdpTransport* receiver;
	HANDLE abortEvent;
	UINT64 now;
	BYTE peerLogWindow;
	size_t count;
	size_t failPacket;
	size_t dropPackets;
	BOOL deliver;
	BOOL callbackFailed;
	SendPacket packets[SEND_TEST_CAPTURE_COUNT];
} SendFixture;

/* Observe the actual protected DATA datagrams. ACKs fed by the script use
 * the production wire codec and receive path; no sender slots are mutated. */
static SSIZE_T send_capture(void* context, const BYTE* data, size_t len)
{
	SendFixture* fixture = context;
	BYTE packet[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
	BOOL dummy = FALSE;
	size_t offset = 0;
	RdpUdp2Layout layout = { 0 };
	if ((len > sizeof(packet)) || !fixture)
		return -1;
	memcpy(packet, data, len);
	if (!rdpeudp2_unprotect(packet, len, &dummy, &offset) ||
	    !rdpeudp2_parse_layout(packet + offset, len - offset, &layout))
	{
		fixture->callbackFailed = TRUE;
		return -1;
	}
	if (dummy || !layout.hasDataBody)
		return (SSIZE_T)len;
	if ((fixture->count >= ARRAYSIZE(fixture->packets)) ||
	    (layout.dataBodyLen > RDPUDP2_MAX_PAYLOAD))
	{
		fixture->callbackFailed = TRUE;
		return -1;
	}
	SendPacket* saved = &fixture->packets[fixture->count++];
	saved->dataSeq = layout.dataSeq;
	saved->channelSeq = layout.channelSeq;
	saved->logWindow = layout.logWindow;
	saved->time = fixture->now;
	saved->len = layout.dataBodyLen;
	memcpy(saved->payload, layout.dataBody, layout.dataBodyLen);
	if (fixture->failPacket && (fixture->count == fixture->failPacket))
		return -1;
	if (fixture->deliver && (fixture->count > fixture->dropPackets) &&
	    !rdpeudp_test_feed(fixture->receiver, data, len))
	{
		fixture->callbackFailed = TRUE;
		return -1;
	}
	return (SSIZE_T)len;
}

static void send_fixture_free(SendFixture* fixture)
{
	if (!fixture)
		return;
	rdpeudp_test_free(fixture->sender);
	rdpeudp_test_free(fixture->receiver);
	if (fixture->abortEvent)
		CloseHandle(fixture->abortEvent);
	free(fixture);
}

static SendFixture* send_fixture_new(UINT16 payload, BYTE logWindow, BYTE peerLogWindow,
                                     UINT16 dataSeq, UINT16 channelSeq)
{
	SendFixture* fixture = calloc(1, sizeof(*fixture));
	if (!fixture)
		return nullptr;
	fixture->sender = rdpeudp_test_new();
	fixture->receiver = rdpeudp_test_new();
	fixture->now = SEND_TEST_TIME;
	fixture->peerLogWindow = peerLogWindow;
	if (!fixture->sender || !fixture->receiver ||
	    !rdpeudp_test_configure_send(fixture->sender, payload, logWindow, peerLogWindow,
	                                 dataSeq, channelSeq))
	{
		send_fixture_free(fixture);
		return nullptr;
	}
	rdpeudp_test_set_time(fixture->sender, fixture->now);
	rdpeudp_test_set_time(fixture->receiver, fixture->now);
	rdpeudp_test_set_send(fixture->sender, send_capture, fixture);
	return fixture;
}

static BOOL send_tick(SendFixture* fixture, UINT64 elapsed)
{
	fixture->now = SEND_TEST_TIME + elapsed;
	rdpeudp_test_set_time(fixture->sender, fixture->now);
	rdpeudp_test_set_time(fixture->receiver, fixture->now);
	return rdpeudp_test_service_send(fixture->sender);
}

static BOOL send_feed_transport(rdpUdpTransport* transport, const RdpUdp2Layout* layout, BOOL dummy)
{
	wStream* packet = rdpeudp2_encode_layout(layout);
	if (!packet)
		return FALSE;
	BOOL ok = Stream_SetPosition(packet, Stream_Length(packet));
	if (ok)
		ok = rdpeudp2_protect(packet, dummy);
	if (ok)
		ok = rdpeudp_test_feed(transport, Stream_Buffer(packet), Stream_Length(packet));
	Stream_Release(packet);
	return ok;
}

static BOOL send_feed_layout_full(SendFixture* fixture, const RdpUdp2Layout* layout, BOOL dummy)
{
	return send_feed_transport(fixture->sender, layout, dummy);
}

static BOOL send_feed_layout(SendFixture* fixture, const RdpUdp2Layout* layout)
{
	return send_feed_layout_full(fixture, layout, FALSE);
}

static BOOL send_ack(SendFixture* fixture, UINT16 dataSeq)
{
	RdpUdp2Layout layout = { 0 };
	layout.logWindow = fixture->peerLogWindow;
	layout.flags = RDPUDP2_FLAG_ACK;
	layout.hasAck = TRUE;
	layout.ackBase = dataSeq;
	return send_feed_layout(fixture, &layout);
}

/* The peer is alive but every DATA receipt is lost. Heartbeats must not be
 * mistaken for progress on a logical payload or a reason to shorten its RTO. */
static BOOL send_live_tick(SendFixture* fixture, UINT64 elapsed)
{
	while (SEND_TEST_TIME + elapsed - fixture->now > RDPEUDP_KEEPALIVE_MS)
	{
		fixture->now += RDPEUDP_KEEPALIVE_MS;
		rdpeudp_test_set_time(fixture->sender, fixture->now);
		if (!send_ack(fixture, 0) || !rdpeudp_test_service_send(fixture->sender))
			return FALSE;
	}
	fixture->now = SEND_TEST_TIME + elapsed;
	rdpeudp_test_set_time(fixture->sender, fixture->now);
	return send_ack(fixture, 0) && send_tick(fixture, elapsed);
}

static BOOL send_ack_aoa(SendFixture* fixture, UINT16 dataSeq, UINT16 aoa)
{
	RdpUdp2Layout layout = { 0 };
	layout.logWindow = fixture->peerLogWindow;
	layout.flags = RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_AOA;
	layout.hasAck = TRUE;
	layout.ackBase = dataSeq;
	layout.hasAoa = TRUE;
	layout.aoa = aoa;
	return send_feed_layout(fixture, &layout);
}

static BOOL send_ackvec(SendFixture* fixture, UINT16 base, const BOOL* received, size_t count)
{
	wStream* vector = rdpeudp_build_ackvec(base, received, count, FALSE, 0, 0);
	if (!vector)
		return FALSE;
	RdpUdp2Layout layout = { 0 };
	layout.logWindow = fixture->peerLogWindow;
	layout.flags = RDPUDP2_FLAG_ACKVEC;
	layout.hasAckvec = TRUE;
	layout.ackvec = Stream_Buffer(vector);
	layout.ackvecLen = Stream_Length(vector);
	const BOOL ok = send_feed_layout(fixture, &layout);
	Stream_Release(vector);
	return ok;
}

static BOOL send_received(SendFixture* fixture, const BYTE* expected, size_t len)
{
	RdpUdpTestRecvState state = { 0 };
	BYTE* actual = malloc(len ? len : 1);
	if (!actual)
		return FALSE;
	const BOOL ok = rdpeudp_test_recv_state(fixture->receiver, &state) &&
	                (state.recvStreamLen == len) &&
	                rdpeudp_test_recv_data(fixture->receiver, 0, actual, len) &&
	                (memcmp(actual, expected, len) == 0);
	free(actual);
	return ok;
}

#define SEND_CHECK(condition)                                                                    \
	do                                                                                           \
	{                                                                                            \
		if (!(condition))                                                                        \
		{                                                                                        \
			(void)fprintf(stderr, "%s FAILED line %d: %s\n", __func__, __LINE__, #condition); \
			send_fixture_free(fixture);                                                           \
			return FALSE;                                                                         \
		}                                                                                        \
	} while (0)

static BOOL test_send_pipeline(void)
{
	static const BYTE bytes[] = "ABCDEFGHIJKL";
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes) - 1) == 12);
	/* No ACK has been injected. All three packets must already be on wire. */
	SEND_CHECK(fixture->count == 3 && !fixture->callbackFailed);
	for (size_t i = 0; i < fixture->count; i++)
	{
		SEND_CHECK(fixture->packets[i].dataSeq == i + 1);
		SEND_CHECK(fixture->packets[i].channelSeq == i + 1);
		SEND_CHECK(fixture->packets[i].len == 4);
		SEND_CHECK(memcmp(fixture->packets[i].payload, bytes + i * 4, 4) == 0);
	}
	SEND_CHECK(send_received(fixture, bytes, sizeof(bytes) - 1));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 3);
	/* A cumulative ACK must retire every earlier channel, including when
 * the ACK's own DataSeq still has an exact retained match. */
	SEND_CHECK(send_ack(fixture, 2));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1);
	SEND_CHECK(send_ack(fixture, 3));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 0 &&
	           state.queuedBytes == 0 && state.aliasCount == 0);
	SEND_CHECK(send_tick(fixture, 2000) && fixture->count == 3);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_window_and_selective_ack(void)
{
	static const BYTE bytes[] = "ABCDEFGHIJ";
	static const BOOL first[] = { TRUE, FALSE, FALSE };
	static const BOOL gap[] = { FALSE, TRUE, FALSE };
	SendFixture* fixture = send_fixture_new(1, 5, 2, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes) - 1) == 10);
	SEND_CHECK(fixture->count == 3);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 3);
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 3);
	SEND_CHECK(send_ackvec(fixture, 1, first, ARRAYSIZE(first)));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 4);
	SEND_CHECK(fixture->packets[3].dataSeq == 4 && fixture->packets[3].channelSeq == 4);
	/* Selective ACK of packet 3 cannot free the gap at packet 2. Even if
 * a logical slot is free, the active DataSeq span still caps packet 5. */
	SEND_CHECK(send_ackvec(fixture, 2, gap, ARRAYSIZE(gap)));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 4);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 2);
	SEND_CHECK(send_ack(fixture, 2));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 6);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 3);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_window_shrink(void)
{
	BYTE bytes[40] = { 0 };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes)) ==
	           (SSIZE_T)sizeof(bytes));
	SEND_CHECK(fixture->count == 31);
	/* Advertise a smaller peer window while a larger write is queued.
 * ACKing all but its last three packets must not emit another packet. */
	fixture->peerLogWindow = 2;
	SEND_CHECK(send_ack(fixture, 28));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 31);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 3);
	SEND_CHECK(send_ack(fixture, 29));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 32);
	SEND_CHECK(fixture->packets[31].dataSeq == 32 && fixture->packets[31].channelSeq == 32);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 3);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_window_shrink_with_loss(void)
{
	BYTE bytes[40] = { 0 };
	BOOL missing[31] = { 0 };
	UINT16 latest[31] = { 0 };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState sent = { 0 };
	RdpUdpTestRecvState received = { 0 };
	SEND_CHECK(fixture);
	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (BYTE)(i + 1);
	/* All 31 original packets are lost while nine later channel chunks
	 * remain queued. The peer then shrinks its window to three packets. */
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes)) ==
	           (SSIZE_T)sizeof(bytes));
	SEND_CHECK(fixture->count == 31);
	fixture->peerLogWindow = 2;
	SEND_CHECK(send_ack(fixture, 0));
	SEND_CHECK(send_ackvec(fixture, 1, missing, ARRAYSIZE(missing)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 31 &&
	           sent.queuedBytes == sizeof(bytes) && sent.maxRetries == 0);
	SEND_CHECK(send_tick(fixture, 299) && fixture->count == 31);
	/* Existing transmissions may replace their oldest active attempt
	 * without expanding the grandfathered span. New channels stay blocked. */
	fixture->deliver = TRUE;
	SEND_CHECK(send_tick(fixture, 300) && fixture->count == 62);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 31 &&
	           sent.aliasCount == 62 && sent.maxRetries == 1 && sent.nextDataSeq == 63 &&
	           sent.nextChannelSeq == 32 && sent.queuedBytes == sizeof(bytes));
	for (size_t i = 0; i < fixture->count; i++)
	{
		const SendPacket* packet = &fixture->packets[i];
		SEND_CHECK(packet->channelSeq >= 1 && packet->channelSeq <= ARRAYSIZE(latest));
		SEND_CHECK(packet->dataSeq == i + 1 && packet->len == 1 &&
		           packet->payload[0] == bytes[packet->channelSeq - 1]);
		if (i >= ARRAYSIZE(latest))
		{
			SEND_CHECK(packet->channelSeq == i - ARRAYSIZE(latest) + 1 &&
			           packet->time == SEND_TEST_TIME + 300);
		}
		latest[packet->channelSeq - 1] = packet->dataSeq;
		size_t span = 0;
		const UINT16 next = (UINT16)(packet->dataSeq + 1);
		for (size_t j = 0; j < ARRAYSIZE(latest); j++)
		{
			if (!latest[j])
				continue;
			const size_t age = (UINT16)(next - latest[j]);
			if (age > span)
				span = age;
		}
		SEND_CHECK(span <= ARRAYSIZE(latest));
		if (i >= ARRAYSIZE(latest))
			SEND_CHECK(span == ARRAYSIZE(latest));
	}
	SEND_CHECK(send_received(fixture, bytes, 31));
	SEND_CHECK(rdpeudp_test_recv_state(fixture->receiver, &received) &&
	           received.recvDataBase == 63);
	/* Once the retries are actually ACKed, queued originals resume under
	 * the smaller peer limit and every accepted byte reaches the receiver. */
	SEND_CHECK(send_ack(fixture, (UINT16)(received.recvDataBase - 1)));
	SEND_CHECK(send_tick(fixture, 300) && fixture->count == 65);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 3 &&
	           sent.nextChannelSeq == 35);
	for (size_t i = 0; i < 8 && sent.queuedBytes; i++)
	{
		const size_t count = fixture->count;
		SEND_CHECK(rdpeudp_test_recv_state(fixture->receiver, &received));
		SEND_CHECK(send_ack(fixture, (UINT16)(received.recvDataBase - 1)));
		SEND_CHECK(send_tick(fixture, 300));
		SEND_CHECK(fixture->count - count <= 3);
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight <= 3);
	}
	SEND_CHECK(sent.queuedBytes == 0 && sent.inFlight == 0 && sent.aliasCount == 0 &&
	           !sent.sendFailed);
	SEND_CHECK(fixture->count == 71 && send_received(fixture, bytes, sizeof(bytes)));
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 71);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_window_storage_cap(void)
{
	BYTE bytes[80] = { 0 };
	SendFixture* fixture = send_fixture_new(1, 10, 15, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
	           state.localLogWindow == 10 && state.peerLogWindow == 15 && state.sendWindow == 63);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes)) ==
	           (SSIZE_T)sizeof(bytes));
	SEND_CHECK(fixture->count == 63);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 63);
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 63);
	SEND_CHECK(send_ack(fixture, 63));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 80);
	SEND_CHECK(send_ack(fixture, 80));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 0 &&
	           state.aliasCount == 0 && state.queuedBytes == 0);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_peer_window_tracking(void)
{
	/* The existing Microsoft wire fixture, with its payload replaced by
	 * AA BB: LogWindow=15, DATA|AOA|DELAYACK (plus its reserved flag),
	 * initial DataSeq=100, ChannelSeq=1, delayed-ACK allowance 1/20 ms.
	 * Feeding wire bytes avoids testing only our own encoder's output. */
	static const BYTE msWire[] = {
		0x00, 0x14, 0xF3, 0x01, 0x14, 0x00, 0x64, 0xE0, 0x64, 0x00, 0x01, 0x00,
		0xAA, 0xBB
	};
	BYTE bytes[40] = { 0 };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	/* The untouched receiver exposes production defaults independently of
	 * the sender's test configuration. A peer advertisement cannot enlarge
	 * either endpoint's local advertisement or packet-storage capacity. */
	SEND_CHECK(rdpeudp_test_send_state(fixture->receiver, &state) &&
	           state.localLogWindow == 5 && state.peerLogWindow == 5 && state.sendWindow == 31);
	SEND_CHECK(rdpeudp_test_feed(fixture->receiver, msWire, sizeof(msWire)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->receiver, &state) &&
	           state.localLogWindow == 5 && state.peerLogWindow == 15 && state.sendWindow == 31);
	SEND_CHECK(!rdpeudp_test_configure_send(fixture->sender, 1, 5, 0, 1, 1));
	SEND_CHECK(!rdpeudp_test_configure_send(fixture->sender, 1, 5, 1, 1, 1));
	SEND_CHECK(!rdpeudp_test_configure_send(fixture->sender, 1, 5, 16, 1, 1));
	SEND_CHECK(!rdpeudp_test_configure_send(fixture->sender, 1, 11, 15, 1, 1));
	SEND_CHECK(!rdpeudp_test_configure_send(fixture->sender, 1, 15, 15, 1, 1));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
	           state.localLogWindow == 5 && state.peerLogWindow == 5 && state.sendWindow == 31);
	SEND_CHECK(rdpeudp_test_feed(fixture->sender, msWire, sizeof(msWire)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
	           state.localLogWindow == 5 && state.peerLogWindow == 15 && state.sendWindow == 31);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes)) ==
	           (SSIZE_T)sizeof(bytes));
	SEND_CHECK(fixture->count == 31);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 31);
	/* All protocol-valid peer windows must be retained, including 11..15,
	 * while local capacity bounds the effective sender window. */
	for (BYTE peer = 2; peer <= 15; peer++)
	{
		fixture->peerLogWindow = peer;
		SEND_CHECK(send_ack(fixture, 0));
		const size_t expected = peer < 5 ? ((size_t)1 << peer) - 1 : 31;
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
		           state.localLogWindow == 5 && state.peerLogWindow == peer &&
		           state.sendWindow == expected && state.inFlight == 31);
	}
	/* Invalid advertisements do not erase the last valid peer capacity. */
	for (BYTE peer = 0; peer < 2; peer++)
	{
		fixture->peerLogWindow = peer;
		SEND_CHECK(send_ack(fixture, 0));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
		           state.peerLogWindow == 15 && state.sendWindow == 31);
	}
	/* Shrink and recover from a Windows-sized advertisement while bytes are
	 * queued: the peer limit must govern admission without changing ours. */
	fixture->peerLogWindow = 2;
	SEND_CHECK(send_ack(fixture, 28));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 31);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 3 &&
	           state.peerLogWindow == 2 && state.sendWindow == 3);
	fixture->peerLogWindow = 15;
	SEND_CHECK(send_ack(fixture, 28));
	SEND_CHECK(send_tick(fixture, 0) && fixture->count == 40);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 12 &&
	           state.peerLogWindow == 15 && state.sendWindow == 31);
	for (size_t i = 0; i < fixture->count; i++)
		SEND_CHECK(fixture->packets[i].logWindow == 5);
	SEND_CHECK(send_ack(fixture, 40));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 0 &&
	           state.queuedBytes == 0 && !state.sendFailed);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_atomic_queue_and_owned_payload(void)
{
	BYTE bytes[60000] = { 0 };
	BYTE expected[60000] = { 0 };
	static const BYTE extra = 0xA5;
	SendFixture* fixture = send_fixture_new(RDPUDP2_MAX_PAYLOAD, 5, 5, 1, 1);
	RdpUdpTestSendState before = { 0 };
	RdpUdpTestSendState after = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (BYTE)(i * 17 + i / 251);
	memcpy(expected, bytes, sizeof(bytes));
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, sizeof(bytes)) ==
	           (SSIZE_T)sizeof(bytes));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &before));
	SEND_CHECK(before.queuedBytes == sizeof(bytes) && before.inFlight == 31);
	const size_t count = fixture->count;
	/* Admission failure happens before either sequence space or wire
 * state changes, so the caller can safely retry this unaccepted write. */
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, &extra, 1) == -2);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &after));
	SEND_CHECK(after.queuedBytes == before.queuedBytes && after.inFlight == before.inFlight &&
	           after.nextDataSeq == before.nextDataSeq &&
	           after.nextChannelSeq == before.nextChannelSeq && fixture->count == count);
	SEND_CHECK(rdpeudp_test_check_health(fixture->sender));
	/* Mutate the caller's storage after acceptance. The deferred portion
 * must still deliver the accepted bytes, including all later chunks. */
	memset(bytes, 0xCC, sizeof(bytes));
	for (size_t i = 0; i < 8 && after.queuedBytes; i++)
	{
		SEND_CHECK(fixture->count > 0);
		SEND_CHECK(send_ack(fixture, fixture->packets[fixture->count - 1].dataSeq));
		SEND_CHECK(send_tick(fixture, 0));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &after));
	}
	SEND_CHECK(after.queuedBytes == 0 && after.inFlight == 0 && after.aliasCount == 0);
	SEND_CHECK(send_received(fixture, expected, sizeof(expected)));
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, &extra, 1) == 1);
	SEND_CHECK(fixture->packets[fixture->count - 1].payload[0] == extra);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_late_original_ack(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BOOL positive[] = { TRUE };
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 299) && fixture->count == 1);
	SEND_CHECK(send_tick(fixture, 300) && fixture->count == 2);
	SEND_CHECK(fixture->packets[0].dataSeq != fixture->packets[1].dataSeq &&
	           fixture->packets[0].channelSeq == fixture->packets[1].channelSeq &&
	           memcmp(fixture->packets[0].payload, fixture->packets[1].payload, 4) == 0);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1 &&
	           state.aliasCount == 2 && state.maxRetries == 1);
	/* The DATA arrived twice because its ACK was lost. A cumulative ACK
	 * covering only the retired original does not prove the latest attempt
	 * arrived: AOA may have advanced the peer across a lost original. */
	SEND_CHECK(send_received(fixture, bytes, 4));
	SEND_CHECK(send_ack(fixture, fixture->packets[0].dataSeq));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1 &&
	           state.aliasCount == 2 && state.queuedBytes == 4);
	/* A positive selective ACK for the original is explicit receipt
	 * evidence and must retire both aliases without duplicate bytes. */
	SEND_CHECK(send_ackvec(fixture, fixture->packets[0].dataSeq, positive, ARRAYSIZE(positive)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 0 &&
	           state.aliasCount == 0 && state.queuedBytes == 0);
	SEND_CHECK(send_ack(fixture, fixture->packets[1].dataSeq));
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 2);
	SEND_CHECK(send_received(fixture, bytes, 4));
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_lost_data(void)
{
	static const BYTE bytes[] = "ABCD";
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	fixture->dropPackets = 1;
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_received(fixture, bytes, 0));
	SEND_CHECK(send_tick(fixture, 300) && fixture->count == 2);
	SEND_CHECK(fixture->packets[1].channelSeq == 1 && fixture->packets[1].dataSeq == 2);
	SEND_CHECK(send_received(fixture, bytes, 4));
	SEND_CHECK(send_ack(fixture, fixture->packets[1].dataSeq));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.queuedBytes == 0 &&
	           state.inFlight == 0 && state.aliasCount == 0);
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 2);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_fast_loss_evidence(void)
{
	static const BYTE bytes[] = "ABCDEFGH";
	static const BOOL first[] = { FALSE, TRUE };
	static const BOOL second[] = { FALSE, FALSE, TRUE };
	static const BOOL third[] = { FALSE, FALSE, FALSE, TRUE };
	static const BOOL positive[] = { TRUE };
	static const BOOL future[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
	                              FALSE, FALSE, TRUE, TRUE, TRUE };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 8) == 8 && fixture->count == 8);
	/* Forged future positives cannot create evidence for a generated gap. */
	SEND_CHECK(send_ackvec(fixture, 1, future, ARRAYSIZE(future)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.lossReports == 0);
	SEND_CHECK(send_tick(fixture, 30));
	SEND_CHECK(send_ackvec(fixture, 1, first, ARRAYSIZE(first)));
	/* The final bitmap's padded negatives must not label packets 3..7 lost.
	 * Repeated vectors with one later positive cannot count as three. */
	for (size_t i = 0; i < 5; i++)
		SEND_CHECK(send_ackvec(fixture, 1, first, ARRAYSIZE(first)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.lossReports == 1);
	SEND_CHECK(send_tick(fixture, 54) && fixture->count == 8);
	SEND_CHECK(send_ackvec(fixture, 1, second, ARRAYSIZE(second)));
	SEND_CHECK(send_tick(fixture, 54) && fixture->count == 8);
	SEND_CHECK(send_ackvec(fixture, 1, third, ARRAYSIZE(third)));
	/* Three distinct receipts permit a retry only once the 25 ms grace
	 * since the first meaningful gap expires, rather than since bit three. */
	SEND_CHECK(send_tick(fixture, 54) && fixture->count == 8);
	SEND_CHECK(send_tick(fixture, 55) && fixture->count == 9);
	SEND_CHECK(fixture->packets[8].dataSeq == 9 && fixture->packets[8].channelSeq == 1 &&
	           fixture->packets[8].time == SEND_TEST_TIME + 55 &&
	           fixture->packets[8].payload[0] == bytes[0]);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 5 &&
	           state.aliasCount == 6 && state.maxRetries == 1);
	/* A delayed positive receipt of the original remains valid after its
	 * fast retry, and releases both aliases as one logical payload. */
	SEND_CHECK(send_ackvec(fixture, 1, positive, ARRAYSIZE(positive)));
	SEND_CHECK(send_ack(fixture, 9));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.inFlight &&
	           !state.queuedBytes && !state.aliasCount);
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 9 && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_fast_loss_reordered_receipt(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BOOL gap[] = { FALSE, TRUE, TRUE, TRUE };
	static const BOOL original[] = { TRUE };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 30));
	SEND_CHECK(send_ackvec(fixture, 1, gap, ARRAYSIZE(gap)));
	SEND_CHECK(send_tick(fixture, 45) && fixture->count == 4);
	/* Reordering for 15 ms after the first gap closes inside the grace. */
	SEND_CHECK(send_ackvec(fixture, 1, original, ARRAYSIZE(original)));
	SEND_CHECK(send_tick(fixture, 55) && fixture->count == 4);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.inFlight &&
	           !state.queuedBytes && !state.maxRetries);
	SEND_CHECK(send_tick(fixture, 1000) && fixture->count == 4 && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_fast_loss_delivers_ordered_bytes(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BOOL gap[] = { FALSE, TRUE, TRUE, TRUE };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	RdpUdpTestRecvState received = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	fixture->dropPackets = 1;
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_received(fixture, bytes, 0));
	SEND_CHECK(send_tick(fixture, 30) && send_ackvec(fixture, 1, gap, ARRAYSIZE(gap)));
	SEND_CHECK(send_tick(fixture, 55) && fixture->count == 5);
	/* Recovering the first channel under a new DataSeq must close the byte
	 * stream gap and deliver all four bytes once, in their original order. */
	SEND_CHECK(send_received(fixture, bytes, 4));
	SEND_CHECK(rdpeudp_test_recv_state(fixture->receiver, &received) &&
	           received.expectedChannelSeq == 5 && received.recvDataBase == 6);
	SEND_CHECK(send_ack(fixture, 5));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.inFlight &&
	           !state.aliasCount && !state.queuedBytes && !state.sendFailed);
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 5 &&
	           send_received(fixture, bytes, 4) && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_fast_loss_attempt_reset(void)
{
	static const BYTE bytes[] = "ABCDEFG";
	static const BOOL gap[] = { FALSE, TRUE, TRUE, TRUE };
	static const BOOL resetGap[] = { FALSE, TRUE, TRUE, TRUE };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 30) && send_ackvec(fixture, 1, gap, ARRAYSIZE(gap)));
	SEND_CHECK(send_tick(fixture, 55) && fixture->count == 5);
	SEND_CHECK(fixture->packets[4].dataSeq == 5 && fixture->packets[4].channelSeq == 1);
	/* Old alias negatives plus old positive evidence cannot arm the retry.
	 * The latest attempt needs fresh later receipts and its own grace. */
	SEND_CHECK(send_ackvec(fixture, 1, gap, ARRAYSIZE(gap)));
	SEND_CHECK(send_tick(fixture, 100) && fixture->count == 5);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes + 4, 3) == 3 && fixture->count == 8);
	SEND_CHECK(send_ackvec(fixture, 5, resetGap, ARRAYSIZE(resetGap)));
	SEND_CHECK(send_tick(fixture, 124) && fixture->count == 8);
	SEND_CHECK(send_tick(fixture, 125) && fixture->count == 9);
	SEND_CHECK(fixture->packets[8].dataSeq == 9 && fixture->packets[8].channelSeq == 1);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1 &&
	           state.maxRetries == 2 && state.aliasCount == 3);
	SEND_CHECK(send_ack(fixture, 9));
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 9 && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_fast_loss_wrap_shrink_and_multiple_gaps(void)
{
	static const BYTE bytes[] = "ABCDEFGH";
	static const BOOL gap[] = { FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE };
	SendFixture* fixture = send_fixture_new(1, 5, 5, UINT16_MAX - 1, UINT16_MAX - 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 8) == 8);
	fixture->peerLogWindow = 2;
	SEND_CHECK(send_tick(fixture, 30));
	SEND_CHECK(send_ackvec(fixture, UINT16_MAX - 1, gap, ARRAYSIZE(gap)));
	SEND_CHECK(send_tick(fixture, 54) && fixture->count == 8);
	/* Two missing originals span the sequence wrap while the peer shrinks
	 * to three active packets. Recovery may replace them, never add channels. */
	SEND_CHECK(send_tick(fixture, 55) && fixture->count == 10);
	SEND_CHECK(fixture->packets[8].dataSeq == 6 &&
	           fixture->packets[8].channelSeq == UINT16_MAX - 1 &&
	           fixture->packets[9].dataSeq == 7 &&
	           fixture->packets[9].channelSeq == UINT16_MAX);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 2 &&
	           state.sendWindow == 3 && state.peerLogWindow == 2 && state.maxRetries == 1 &&
	           state.nextChannelSeq == 6 && state.aliasCount == 4);
	SEND_CHECK(send_ack(fixture, 7));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.queuedBytes &&
	           !state.inFlight && !state.aliasCount && !state.sendFailed);
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 10 && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_idle_health_pump(void)
{
	static const BYTE bytes[] = "ABCD";
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	fixture->now = SEND_TEST_TIME + RDPEUDP_ACK_TIMEOUT_MS;
	rdpeudp_test_set_time(fixture->sender, fixture->now);
	/* No new write, receive packet, or explicit sender tick. The ordinary
 * transport health path must service the pending sender while idle. */
	SEND_CHECK(rdpeudp_test_check_health(fixture->sender) && fixture->count == 2);
	SEND_CHECK(fixture->packets[1].channelSeq == 1 && fixture->packets[1].dataSeq == 2);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_repeated_loss(void)
{
	static const BYTE bytes[] = "ABCD";
	static const UINT64 deadlines[] = { 300,   900,   2100,  4500,   9300,
		                                18900, 38100, 76500, 153300, 273300 };
	static const UINT32 backedOff[] = { 600,   1200,  2400,  4800,   9600,
		                                19200, 38400, 76800, 120000, 120000 };
	for (size_t ackLoss = 0; ackLoss < 2; ackLoss++)
	{
		SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
		RdpUdpTestSendState state = { 0 };
		SEND_CHECK(fixture);
		fixture->deliver = (ackLoss != 0);
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
		for (size_t i = 0; i < ARRAYSIZE(deadlines); i++)
		{
			SEND_CHECK(send_live_tick(fixture, deadlines[i] - 1) && fixture->count == i + 1);
			SEND_CHECK(send_live_tick(fixture, deadlines[i]) && fixture->count == i + 2);
			SEND_CHECK(fixture->packets[i + 1].dataSeq == i + 2 &&
			           fixture->packets[i + 1].channelSeq == 1 &&
			           fixture->packets[i + 1].time == SEND_TEST_TIME + deadlines[i]);
			SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
			           state.maxRetries == i + 1 && state.inFlight == 1 &&
			           state.aliasCount == i + 2 && state.maxRtoMs == backedOff[i] &&
			           state.srttMs == 0);
		}
		SEND_CHECK(send_live_tick(fixture, 393299) && fixture->count == 11);
		/* A live peer permits capped 120 s timers, but never an eleventh retry. */
		SEND_CHECK(!send_live_tick(fixture, 393300) && fixture->count == 11);
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.sendFailed);
		SEND_CHECK(!rdpeudp_test_check_health(fixture->sender));
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == -1);
		SEND_CHECK(!send_tick(fixture, 400000) && fixture->count == 11);
		SEND_CHECK(send_received(fixture, bytes, ackLoss ? 4 : 0));
		SEND_CHECK(!fixture->callbackFailed);
		send_fixture_free(fixture);
	}
	return TRUE;
}

static BOOL test_send_adaptive_rtt_and_karn(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BOOL positive[] = { TRUE };
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.initialRtoMs == 300 &&
	           state.srttMs == 0);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 240) && send_ack(fixture, 1));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.srttMs == 240 &&
	           state.initialRtoMs == 480);
	/* The next unambiguous ACK updates SRTT as (7*240 + 160)/8. */
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 400) && send_ack(fixture, 2));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.srttMs == 230 &&
	           state.initialRtoMs == 460);
	SEND_CHECK(send_ack(fixture, 200)); /* impossible future ACK provides no sample */
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 859) && fixture->count == 3);
	SEND_CHECK(send_tick(fixture, 860) && fixture->count == 4);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.maxRtoMs == 920);
	/* A positive old alias still retires the payload, but cannot sample RTT. */
	SEND_CHECK(send_tick(fixture, 880) && send_ackvec(fixture, 3, positive, ARRAYSIZE(positive)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.inFlight &&
	           state.srttMs == 230 && state.initialRtoMs == 460);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 1340) && fixture->count == 6);
	SEND_CHECK(send_tick(fixture, 1360) && send_ack(fixture, 6));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.inFlight &&
	           state.srttMs == 230 && state.initialRtoMs == 460 && !state.sendFailed);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_peer_silence_and_idle(void)
{
	static const BYTE bytes[] = "ABCD";
	for (size_t pending = 0; pending < 2; pending++)
	{
		SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
		RdpUdpTestSendState state = { 0 };
		SEND_CHECK(fixture);
		if (pending)
			SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
		SEND_CHECK(send_tick(fixture, RDPEUDP_PEER_TIMEOUT_MS - 1));
		/* Invalid input at the edge cannot refresh liveness. */
		const BYTE invalid[] = { 0xFF };
		SEND_CHECK(rdpeudp_test_feed(fixture->sender, invalid, sizeof(invalid)));
		SEND_CHECK(!send_tick(fixture, RDPEUDP_PEER_TIMEOUT_MS));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.sendFailed);
		SEND_CHECK(send_ack(fixture, 0)); /* terminal failure cannot revive the tunnel */
		SEND_CHECK(!rdpeudp_test_check_health(fixture->sender));
		send_fixture_free(fixture);
	}
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	/* A desktop idle for minutes remains healthy when keepalives arrive;
	 * no queued channel data means there is no incoming reassembly deadline. */
	SEND_CHECK(send_live_tick(fixture, 300000));
	SEND_CHECK(rdpeudp_test_check_health(fixture->sender));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.sendFailed &&
	           !state.inFlight && state.lastRecvTs == SEND_TEST_TIME + 300000);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_malformed_ack_cannot_refresh_peer(void)
{
	static const BYTE bytes[] = "ABCD";
	static const UINT16 flags[] = { RDPUDP2_FLAG_ACKVEC, RDPUDP2_FLAG_ACKVEC,
		                            RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_ACKVEC,
		                            RDPUDP2_FLAG_AOA | RDPUDP2_FLAG_DELAYACK | RDPUDP2_FLAG_ACKVEC,
		                            RDPUDP2_FLAG_DATA | RDPUDP2_FLAG_ACKVEC };
	for (size_t test = 0; test < ARRAYSIZE(flags); test++)
	{
		SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
		RdpUdpTestSendState sent = { 0 };
		RdpUdpTestRecvState received = { 0 };
		wStream* packet = nullptr;
		SEND_CHECK(fixture);
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
		SEND_CHECK(send_tick(fixture, RDPEUDP_PEER_TIMEOUT_MS - 1));
		packet = Stream_New(nullptr, 64);
		SEND_CHECK(packet);
		/* Build raw layouts because the production encoder rightly refuses
		 * ACK and ACKVEC together. Other cases are length-correct vectors that
		 * describe no packets. */
		Stream_Write_UINT16(packet, (UINT16)(0x5000 | flags[test]));
		if (flags[test] & RDPUDP2_FLAG_ACK)
		{
			Stream_Write_UINT16(packet, 1);
			Stream_Write_UINT32(packet, 0);
			Stream_Write_UINT8(packet, 0);
		}
		if (flags[test] & RDPUDP2_FLAG_DELAYACK)
		{
			Stream_Write_UINT8(packet, 2);
			Stream_Write_UINT16(packet, 20);
		}
		if (flags[test] & RDPUDP2_FLAG_AOA)
			Stream_Write_UINT16(packet, 5000);
		if (flags[test] & RDPUDP2_FLAG_DATA)
			Stream_Write_UINT16(packet, 1);
		Stream_Write_UINT16(packet, 1); /* ACKVEC base */
		const BYTE codedCount = test == 1 || test == 2 ? 1 : 0;
		Stream_Write_UINT8(packet, codedCount);
		for (size_t i = 0; i < codedCount; i++)
			Stream_Write_UINT8(packet, test == 2 ? 1 : 0x80);
		if (flags[test] & RDPUDP2_FLAG_DATA)
		{
			Stream_Write_UINT16(packet, 1);
			Stream_Write_UINT8(packet, 'x');
		}
		RdpUdp2Layout parsed = { 0 };
		SEND_CHECK(
		    !rdpeudp2_parse_layout(Stream_Buffer(packet), Stream_GetPosition(packet), &parsed));
		SEND_CHECK(rdpeudp2_protect(packet, FALSE));
		SEND_CHECK(
		    rdpeudp_test_feed(fixture->sender, Stream_Buffer(packet), Stream_Length(packet)));
		Stream_Release(packet);
		packet = nullptr;
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) &&
		           sent.lastRecvTs == SEND_TEST_TIME && sent.inFlight == 1 &&
		           sent.queuedBytes == 4 && !sent.sendFailed);
		SEND_CHECK(rdpeudp_test_recv_state(fixture->sender, &received) &&
		           received.recvDataBase == 1 && !received.haveSeenAoa && !received.haveRealData &&
		           received.recvStreamLen == 0);
		SEND_CHECK(!send_tick(fixture, RDPEUDP_PEER_TIMEOUT_MS));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.sendFailed);
		send_fixture_free(fixture);
	}
	return TRUE;
}

static BOOL test_send_large_ackvec_is_valid_peer_traffic(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BYTE codedCounts[] = { 17, 127 };
	for (size_t test = 0; test < ARRAYSIZE(codedCounts); test++)
	{
		SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
		RdpUdpTestSendState state = { 0 };
		BYTE vector[130] = { 1, 0 };
		const BYTE coded = codedCounts[test];
		vector[2] = coded;
		for (size_t i = 0; i < coded; i++)
			vector[3 + i] = 0xBF; /* Missing run of 63 packets. */
		UINT16 base = 0;
		BOOL* received = nullptr;
		size_t decoded = 0;
		SEND_CHECK(fixture);
		SEND_CHECK(rdpeudp_parse_ackvec(vector, 3 + coded, &base, &received, &decoded));
		const BOOL valid =
		    base == 1 && decoded == (size_t)coded * 63 && !received[0] && !received[decoded - 1];
		free(received);
		SEND_CHECK(valid);
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
		SEND_CHECK(send_tick(fixture, RDPEUDP_PEER_TIMEOUT_MS - 1));
		RdpUdp2Layout layout = { 0 };
		layout.logWindow = 5;
		layout.flags = RDPUDP2_FLAG_ACKVEC;
		layout.hasAckvec = TRUE;
		layout.ackvec = vector;
		layout.ackvecLen = 3 + coded;
		SEND_CHECK(send_feed_layout(fixture, &layout));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
		           state.lastRecvTs == SEND_TEST_TIME + RDPEUDP_PEER_TIMEOUT_MS - 1 &&
		           state.inFlight == 1 && !state.sendFailed);
		/* This genuine feedback keeps the peer alive but does not acknowledge
		 * missing payload, inflate the local window, or manufacture fast-loss evidence. */
		SEND_CHECK(send_tick(fixture, RDPEUDP_PEER_TIMEOUT_MS));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && !state.sendFailed &&
		           state.inFlight == 1 && state.sendWindow == 31);
		send_fixture_free(fixture);
	}
	return TRUE;
}

static BOOL test_send_fast_retry_budget_and_abort(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BOOL gap[] = { FALSE, TRUE, TRUE, TRUE };
	SendFixture* fixture = send_fixture_new(1, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	UINT16 latest = 1;
	UINT64 elapsed = 0;
	for (size_t retry = 0; retry < RDPEUDP_MAX_RETRIES; retry++)
	{
		elapsed += 30;
		SEND_CHECK(send_tick(fixture, elapsed));
		SEND_CHECK(send_ackvec(fixture, latest, gap, ARRAYSIZE(gap)));
		SEND_CHECK(send_tick(fixture, elapsed + 24));
		const size_t count = fixture->count;
		SEND_CHECK(send_tick(fixture, elapsed + 25) && fixture->count == count + 1);
		latest = fixture->packets[fixture->count - 1].dataSeq;
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
		           state.maxRetries == retry + 1 && state.aliasCount == retry + 2 &&
		           state.inFlight == 1 && !state.sendFailed);
		elapsed += 25;
		/* Fresh later original channels provide fresh receipt evidence. */
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes + 1, 3) == 3);
	}
	elapsed += 30;
	SEND_CHECK(send_tick(fixture, elapsed));
	SEND_CHECK(send_ackvec(fixture, latest, gap, ARRAYSIZE(gap)));
	const size_t count = fixture->count;
	SEND_CHECK(!send_tick(fixture, elapsed + 25) && fixture->count == count);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.sendFailed &&
	           state.maxRetries == RDPEUDP_MAX_RETRIES && state.aliasCount == 11);
	send_fixture_free(fixture);

	fixture = send_fixture_new(4, 5, 5, 1, 1);
	SEND_CHECK(fixture);
	fixture->abortEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	SEND_CHECK(fixture->abortEvent);
	rdpeudp_test_set_abort(fixture->sender, fixture->abortEvent);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	static const UINT64 beforeAbortTicks[] = { 300, 900, 2100, 4500, 9300, 18900 };
	for (size_t i = 0; i < ARRAYSIZE(beforeAbortTicks); i++)
		SEND_CHECK(send_live_tick(fixture, beforeAbortTicks[i]));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.maxRtoMs == 19200);
	const size_t beforeAbort = fixture->count;
	SEND_CHECK(SetEvent(fixture->abortEvent));
	/* Cancellation does not wait for the large backed-off timer. */
	SEND_CHECK(!send_tick(fixture, 18901) && fixture->count == beforeAbort);
	SEND_CHECK(rdpeudp_test_write_data(fixture->sender, bytes, 4) == -1);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_ack_wrap(void)
{
	static const BYTE bytes[] = "ABCD";
	SendFixture* fixture = send_fixture_new(1, 5, 5, UINT16_MAX - 1, UINT16_MAX - 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(fixture->count == 4);
	for (size_t i = 0; i < 4; i++)
	{
		const UINT16 sequence = (UINT16)(UINT16_MAX - 1 + i);
		SEND_CHECK(fixture->packets[i].dataSeq == sequence &&
		           fixture->packets[i].channelSeq == sequence);
	}
	SEND_CHECK(send_ack(fixture, 0));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1);
	SEND_CHECK(send_ack(fixture, UINT16_MAX));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1);
	SEND_CHECK(send_ack(fixture, 1));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 0 &&
	           state.aliasCount == 0 && state.queuedBytes == 0);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_alias_wrap(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BOOL positive[] = { TRUE };
	SendFixture* fixture = send_fixture_new(4, 5, 5, UINT16_MAX, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(send_tick(fixture, 300) && fixture->count == 2);
	SEND_CHECK(fixture->packets[0].dataSeq == UINT16_MAX && fixture->packets[1].dataSeq == 0 &&
	           fixture->packets[0].channelSeq == fixture->packets[1].channelSeq);
	SEND_CHECK(send_ack(fixture, UINT16_MAX));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1 &&
	           state.queuedBytes == 4 && state.aliasCount == 2);
	SEND_CHECK(send_ackvec(fixture, UINT16_MAX, positive, ARRAYSIZE(positive)));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.queuedBytes == 0 &&
	           state.aliasCount == 0);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	SEND_CHECK(fixture->count == 3 && fixture->packets[2].dataSeq == 1 &&
	           fixture->packets[2].channelSeq == 2);
	/* A late ACK for the retired retry cannot ACK the next channel. */
	SEND_CHECK(send_ack(fixture, 0));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 1);
	SEND_CHECK(send_ack(fixture, 1));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.inFlight == 0 &&
	           state.queuedBytes == 0);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_cumulative_ack_after_aoa_retirement(void)
{
	static const BYTE bytes[] = "ABCDEFGH";
	static const BOOL secondReceived[] = { FALSE, TRUE };
	static const BOOL originalReceived[] = { TRUE };
	/* Exercise both valid ways to finish the pending channel: cumulative
	 * receipt of its latest attempt, or explicit receipt of an old alias. */
	for (size_t selectiveOriginal = 0; selectiveOriginal < 2; selectiveOriginal++)
	{
		SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
		RdpUdpTestSendState sent = { 0 };
		RdpUdpTestRecvState received = { 0 };
		RdpUdp2Layout layout = { 0 };
		SEND_CHECK(fixture);
		/* Neither the original DataSeq 1 nor its retry DataSeq 3 reaches
		 * the receiver. DataSeq 2 arrives and buffers channel 2's bytes. */
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes + 4, 4) == 4);
		SEND_CHECK(fixture->count == 2 && fixture->packets[0].dataSeq == 1 &&
		           fixture->packets[1].dataSeq == 2);
		layout.logWindow = fixture->peerLogWindow;
		layout.flags = RDPUDP2_FLAG_DATA;
		layout.hasDataHeader = TRUE;
		layout.dataSeq = 2;
		layout.hasDataBody = TRUE;
		layout.channelSeq = 2;
		layout.dataBody = bytes + 4;
		layout.dataBodyLen = 4;
		SEND_CHECK(send_feed_transport(fixture->receiver, &layout, FALSE));
		SEND_CHECK(send_ackvec(fixture, 1, secondReceived, ARRAYSIZE(secondReceived)));
		SEND_CHECK(send_tick(fixture, 300) && fixture->count == 3 &&
		           fixture->packets[2].dataSeq == 3 && fixture->packets[2].channelSeq == 1);
		/* An independently delivered control packet retires old DataSeqs
		 * before the lost retry arrives. The peer can now ACK base-1 = 2,
		 * although it has never received channel 1's payload. */
		memset(&layout, 0, sizeof(layout));
		layout.logWindow = fixture->peerLogWindow;
		layout.flags = RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_AOA;
		layout.hasAck = TRUE;
		layout.ackBase = 0;
		layout.hasAoa = TRUE;
		layout.aoa = 3;
		SEND_CHECK(send_feed_transport(fixture->receiver, &layout, FALSE));
		SEND_CHECK(rdpeudp_test_recv_state(fixture->receiver, &received) &&
		           received.recvDataBase == 3 && received.expectedChannelSeq == 1 &&
		           received.recvStreamLen == 0);
		SEND_CHECK(send_ack(fixture, (UINT16)(received.recvDataBase - 1)));
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 1 &&
		           sent.aliasCount == 2 && sent.queuedBytes == 4);
		SEND_CHECK(send_tick(fixture, 300) && fixture->count == 3);
		if (selectiveOriginal)
		{
			SEND_CHECK(send_ackvec(fixture, 1, originalReceived, ARRAYSIZE(originalReceived)));
		}
		else
		{
			/* Delivery of the latest DataSeq 3 closes the channel gap.
			 * Its cumulative ACK now provides valid receipt evidence. */
			memset(&layout, 0, sizeof(layout));
			layout.logWindow = fixture->peerLogWindow;
			layout.flags = RDPUDP2_FLAG_DATA;
			layout.hasDataHeader = TRUE;
			layout.dataSeq = 3;
			layout.hasDataBody = TRUE;
			layout.channelSeq = 1;
			layout.dataBody = bytes;
			layout.dataBodyLen = 4;
			SEND_CHECK(send_feed_transport(fixture->receiver, &layout, FALSE));
			SEND_CHECK(send_received(fixture, bytes, sizeof(bytes) - 1));
			SEND_CHECK(send_ack(fixture, 3));
		}
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 0 &&
		           sent.aliasCount == 0 && sent.queuedBytes == 0);
		SEND_CHECK(send_tick(fixture, 1000) && fixture->count == 3);
		SEND_CHECK(!fixture->callbackFailed);
		send_fixture_free(fixture);
	}
	return TRUE;
}

static BOOL test_send_ack_aoa_epoch(void)
{
	static const BYTE bytes[] = "ABCD";
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState sent = { 0 };
	RdpUdpTestRecvState received = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	/* ACK and AOA use opposite sequence spaces. This pure ACK retires our
 * outgoing chunk and advances the peer receive epoch without any DATA. */
	SEND_CHECK(send_ack_aoa(fixture, 1, 101));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 0 &&
	           sent.queuedBytes == 0 && sent.aliasCount == 0);
	SEND_CHECK(rdpeudp_test_recv_state(fixture->sender, &received) &&
	           received.recvDataBase == 101 && received.haveSeenAoa &&
	           !received.haveRecvData && !received.haveRealData &&
	           received.expectedChannelSeq == 1 && received.recvStreamLen == 0);
	SEND_CHECK(send_tick(fixture, 300) && fixture->count == 1);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_ack_aoa_monotonic(void)
{
	static const UINT16 advertisements[] = { 10, 10, 9, 12, 11, 12, 15 };
	static const UINT16 expected[] = { 10, 10, 10, 12, 12, 12, 15 };
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestRecvState received = { 0 };
	SEND_CHECK(fixture);
	for (size_t i = 0; i < ARRAYSIZE(advertisements); i++)
	{
		SEND_CHECK(send_ack_aoa(fixture, 0, advertisements[i]));
		SEND_CHECK(rdpeudp_test_recv_state(fixture->sender, &received) &&
		           received.recvDataBase == expected[i] && received.haveSeenAoa &&
		           !received.haveRecvData && !received.haveRealData && received.recvStreamLen == 0);
	}
	SEND_CHECK(fixture->count == 0 && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_ack_aoa_wrap(void)
{
	/* Reach the wrap with legal forward advances smaller than half the
 * sequence space; no receive-state mutation or DATA packet is required. */
	static const UINT16 advertisements[] = { 32767, 65534, 0, 65535, 0, 2, 1 };
	static const UINT16 expected[] = { 32767, 65534, 0, 0, 0, 2, 2 };
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestRecvState received = { 0 };
	SEND_CHECK(fixture);
	for (size_t i = 0; i < ARRAYSIZE(advertisements); i++)
	{
		SEND_CHECK(send_ack_aoa(fixture, 0, advertisements[i]));
		SEND_CHECK(rdpeudp_test_recv_state(fixture->sender, &received) &&
		           received.recvDataBase == expected[i] && received.haveSeenAoa &&
		           !received.haveRecvData && !received.haveRealData && received.recvStreamLen == 0);
	}
	SEND_CHECK(fixture->count == 0 && !fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_dummy_ack(void)
{
	static const BYTE bytes[] = "ABCD";
	static const BYTE dummyBody[] = "IGNORE";
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState sent = { 0 };
	RdpUdpTestRecvState received = { 0 };
	RdpUdp2Layout layout = { 0 };
	SEND_CHECK(fixture);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
	layout.logWindow = fixture->peerLogWindow;
	layout.flags = RDPUDP2_FLAG_ACK | RDPUDP2_FLAG_DATA;
	layout.hasAck = TRUE;
	layout.ackBase = 1;
	layout.hasDataHeader = TRUE;
	layout.dataSeq = 1;
	layout.hasDataBody = TRUE;
	layout.channelSeq = 1;
	layout.dataBody = dummyBody;
	layout.dataBodyLen = sizeof(dummyBody) - 1;
	/* The protected dummy bit suppresses only channel delivery. Its ACK
 * remains useful, and its DataSeq participates in receive accounting. */
	SEND_CHECK(send_feed_layout_full(fixture, &layout, TRUE));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 0 &&
	           sent.queuedBytes == 0 && sent.aliasCount == 0);
	SEND_CHECK(rdpeudp_test_recv_state(fixture->sender, &received) &&
	           received.recvDataBase == 2 && received.haveRecvData && !received.haveRealData &&
	           received.expectedChannelSeq == 1 && received.recvStreamLen == 0);
	SEND_CHECK(send_tick(fixture, 5000) && fixture->count == 1);
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_terminal_failure(void)
{
	static const BYTE bytes[] = "ABCDEFGHIJKL";
	SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	fixture->failPacket = 2;
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 12) == -1);
	SEND_CHECK(fixture->count == 2 && send_received(fixture, bytes, 4));
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.sendFailed);
	SEND_CHECK(!rdpeudp_test_check_health(fixture->sender));
	/* The first four bytes reached the receiver. A new application write
 * must fail without emitting those bytes under fresh ChannelSeq values. */
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 12) == -1);
	SEND_CHECK(!send_tick(fixture, 10000) && fixture->count == 2);
	SEND_CHECK(send_received(fixture, bytes, 4));
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

static BOOL test_send_abort(void)
{
	static const BYTE bytes[] = "ABCDEFGHIJ";
	SendFixture* fixture = send_fixture_new(1, 2, 2, 1, 1);
	RdpUdpTestSendState state = { 0 };
	SEND_CHECK(fixture);
	fixture->deliver = TRUE;
	fixture->abortEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	SEND_CHECK(fixture->abortEvent);
	rdpeudp_test_set_abort(fixture->sender, fixture->abortEvent);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 10) == 10 && fixture->count == 3);
	SEND_CHECK(SetEvent(fixture->abortEvent));
	SEND_CHECK(!send_tick(fixture, 300) && fixture->count == 3);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.sendFailed);
	SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 10) == -1 && fixture->count == 3);
	SEND_CHECK(send_received(fixture, bytes, 3));
	SEND_CHECK(!fixture->callbackFailed);
	send_fixture_free(fixture);
	return TRUE;
}

int TestRdpeUdpSend(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	if (!test_send_pipeline() || !test_send_window_and_selective_ack() ||
	    !test_send_window_shrink() || !test_send_window_shrink_with_loss() ||
	    !test_send_window_storage_cap() || !test_send_peer_window_tracking() ||
	    !test_send_atomic_queue_and_owned_payload() || !test_send_late_original_ack() ||
	    !test_send_lost_data() || !test_send_idle_health_pump() || !test_send_repeated_loss() ||
	    !test_send_adaptive_rtt_and_karn() || !test_send_peer_silence_and_idle() ||
	    !test_send_malformed_ack_cannot_refresh_peer() ||
	    !test_send_large_ackvec_is_valid_peer_traffic() ||
	    !test_send_fast_retry_budget_and_abort() || !test_send_fast_loss_evidence() ||
	    !test_send_fast_loss_reordered_receipt() || !test_send_fast_loss_delivers_ordered_bytes() ||
	    !test_send_fast_loss_attempt_reset() ||
	    !test_send_fast_loss_wrap_shrink_and_multiple_gaps() || !test_send_ack_wrap() ||
	    !test_send_alias_wrap() || !test_send_cumulative_ack_after_aoa_retirement() ||
	    !test_send_ack_aoa_epoch() || !test_send_ack_aoa_monotonic() || !test_send_ack_aoa_wrap() ||
	    !test_send_dummy_ack() || !test_send_terminal_failure() || !test_send_abort())
		return -1;
	(void)printf("TestRdpeUdpSend passed\n");
	return 0;
}
