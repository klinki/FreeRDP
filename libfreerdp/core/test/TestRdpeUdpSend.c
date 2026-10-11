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
	SEND_CHECK(send_tick(fixture, 199) && fixture->count == 31);
	/* Existing transmissions may replace their oldest active attempt
	 * without expanding the grandfathered span. New channels stay blocked. */
	fixture->deliver = TRUE;
	SEND_CHECK(send_tick(fixture, 200) && fixture->count == 62);
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
			           packet->time == SEND_TEST_TIME + 200);
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
	SEND_CHECK(send_tick(fixture, 200) && fixture->count == 65);
	SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &sent) && sent.inFlight == 3 &&
	           sent.nextChannelSeq == 35);
	for (size_t i = 0; i < 8 && sent.queuedBytes; i++)
	{
		const size_t count = fixture->count;
		SEND_CHECK(rdpeudp_test_recv_state(fixture->receiver, &received));
		SEND_CHECK(send_ack(fixture, (UINT16)(received.recvDataBase - 1)));
		SEND_CHECK(send_tick(fixture, 200));
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
	SEND_CHECK(send_tick(fixture, 199) && fixture->count == 1);
	SEND_CHECK(send_tick(fixture, 200) && fixture->count == 2);
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
	SEND_CHECK(send_tick(fixture, 200) && fixture->count == 2);
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
	static const UINT64 deadlines[] = { 200, 600, 1200, 2000, 3000 };
	for (size_t ackLoss = 0; ackLoss < 2; ackLoss++)
	{
		SendFixture* fixture = send_fixture_new(4, 5, 5, 1, 1);
		RdpUdpTestSendState state = { 0 };
		SEND_CHECK(fixture);
		fixture->deliver = (ackLoss != 0);
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == 4);
		for (size_t i = 0; i < ARRAYSIZE(deadlines); i++)
		{
			SEND_CHECK(send_tick(fixture, deadlines[i] - 1) && fixture->count == i + 1);
			SEND_CHECK(send_tick(fixture, deadlines[i]) && fixture->count == i + 2);
			SEND_CHECK(fixture->packets[i + 1].dataSeq == i + 2 &&
			           fixture->packets[i + 1].channelSeq == 1 &&
			           fixture->packets[i + 1].time == SEND_TEST_TIME + deadlines[i]);
			SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) &&
			           state.maxRetries == i + 1 && state.inFlight == 1 &&
			           state.aliasCount == i + 2);
		}
		SEND_CHECK(send_tick(fixture, 3999) && fixture->count == 6);
		/* The retry limit is checked before a seventh DATA transmission. */
		SEND_CHECK(!send_tick(fixture, 4000) && fixture->count == 6);
		SEND_CHECK(rdpeudp_test_send_state(fixture->sender, &state) && state.sendFailed);
		SEND_CHECK(!rdpeudp_test_check_health(fixture->sender));
		SEND_CHECK(rdpeudp_test_send_data(fixture->sender, bytes, 4) == -1);
		SEND_CHECK(!send_tick(fixture, 10000) && fixture->count == 6);
		SEND_CHECK(send_received(fixture, bytes, ackLoss ? 4 : 0));
		SEND_CHECK(!fixture->callbackFailed);
		send_fixture_free(fixture);
	}
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
	SEND_CHECK(send_tick(fixture, 200) && fixture->count == 2);
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
		SEND_CHECK(send_tick(fixture, 200) && fixture->count == 3 &&
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
		SEND_CHECK(send_tick(fixture, 200) && fixture->count == 3);
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
	SEND_CHECK(send_tick(fixture, 200) && fixture->count == 1);
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
	SEND_CHECK(!send_tick(fixture, 200) && fixture->count == 3);
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
	    !test_send_ack_wrap() || !test_send_alias_wrap() ||
	    !test_send_cumulative_ack_after_aoa_retirement() || !test_send_ack_aoa_epoch() ||
	    !test_send_ack_aoa_monotonic() || !test_send_ack_aoa_wrap() || !test_send_dummy_ack() ||
	    !test_send_terminal_failure() || !test_send_abort())
		return -1;
	(void)printf("TestRdpeUdpSend passed\n");
	return 0;
}
