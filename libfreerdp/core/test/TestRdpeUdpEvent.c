/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Socket and manual-event integration for the queued RDP-UDP2 sender
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
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <freerdp/freerdp.h>
#include <freerdp/transport_io.h>

#include "../multitransport.h"
#include "../rdpeudp.h"
#include "../udp.h"

#define EVENT_TEST_TIME 1000

typedef struct
{
	freerdp* instance;
	rdpUdpTransport* udp;
	BOOL ownedByContext;
	HANDLE socketEvent;
	HANDLE stateEvent;
	size_t dataPackets;
} EventFixture;

/* Lose every DATA datagram, including retransmissions, without modifying the
 * production queue. This deliberately leaves the attached socket unreadable. */
static SSIZE_T event_drop_send(void* context, const BYTE* data, size_t len)
{
	EventFixture* fixture = context;
	BYTE packet[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
	BOOL dummy = FALSE;
	size_t offset = 0;
	RdpUdp2Layout layout = { 0 };
	if (!fixture || len > sizeof(packet))
		return -1;
	memcpy(packet, data, len);
	if (!rdpeudp2_unprotect(packet, len, &dummy, &offset) ||
	    !rdpeudp2_parse_layout(packet + offset, len - offset, &layout))
		return -1;
	if (!dummy && layout.hasDataBody)
		fixture->dataPackets++;
	return (SSIZE_T)len;
}

static void event_fixture_free(EventFixture* fixture)
{
	if (!fixture)
		return;
	if (!fixture->ownedByContext)
		rdpeudp_test_free(fixture->udp);
	if (fixture->instance)
	{
		freerdp_context_free(fixture->instance);
		freerdp_free(fixture->instance);
	}
	free(fixture);
}

static EventFixture* event_fixture_new(BOOL attachSocket)
{
	EventFixture* fixture = calloc(1, sizeof(*fixture));
	if (!fixture)
		return nullptr;
	fixture->instance = freerdp_new();
	if (!fixture->instance || !freerdp_context_new(fixture->instance))
		goto fail;
	fixture->udp = rdpeudp_test_new();
	if (!fixture->udp)
		goto fail;
	rdpeudp_test_set_context(fixture->udp, fixture->instance->context);
	if (!rdpeudp_test_configure_send(fixture->udp, 4, 2, 2, 1, 1))
		goto fail;
	rdpeudp_test_set_time(fixture->udp, EVENT_TEST_TIME);
	rdpeudp_test_set_send(fixture->udp, event_drop_send, fixture);
	if (attachSocket)
	{
		const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (fd < 0)
			goto fail;
		struct sockaddr_in local = { 0 };
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(fd, (struct sockaddr*)&local, sizeof(local)) != 0)
		{
			close(fd);
			goto fail;
		}
		/* The production event association takes ownership of fd. */
		if (!rdpeudp_test_attach_socket(fixture->udp, fd))
			goto fail;
	}
	rdpeudp_test_get_events(fixture->udp, &fixture->socketEvent, &fixture->stateEvent);
	if (!fixture->stateEvent || (attachSocket && !fixture->socketEvent) ||
	    !multitransport_test_attach_udp(fixture->instance->context, fixture->udp))
		goto fail;
	fixture->ownedByContext = TRUE;
	return fixture;
fail:
	event_fixture_free(fixture);
	return nullptr;
}

#define EVENT_CHECK(condition)                                                                  \
	do                                                                                          \
	{                                                                                           \
		if (!(condition))                                                                       \
		{                                                                                       \
			(void)fprintf(stderr, "%s FAILED line %d: %s\n", __func__, __LINE__, #condition); \
			event_fixture_free(fixture);                                                          \
			return FALSE;                                                                        \
		}                                                                                       \
	} while (0)

static size_t event_count_handle(const HANDLE* handles, size_t count, HANDLE expected)
{
	size_t matches = 0;
	for (size_t i = 0; i < count; i++)
		matches += handles[i] == expected ? 1 : 0;
	return matches;
}

static BOOL test_queued_write_wakes_mainloop(void)
{
	EventFixture* fixture = event_fixture_new(TRUE);
	if (!fixture)
		return FALSE;
	const BYTE bytes[4] = { 1, 2, 3, 4 };
	EVENT_CHECK(fixture->socketEvent != fixture->stateEvent);
	EVENT_CHECK(WaitForSingleObject(fixture->socketEvent, 0) == WAIT_TIMEOUT);
	EVENT_CHECK(WaitForSingleObject(fixture->stateEvent, 0) == WAIT_TIMEOUT);
	EVENT_CHECK(rdpeudp_test_write_data(fixture->udp, bytes, sizeof(bytes)) == sizeof(bytes));
	EVENT_CHECK(fixture->dataPackets == 1);
	const HANDLE events[] = { fixture->socketEvent, fixture->stateEvent };
	EVENT_CHECK(WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 0) == WAIT_OBJECT_0 + 1);
	EVENT_CHECK(WaitForSingleObject(fixture->socketEvent, 0) == WAIT_TIMEOUT);

	HANDLE handles[32] = { 0 };
	const DWORD count = freerdp_get_event_handles(fixture->instance->context, handles,
	                                             ARRAYSIZE(handles));
	EVENT_CHECK(count > 0 && count <= ARRAYSIZE(handles));
	EVENT_CHECK(event_count_handle(handles, count, fixture->socketEvent) == 1);
	EVENT_CHECK(event_count_handle(handles, count, fixture->stateEvent) == 1);
	/* A small array must fail before storing the second UDP handle. */
	const HANDLE sentinel = INVALID_HANDLE_VALUE;
	handles[2] = sentinel;
	EVENT_CHECK(freerdp_get_event_handles(fixture->instance->context, handles, 2) == 0);
	EVENT_CHECK(handles[2] == sentinel);
	event_fixture_free(fixture);
	return TRUE;
}

static BOOL test_enqueue_after_snapshot_preserves_wake(void)
{
	EventFixture* fixture = event_fixture_new(TRUE);
	if (!fixture)
		return FALSE;
	const BYTE bytes[4] = { 1, 2, 3, 4 };
	UINT64 firstGeneration = 0;
	UINT64 secondGeneration = 0;
	EVENT_CHECK(rdpeudp_test_write_data(fixture->udp, bytes, sizeof(bytes)) == sizeof(bytes));
	EVENT_CHECK(rdpeudp_test_send_snapshot(fixture->udp, &firstGeneration));
	/* Deterministically interleave a writer after the mainloop snapshot and
	 * before its consume/reset, without relying on a thread timing race. */
	EVENT_CHECK(rdpeudp_test_write_data(fixture->udp, bytes, sizeof(bytes)) == sizeof(bytes));
	EVENT_CHECK(rdpeudp_test_send_snapshot(fixture->udp, &secondGeneration));
	EVENT_CHECK(secondGeneration != firstGeneration);
	rdpeudp_test_consume_send_wake(fixture->udp, firstGeneration);
	rdpeudp_test_update_event(fixture->udp);
	EVENT_CHECK(WaitForSingleObject(fixture->stateEvent, 0) == WAIT_OBJECT_0);
	EVENT_CHECK(WaitForSingleObject(fixture->socketEvent, 0) == WAIT_TIMEOUT);
	/* Once the current generation is consumed, pending bytes rely on the
	 * active maintenance timer rather than continually signaling readiness. */
	rdpeudp_test_consume_send_wake(fixture->udp, secondGeneration);
	rdpeudp_test_update_event(fixture->udp);
	EVENT_CHECK(WaitForSingleObject(fixture->stateEvent, 0) == WAIT_TIMEOUT);
	EVENT_CHECK(fixture->dataPackets == 2);
	rdpeudp_test_set_time(fixture->udp, EVENT_TEST_TIME + RDPEUDP_ACK_TIMEOUT_MS);
	EVENT_CHECK(rdpeudp_test_check_health(fixture->udp));
	EVENT_CHECK(fixture->dataPackets == 4);
	RdpUdpTestSendState state = { 0 };
	EVENT_CHECK(rdpeudp_test_send_state(fixture->udp, &state));
	EVENT_CHECK(state.inFlight == 2 && state.maxRetries == 1 && !state.sendFailed);
	event_fixture_free(fixture);
	return TRUE;
}

static BOOL test_buffered_receive_preserves_wake(void)
{
	EventFixture* fixture = event_fixture_new(TRUE);
	if (!fixture)
		return FALSE;
	const BYTE body[4] = { 5, 6, 7, 8 };
	RdpUdp2Layout layout = { 0 };
	layout.flags = RDPUDP2_FLAG_DATA;
	layout.logWindow = 2;
	layout.hasDataHeader = layout.hasDataBody = TRUE;
	layout.dataSeq = layout.channelSeq = 1;
	layout.dataBody = body;
	layout.dataBodyLen = sizeof(body);
	wStream* packet = rdpeudp2_encode_layout(&layout);
	EVENT_CHECK(packet);
	BOOL fed = Stream_SetPosition(packet, Stream_Length(packet)) && rdpeudp2_protect(packet, FALSE);
	if (fed)
		fed = rdpeudp_test_feed(fixture->udp, Stream_Buffer(packet), Stream_Length(packet));
	Stream_Release(packet);
	EVENT_CHECK(fed);
	RdpUdpTestRecvState received = { 0 };
	EVENT_CHECK(rdpeudp_test_recv_state(fixture->udp, &received));
	EVENT_CHECK(received.recvStreamLen == sizeof(body));
	UINT64 generation = 0;
	EVENT_CHECK(!rdpeudp_test_send_snapshot(fixture->udp, &generation));
	rdpeudp_test_consume_send_wake(fixture->udp, generation);
	rdpeudp_test_update_event(fixture->udp);
	EVENT_CHECK(WaitForSingleObject(fixture->stateEvent, 0) == WAIT_OBJECT_0);
	EVENT_CHECK(WaitForSingleObject(fixture->socketEvent, 0) == WAIT_TIMEOUT);
	event_fixture_free(fixture);
	return TRUE;
}

static BOOL test_socketless_event_is_not_duplicated(void)
{
	EventFixture* fixture = event_fixture_new(FALSE);
	if (!fixture)
		return FALSE;
	HANDLE handles[32] = { 0 };
	const DWORD count = freerdp_get_event_handles(fixture->instance->context, handles,
	                                             ARRAYSIZE(handles));
	EVENT_CHECK(count > 0 && count <= ARRAYSIZE(handles));
	EVENT_CHECK(event_count_handle(handles, count, fixture->stateEvent) == 1);
	event_fixture_free(fixture);
	return TRUE;
}

int TestRdpeUdpEvent(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	if (!test_queued_write_wakes_mainloop() || !test_enqueue_after_snapshot_preserves_wake() ||
	    !test_buffered_receive_preserves_wake() || !test_socketless_event_is_not_duplicated())
		return -1;
	(void)printf("TestRdpeUdpEvent passed\n");
	return 0;
}
