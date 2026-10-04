/** Opt-in transport counters and passive RTT. Licensed under Apache-2.0. */
#include <freerdp/config.h>
#include <freerdp/freerdp.h>
#include <winpr/interlocked.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include "performance.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifdef __APPLE__
#include <sys/socket.h>
#include <netinet/tcp.h>
#endif

typedef struct
{
	CRITICAL_SECTION lock;
	LONG enabled;
	LONG64 epoch;
    LONG64 udpSentDatagrams, udpReceivedDatagrams, udpRetransmissions, udpLossReports;
    CRITICAL_SECTION decodeLock;
    UINT64 decodeCalls, decodeFailures, decodeWallNs;
    LONG64 decodeSamplesSkipped;
    UINT64 decodeDurations[256];
    char decodeCodec[32];
	LONG64 tcpIn, tcpOut, udpIn, udpOut;
	int tcpSocket;
	LONG udpConnections;
	BOOL rttValid[2];
	UINT32 rtt[2];
	UINT64 rttTime[2];
	BOOL bandwidthValid[2];
	double bandwidth[2];
	UINT32 bandwidthSource[2];
	UINT64 bandwidthTime[2];
} Performance;
static LONG64 counter_read(LONG64* value)
{
	return InterlockedCompareExchange64(value, 0, 0);
}
static void counter_write(LONG64* value, LONG64 replacement)
{
	LONG64 previous = counter_read(value);
	while (InterlockedCompareExchange64(value, replacement, previous) != previous)
		previous = counter_read(value);
}
static void counter_add(LONG64* value, LONG64 amount)
{
	LONG64 previous = counter_read(value);
	while (InterlockedCompareExchange64(value, previous + amount, previous) != previous)
		previous = counter_read(value);
}
static void reset_extended(Performance* p)
{
    counter_write(&p->udpSentDatagrams, 0);
    counter_write(&p->udpReceivedDatagrams, 0);
    counter_write(&p->udpRetransmissions, 0);
    counter_write(&p->udpLossReports, 0);
    counter_write(&p->decodeSamplesSkipped, 0);
    EnterCriticalSection(&p->decodeLock);
    p->decodeCalls = p->decodeFailures = p->decodeWallNs = 0;
    memset(p->decodeDurations, 0, sizeof(p->decodeDurations));
    p->decodeCodec[0] = '\0';
    LeaveCriticalSection(&p->decodeLock);
}
static Performance* get(rdpContext* ctx)
{
	return ctx ? (Performance*)ctx->performance : NULL;
}
BOOL performance_init(rdpMetrics* metrics)
{
	if (!metrics->context)
		return TRUE;
	Performance* p = (Performance*)calloc(1, sizeof(*p));
	if (!p)
		return FALSE;
	if (!InitializeCriticalSectionAndSpinCount(&p->lock, 4000))
	{
		free(p);
		return FALSE;
	}
    if (!InitializeCriticalSectionAndSpinCount(&p->decodeLock, 0))
    {
        DeleteCriticalSection(&p->lock);
        free(p);
        return FALSE;
    }
	p->tcpSocket = -1;
	metrics->context->performance = p;
	return TRUE;
}
void performance_free(rdpMetrics* metrics)
{
	Performance* p = metrics ? get(metrics->context) : NULL;
	if (p)
	{
		DeleteCriticalSection(&p->decodeLock);
		DeleteCriticalSection(&p->lock);
		free(p);
		metrics->context->performance = NULL;
	}
}
BOOL freerdp_performance_set_enabled(rdpContext* ctx, BOOL enabled)
{
	Performance* p = get(ctx);
	if (!p)
		return FALSE;
	EnterCriticalSection(&p->lock);
	if (p->enabled != !!enabled)
	{
		InterlockedExchange(&p->enabled, 0);
		counter_write(&p->tcpIn, 0);
		counter_write(&p->tcpOut, 0);
		counter_write(&p->udpIn, 0);
		counter_write(&p->udpOut, 0);
        counter_add(&p->epoch, 1);
        reset_extended(p);
        InterlockedExchange(&p->enabled, !!enabled);
	}
	LeaveCriticalSection(&p->lock);
	return TRUE;
}
void performance_account(rdpContext* ctx, BOOL udp, BOOL outbound, size_t bytes)
{
	Performance* p = get(ctx);
	if (!p || !bytes || !InterlockedCompareExchange(&p->enabled, 0, 0))
		return;
	// Accounting never takes the snapshot/RTT lock on an I/O thread.
	LONG64* counter =
	    udp ? (outbound ? &p->udpOut : &p->udpIn) : (outbound ? &p->tcpOut : &p->tcpIn);
	counter_add(counter, (LONG64)bytes);
}
void performance_tcp_socket(rdpContext* ctx, int fd)
{
	Performance* p = get(ctx);
	if (!p)
		return;
	EnterCriticalSection(&p->lock);
	if (fd >= 0)
	{
		counter_add(&p->epoch, 1);
		reset_extended(p);
		counter_write(&p->tcpIn, 0);
		counter_write(&p->tcpOut, 0);
		counter_write(&p->udpIn, 0);
		counter_write(&p->udpOut, 0);
		p->rttValid[0] = p->rttValid[1] = FALSE;
		p->bandwidthValid[0] = p->bandwidthValid[1] = FALSE;
	}
	p->tcpSocket = fd;
	LeaveCriticalSection(&p->lock);
}
void performance_udp_connected(rdpContext* ctx, BOOL connected)
{
	Performance* p = get(ctx);
	if (!p)
		return;
	EnterCriticalSection(&p->lock);
	p->udpConnections += connected ? 1 : -1;
	if (p->udpConnections < 0)
		p->udpConnections = 0;
	if (!p->udpConnections)
	{
		p->rttValid[1] = FALSE;
		p->bandwidthValid[1] = FALSE;
	}
	LeaveCriticalSection(&p->lock);
}
void performance_rtt_at(rdpContext* ctx, BOOL udp, UINT32 ms, UINT64 timestamp)
{
	Performance* p = get(ctx);
	if (!p)
		return;
	EnterCriticalSection(&p->lock);
	const size_t index = udp ? 1 : 0;
	p->rtt[index] = ms;
	p->rttTime[index] = timestamp;
	p->rttValid[index] = TRUE;
	LeaveCriticalSection(&p->lock);
}
void performance_rtt(rdpContext* ctx, BOOL udp, UINT32 ms)
{
	performance_rtt_at(ctx, udp, ms, GetTickCount64());
}
void performance_bandwidth_at(rdpContext* ctx, BOOL udp, double kbps, UINT32 source,
	                          UINT64 timestamp)
{
	Performance* p = get(ctx);
	if (!p || !isfinite(kbps) || kbps <= 0 ||
		(source != FREERDP_PERFORMANCE_BANDWIDTH_RDP_ESTIMATE &&
		 source != FREERDP_PERFORMANCE_BANDWIDTH_RDP_MEASUREMENT))
		return;
	EnterCriticalSection(&p->lock);
	const size_t index = udp ? 1 : 0;
	p->bandwidth[index] = kbps;
	p->bandwidthSource[index] = source;
	p->bandwidthTime[index] = timestamp;
	p->bandwidthValid[index] = TRUE;
	LeaveCriticalSection(&p->lock);
}
void performance_bandwidth(rdpContext* ctx, BOOL udp, UINT32 kbps)
{
	performance_bandwidth_at(ctx, udp, kbps, FREERDP_PERFORMANCE_BANDWIDTH_RDP_ESTIMATE,
	                         GetTickCount64());
}
void performance_bandwidth_measurement(rdpContext* ctx, BOOL udp, UINT32 ms, UINT32 bytes)
{
	// Bytes / milliseconds yields decimal kilobits/s after multiplying by eight.
	// Zero-duration and empty measurements cannot establish available bandwidth.
	if (ms && bytes)
		performance_bandwidth_at(ctx, udp, (double)bytes * 8.0 / ms,
	                             FREERDP_PERFORMANCE_BANDWIDTH_RDP_MEASUREMENT, GetTickCount64());
}
void performance_udp_stats(rdpContext* ctx, UINT64 sent, UINT64 received,
                           UINT64 retransmissions, UINT64 lossReports)
{
    Performance* p = get(ctx);
    if (!p || !InterlockedCompareExchange(&p->enabled, 0, 0))
        return;
    if (sent) counter_add(&p->udpSentDatagrams, (LONG64)sent);
    if (received) counter_add(&p->udpReceivedDatagrams, (LONG64)received);
    if (retransmissions) counter_add(&p->udpRetransmissions, (LONG64)retransmissions);
    if (lossReports) counter_add(&p->udpLossReports, (LONG64)lossReports);
}
performanceDecodeToken performance_decode_begin(rdpContext* ctx)
{
    performanceDecodeToken token = { 0 };
    Performance* p = get(ctx);
    if (p && InterlockedCompareExchange(&p->enabled, 0, 0))
    {
        token.epoch = (UINT64)counter_read(&p->epoch);
        token.startedNs = winpr_GetTickCount64NS();
    }
    return token;
}
void performance_decode_record(rdpContext* ctx, UINT64 epoch, UINT64 ns, BOOL success,
                               const char* codec)
{
    Performance* p = get(ctx);
    if (!p || !InterlockedCompareExchange(&p->enabled, 0, 0) || epoch != (UINT64)counter_read(&p->epoch))
        return;
    if (!TryEnterCriticalSection(&p->decodeLock))
    {
        counter_add(&p->decodeSamplesSkipped, 1);
        return;
    }
    if (InterlockedCompareExchange(&p->enabled, 0, 0) && epoch == (UINT64)counter_read(&p->epoch))
    {
        p->decodeDurations[p->decodeCalls % ARRAYSIZE(p->decodeDurations)] = ns;
        p->decodeCalls++;
        p->decodeWallNs += ns;
        if (!success) p->decodeFailures++;
        strncpy(p->decodeCodec, codec ? codec : "Unknown", sizeof(p->decodeCodec) - 1);
        p->decodeCodec[sizeof(p->decodeCodec) - 1] = '\0';
    }
    LeaveCriticalSection(&p->decodeLock);
}
void performance_decode_end(rdpContext* ctx, performanceDecodeToken token, BOOL success,
                            const char* codec)
{
    if (!token.startedNs) return;
    const UINT64 now = winpr_GetTickCount64NS();
    if (now >= token.startedNs)
        performance_decode_record(ctx, token.epoch, now - token.startedNs, success, codec);
}
static int compare_duration(const void* a, const void* b)
{
    const UINT64 x = *(const UINT64*)a, y = *(const UINT64*)b;
    return (x > y) - (x < y);
}
BOOL freerdp_performance_get_snapshot(rdpContext* ctx, rdpPerformanceSnapshot* result)
{
	Performance* p = get(ctx);
	if (!p || !result || (result->version != 1 && result->version != 2 && result->version != FREERDP_PERFORMANCE_VERSION))
		return FALSE;
    const size_t required = result->version == 1 ? FREERDP_PERFORMANCE_V1_SIZE
                            : result->version == 2 ? FREERDP_PERFORMANCE_V2_SIZE : sizeof(*result);
	if (result->size < required)
		return FALSE;
	rdpPerformanceSnapshot value = { 0 };
	rdpPerformanceSnapshot* out = &value;
	out->size = (UINT32)required;
	out->version = result->version;
	EnterCriticalSection(&p->lock);
	out->epoch = (UINT64)counter_read(&p->epoch);
	out->enabled = !!p->enabled;
	out->tcpInBytes = (UINT64)InterlockedCompareExchange64(&p->tcpIn, 0, 0);
	out->tcpOutBytes = (UINT64)InterlockedCompareExchange64(&p->tcpOut, 0, 0);
	out->udpInBytes = (UINT64)InterlockedCompareExchange64(&p->udpIn, 0, 0);
	out->udpOutBytes = (UINT64)InterlockedCompareExchange64(&p->udpOut, 0, 0);
	if (p->tcpSocket >= 0)
		out->connectedTransports |= FREERDP_PERFORMANCE_TCP;
	if (p->udpConnections > 0)
		out->connectedTransports |= FREERDP_PERFORMANCE_UDP;
	const UINT64 now = GetTickCount64();
	for (int step = 0; step < 2; step++)
	{
		const int index = (p->udpConnections > 0 && step == 0) ? 1 : 0;
		if (p->rttValid[index] && now >= p->rttTime[index] && now - p->rttTime[index] <= 15000)
		{
			out->rttValid = TRUE;
			out->rttMilliseconds = p->rtt[index];
			out->rttSource = FREERDP_PERFORMANCE_RTT_RDP;
			out->rttTransport = index ? FREERDP_PERFORMANCE_UDP : FREERDP_PERFORMANCE_TCP;
			out->rttAgeMilliseconds = now - p->rttTime[index];
			break;
		}
	}
#ifdef __APPLE__
	if (!out->rttValid && p->tcpSocket >= 0)
	{
		struct tcp_connection_info info = { 0 };
		socklen_t len = sizeof(info);
		if (getsockopt(p->tcpSocket, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &len) == 0 &&
		    info.tcpi_state == 4)
		{
			out->rttValid = TRUE;
			out->rttMilliseconds = info.tcpi_srtt;
			out->rttSource = FREERDP_PERFORMANCE_RTT_TCP_ESTIMATE;
			out->rttTransport = FREERDP_PERFORMANCE_TCP;
		}
	}
#endif
	// Prefer fresh measurements on the active graphics transport. Retain the last
	// known estimate with an explicit stale flag rather than pretending it is current.
	const int preferred = p->udpConnections > 0 ? 1 : 0;
	for (int stale = 0; stale < 2 && !out->bandwidthValid; stale++)
	{
		for (int step = 0; step < (preferred ? 2 : 1); step++)
		{
			const int index = step == 0 ? preferred : 0;
			if (!p->bandwidthValid[index] || now < p->bandwidthTime[index])
				continue;
			const UINT64 age = now - p->bandwidthTime[index];
			if ((age > 15000) != stale)
				continue;
			out->bandwidthValid = TRUE;
			out->bandwidthStale = age > 15000;
			out->bandwidthKilobitsPerSecond = p->bandwidth[index];
			out->bandwidthSource = p->bandwidthSource[index];
			out->bandwidthTransport = index ? FREERDP_PERFORMANCE_UDP : FREERDP_PERFORMANCE_TCP;
			out->bandwidthAgeMilliseconds = age;
			break;
		}
	}
    out->udpSentDatagrams = (UINT64)counter_read(&p->udpSentDatagrams);
    out->udpReceivedDatagrams = (UINT64)counter_read(&p->udpReceivedDatagrams);
    out->udpRetransmissions = (UINT64)counter_read(&p->udpRetransmissions);
    out->udpLossReports = (UINT64)counter_read(&p->udpLossReports);
    UINT64 durations[256] = { 0 };
    EnterCriticalSection(&p->decodeLock);
    out->decodeCalls = p->decodeCalls;
    out->decodeFailures = p->decodeFailures;
    out->decodeWallNs = p->decodeWallNs;
    out->decodeSamplesSkipped = (UINT64)counter_read(&p->decodeSamplesSkipped);
    out->decodeRecentSampleCount = (UINT32)MIN(p->decodeCalls, ARRAYSIZE(durations));
    memcpy(durations, p->decodeDurations, sizeof(durations));
    memcpy(out->decodeCodec, p->decodeCodec, sizeof(out->decodeCodec));
    LeaveCriticalSection(&p->decodeLock);
    LeaveCriticalSection(&p->lock);
    out->decodeRecentP95Milliseconds = NAN;
    if (out->decodeRecentSampleCount)
    {
        qsort(durations, out->decodeRecentSampleCount, sizeof(UINT64), compare_duration);
        const size_t index = (out->decodeRecentSampleCount * 95 + 99) / 100 - 1;
        out->decodeRecentP95Milliseconds = durations[index] / 1e6;
    }
	// Version 1 callers receive only the original prefix, including on repeated calls.
	memcpy(result, out, required);
	return TRUE;
}
