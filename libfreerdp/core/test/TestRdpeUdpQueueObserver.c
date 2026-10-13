/**
 * Deterministic regressions for the optional native benchmark observer.
 * Licensed under the Apache License, Version 2.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winpr/crt.h>
#include <winpr/environment.h>
#include "../rdpeudp.h"
#include "../udp.h"

#define QUEUE_STATS 40
#define QUEUE_BINS 16384
#define QUEUE_NS(ms) ((UINT64)(ms) * 1000000ULL)

typedef struct
{
	rdpUdpTransport* udp;
	BOOL autoAck;
	BOOL failed;
	BOOL tickAfterAck;
	UINT64 clockMs;
	BYTE logWindow;
	size_t packets;
	size_t bytes;
	BYTE payload[131072];
} QueueFixture;

static BOOL queue_ack(QueueFixture* fixture, UINT16 sequence)
{
	RdpUdp2Layout layout = { 0 };
	layout.logWindow = fixture->logWindow;
	layout.flags = RDPUDP2_FLAG_ACK;
	layout.hasAck = TRUE;
	layout.ackBase = sequence;
	wStream* packet = rdpeudp2_encode_layout(&layout);
	if (!packet)
		return FALSE;
	BOOL result = Stream_SetPosition(packet, Stream_Length(packet));
	if (result)
		result = rdpeudp2_protect(packet, FALSE);
	if (result)
		result = rdpeudp_test_feed(fixture->udp, Stream_Buffer(packet), Stream_Length(packet));
	Stream_Release(packet);
	return result;
}

static SSIZE_T queue_capture(void* context, const BYTE* data, size_t len)
{
	QueueFixture* fixture = context;
	BYTE packet[FREERDP_UDP_MAX_DATAGRAM] = { 0 };
	RdpUdp2Layout layout = { 0 };
	BOOL dummy = FALSE;
	size_t offset = 0;
	if (len > sizeof(packet))
		return -1;
	memcpy(packet, data, len);
	if (!rdpeudp2_unprotect(packet, len, &dummy, &offset) ||
	    !rdpeudp2_parse_layout(packet + offset, len - offset, &layout))
		return -1;
	if (!dummy && layout.hasDataBody)
	{
		fixture->packets++;
		if (layout.dataBodyLen > sizeof(fixture->payload) - fixture->bytes)
			return -1;
		memcpy(fixture->payload + fixture->bytes, layout.dataBody, layout.dataBodyLen);
		fixture->bytes += layout.dataBodyLen;
		if (fixture->autoAck && !queue_ack(fixture, layout.dataSeq))
		{
			fixture->failed = TRUE;
			return -1;
		}
		if (fixture->autoAck && fixture->tickAfterAck)
			rdpeudp_test_set_time(fixture->udp, ++fixture->clockMs);
	}
	return (SSIZE_T)len;
}

static QueueFixture* queue_fixture_new(UINT16 payload, BYTE window)
{
	QueueFixture* fixture = calloc(1, sizeof(*fixture));
	if (!fixture)
		return nullptr;
	fixture->udp = rdpeudp_test_new();
	fixture->logWindow = window;
	fixture->clockMs = 1000;
	if (!fixture->udp || !rdpeudp_test_configure_send(fixture->udp, payload, window, window, 1, 1))
	{
		rdpeudp_test_free(fixture->udp);
		free(fixture);
		return nullptr;
	}
	rdpeudp_test_set_time(fixture->udp, 1000);
	rdpeudp_test_set_send(fixture->udp, queue_capture, fixture);
	return fixture;
}

static void queue_fixture_free(QueueFixture* fixture)
{
	if (fixture)
		rdpeudp_test_free(fixture->udp);
	free(fixture);
}

#define QUEUE_CHECK(expression) \
	do { if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); goto fail; } } while (0)

static BOOL test_queue_segment_boundaries(void)
{
	QueueFixture* fixture = queue_fixture_new(4, 2);
	UINT64 stats[QUEUE_STATS] = { 0 };
	UINT64* histogram = calloc(QUEUE_BINS, sizeof(*histogram));
	BYTE first[] = "ABCDEFGHIJ";
	QUEUE_CHECK(fixture && histogram);
	QUEUE_CHECK(rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(2000)));
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, first, 10) == 10);
	/* Acceptance owns bytes independently of the caller's buffer. */
	memset(first, 'X', 10);
	rdpeudp_test_set_time(fixture->udp, 1001);
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, (const BYTE*)"KL", 2) == 2);
	rdpeudp_test_set_time(fixture->udp, 1010);
	QUEUE_CHECK(rdpeudp_test_service_send(fixture->udp));
	QUEUE_CHECK(fixture->bytes == 12 && memcmp(fixture->payload, "ABCDEFGHIJKL", 12) == 0);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(stats[0] && stats[4] == 12 && stats[5] == QUEUE_NS(9) && stats[6] == QUEUE_NS(10));
	QUEUE_CHECK(stats[7] == QUEUE_NS(118) && stats[23] == 12 && stats[24] == 12);
	QUEUE_CHECK(stats[19] == 12 && stats[20] == 12 && stats[21] == 12 && stats[22] == 3);
	QUEUE_CHECK(stats[25] == 0 && stats[26] == 0);
	QUEUE_CHECK(rdpeudp_test_queue_hist(fixture->udp, 0, histogram, QUEUE_BINS));
	QUEUE_CHECK(histogram[72] == 2 && histogram[80] == 10);
	free(histogram);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	free(histogram);
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_compaction_and_cohort(void)
{
	QueueFixture* fixture = queue_fixture_new(4, 2);
	UINT64 stats[QUEUE_STATS] = { 0 };
	QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, (const BYTE*)"ABCDEFGHIJKLMNOPQRST", 20) == 20);
	QUEUE_CHECK(rdpeudp_test_service_send(fixture->udp) && fixture->bytes == 12);
	/* Reset while a warmup segment is pending. It must remain tracked but
	 * must never enter the measured acceptance cohort. */
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1010), QUEUE_NS(1025)));
	rdpeudp_test_set_time(fixture->udp, 1010);
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, (const BYTE*)"UVWXYZ", 6) == 6);
	rdpeudp_test_set_time(fixture->udp, 1020);
	QUEUE_CHECK(queue_ack(fixture, 3) && rdpeudp_test_service_send(fixture->udp));
	QUEUE_CHECK(fixture->bytes == 24);
	rdpeudp_test_set_time(fixture->udp, 1030);
	QUEUE_CHECK(queue_ack(fixture, 6) && rdpeudp_test_service_send(fixture->udp));
	QUEUE_CHECK(fixture->bytes == 26 && memcmp(fixture->payload, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26) == 0);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	/* Final two accepted-cohort bytes first transmit in drain and count. */
	QUEUE_CHECK(stats[0] && stats[4] == 6 && stats[23] == 6 && stats[24] == 6);
	QUEUE_CHECK(stats[5] == QUEUE_NS(10) && stats[6] == QUEUE_NS(20) && stats[7] == QUEUE_NS(80));
	QUEUE_CHECK(stats[25] == 0 && stats[26] == 0);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_rejection_and_caps(void)
{
	QueueFixture* fixture = queue_fixture_new(1150, 5);
	UINT64 stats[QUEUE_STATS] = { 0 };
	BYTE* bytes = calloc(32768, 1);
	QUEUE_CHECK(fixture && bytes);
	/* Default cap and packet window are independently observable. */
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(stats[1] == 32768 && stats[35] == 31 && stats[36] == 0);
	QUEUE_CHECK(rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(2000)));
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, bytes, 32768) == 32768);
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, bytes, 1) == -2);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(stats[0] && stats[23] == 32768 && stats[25] == 1 && stats[26] == 32768);
	QUEUE_CHECK(!rdpeudp_test_queue_enable(fixture->udp, 49152));
	free(bytes);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	free(bytes);
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_retries_and_callback(void)
{
	QueueFixture* fixture = queue_fixture_new(4, 2);
	UINT64 stats[QUEUE_STATS] = { 0 };
	QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(2000)));
	QUEUE_CHECK(rdpeudp_test_send_data(fixture->udp, (const BYTE*)"ABCD", 4) == 4);
	rdpeudp_test_set_time(fixture->udp, 1000 + RDPEUDP_ACK_TIMEOUT_MS);
	QUEUE_CHECK(rdpeudp_test_service_send(fixture->udp) && fixture->packets == 2);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS) && stats[4] == 4);
	QUEUE_CHECK(queue_ack(fixture, 2));
	fixture->autoAck = TRUE;
	QUEUE_CHECK(rdpeudp_test_send_data(fixture->udp, (const BYTE*)"EFGHIJKL", 8) == 8);
	QUEUE_CHECK(!fixture->failed && fixture->packets == 4);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(stats[0] && stats[4] == 12 && stats[23] == 12 && stats[24] == 12);
	QUEUE_CHECK(stats[25] == 0 && stats[26] == 0);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_all_cap_boundaries(void)
{
	QueueFixture* fixture = nullptr;
	UINT64 stats[QUEUE_STATS] = { 0 };
	const size_t capacities[] = { 32768, 49152, 65536, 131072 };
	BYTE* bytes = calloc(32768, 1);
	QUEUE_CHECK(bytes);
	for (size_t i = 0; i < ARRAYSIZE(capacities); i++)
	{
		fixture = queue_fixture_new(1150, 5);
		QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, capacities[i]));
		QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(2000)));
		size_t remaining = capacities[i];
		while (remaining)
		{
			const size_t chunk = remaining < 32768 ? remaining : 32768;
			QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, bytes, chunk) == (SSIZE_T)chunk);
			remaining -= chunk;
		}
		QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, bytes, 1) == -2);
		QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
		QUEUE_CHECK(stats[0] && stats[1] == capacities[i] && stats[19] == capacities[i]);
		QUEUE_CHECK(stats[20] == capacities[i] && stats[23] == capacities[i] && stats[26] == capacities[i]);
		QUEUE_CHECK(stats[35] == 31);
		queue_fixture_free(fixture);
		fixture = nullptr;
	}
	free(bytes);
	return TRUE;
fail:
	free(bytes);
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_admission_wait(void)
{
	QueueFixture* fixture = queue_fixture_new(1150, 5);
	UINT64 stats[QUEUE_STATS] = { 0 };
	BYTE* bytes = calloc(32768, 1);
	QUEUE_CHECK(fixture && bytes && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(2000)));
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, bytes, 32768) == 32768);
	fixture->autoAck = fixture->tickAfterAck = TRUE;
	/* Capacity refusal pumps the old bytes. Each genuine ACK callback moves
	 * the fixture clock one ms, independently of queue residence timestamps. */
	QUEUE_CHECK(rdpeudp_test_write_data(fixture->udp, (const BYTE*)"A", 1) == 1);
	fixture->tickAfterAck = FALSE;
	QUEUE_CHECK(rdpeudp_test_write_data(fixture->udp, (const BYTE*)"B", 1) == 1);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(stats[0] && stats[8] == 2 && stats[12] == 1);
	QUEUE_CHECK(stats[9] == 0 && stats[10] == QUEUE_NS(29) && stats[11] == QUEUE_NS(29));
	QUEUE_CHECK(stats[13] == QUEUE_NS(29) && stats[14] == QUEUE_NS(29) && stats[15] == QUEUE_NS(29));
	QUEUE_CHECK(stats[4] == 32770 && stats[23] == 32770 && stats[24] == 32770);
	QUEUE_CHECK(!fixture->failed && stats[25] == 0);
	free(bytes);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	free(bytes);
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_observer_limits(void)
{
	QueueFixture* fixture = queue_fixture_new(4, 2);
	UINT64 stats[QUEUE_STATS] = { 0 };
	QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(10000)));
	for (size_t i = 0; i < 1025; i++)
		QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, (const BYTE*)"A", 1) == 1);
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(!stats[0] && stats[28] == 1 && stats[23] == 1025);
	queue_fixture_free(fixture);
	fixture = queue_fixture_new(4, 2);
	QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(10000)));
	QUEUE_CHECK(rdpeudp_test_send_data(fixture->udp, (const BYTE*)"ABCDEFGHIJKLMNOP", 16) == 16);
	rdpeudp_test_set_time(fixture->udp, 3050);
	QUEUE_CHECK(queue_ack(fixture, 3) && rdpeudp_test_service_send(fixture->udp));
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(!stats[0] && stats[4] == 16 && stats[6] == QUEUE_NS(2050) && stats[16] == 4);
	queue_fixture_free(fixture);
	fixture = queue_fixture_new(4, 2);
	QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), UINT64_MAX));
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, (const BYTE*)"ABCD", 4) == 4);
	/* Product overflow is flagged independently of histogram overflow. */
	rdpeudp_test_set_time(fixture->udp, UINT64_MAX / 1000000ULL);
	/* The synthetic observer clock does not model a dead transport peer. */
	QUEUE_CHECK(queue_ack(fixture, 0));
	QUEUE_CHECK(rdpeudp_test_service_send(fixture->udp));
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(!stats[0] && stats[29] && stats[16] == 4 && stats[7] == UINT64_MAX);
	queue_fixture_free(fixture);
	fixture = queue_fixture_new(4, 2);
	QUEUE_CHECK(fixture && rdpeudp_test_queue_enable(fixture->udp, 32768));
	QUEUE_CHECK(rdpeudp_test_queue_measure(fixture->udp, QUEUE_NS(1000), QUEUE_NS(2000)));
	QUEUE_CHECK(rdpeudp_test_queue_enqueue(fixture->udp, (const BYTE*)"ABCD", 4) == 4);
	rdpeudp_test_set_time(fixture->udp, 999);
	QUEUE_CHECK(queue_ack(fixture, 0));
	QUEUE_CHECK(rdpeudp_test_service_send(fixture->udp));
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(!stats[0] && stats[27]);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	queue_fixture_free(fixture);
	return FALSE;
}

static BOOL test_queue_environment(void)
{
	QueueFixture* fixture = queue_fixture_new(1150, 5);
	UINT64 stats[QUEUE_STATS] = { 0 };
	const char* oldCap = getenv("RDP_BENCH_SEND_QUEUE_BYTES");
	const char* oldObserver = getenv("RDP_BENCH_QUEUE_OBSERVER");
	char* savedCap = oldCap ? _strdup(oldCap) : nullptr;
	char* savedObserver = oldObserver ? _strdup(oldObserver) : nullptr;
	const char* valid[] = { "32768", "49152", "65536", "131072" };
	const UINT64 expected[] = { 32768, 49152, 65536, 131072 };
	const char* invalid[] = { "32768 ", "+32768", "65535", "131073", "32768.0", "" };
	QUEUE_CHECK(fixture);
	QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_QUEUE_OBSERVER", "1"));
	for (size_t i = 0; i < ARRAYSIZE(valid); i++)
	{
		QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_SEND_QUEUE_BYTES", valid[i]));
		QUEUE_CHECK(rdpeudp_test_queue_configure_env(fixture->udp));
		QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
		QUEUE_CHECK(stats[1] == expected[i] && stats[32] == 5 && stats[33] == 5 && stats[35] == 31 && stats[36]);
	}
	for (size_t i = 0; i < ARRAYSIZE(invalid); i++)
	{
		QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_SEND_QUEUE_BYTES", invalid[i]));
		QUEUE_CHECK(!rdpeudp_test_queue_configure_env(fixture->udp));
	}
	QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_SEND_QUEUE_BYTES", nullptr));
	QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_QUEUE_OBSERVER", "no"));
	QUEUE_CHECK(!rdpeudp_test_queue_configure_env(fixture->udp));
	QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_QUEUE_OBSERVER", "0"));
	QUEUE_CHECK(rdpeudp_test_queue_configure_env(fixture->udp));
	QUEUE_CHECK(rdpeudp_test_queue_stats(fixture->udp, stats, QUEUE_STATS));
	QUEUE_CHECK(stats[1] == 32768 && !stats[36]);
	QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_SEND_QUEUE_BYTES", savedCap));
	QUEUE_CHECK(SetEnvironmentVariableA("RDP_BENCH_QUEUE_OBSERVER", savedObserver));
	free(savedCap);
	free(savedObserver);
	queue_fixture_free(fixture);
	return TRUE;
fail:
	(void)SetEnvironmentVariableA("RDP_BENCH_SEND_QUEUE_BYTES", savedCap);
	(void)SetEnvironmentVariableA("RDP_BENCH_QUEUE_OBSERVER", savedObserver);
	free(savedCap);
	free(savedObserver);
	queue_fixture_free(fixture);
	return FALSE;
}

int TestRdpeUdpQueueObserver(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	if (!test_queue_segment_boundaries() || !test_queue_compaction_and_cohort() ||
	    !test_queue_rejection_and_caps() || !test_queue_retries_and_callback() ||
	    !test_queue_all_cap_boundaries() || !test_queue_admission_wait() ||
	    !test_queue_observer_limits() || !test_queue_environment())
		return -1;
	printf("TestRdpeUdpQueueObserver passed\n");
	return 0;
}
