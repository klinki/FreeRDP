/** Opt-in transport counters and passive RTT. Licensed under Apache-2.0. */
#include <freerdp/config.h>
#include <freerdp/freerdp.h>
#include <winpr/interlocked.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include "performance.h"
#include <string.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <sys/socket.h>
#include <netinet/tcp.h>
#endif

typedef struct
{
	CRITICAL_SECTION lock;
	LONG enabled;
	UINT64 epoch;
	LONG64 tcpIn, tcpOut, udpIn, udpOut;
	int tcpSocket;
	LONG udpConnections;
	BOOL rttValid[2];
	UINT32 rtt[2];
	UINT64 rttTime[2];
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
	p->tcpSocket = -1;
	metrics->context->performance = p;
	return TRUE;
}
void performance_free(rdpMetrics* metrics)
{
	Performance* p = metrics ? get(metrics->context) : NULL;
	if (p)
	{
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
		InterlockedExchange(&p->enabled, !!enabled);
		counter_write(&p->tcpIn, 0);
		counter_write(&p->tcpOut, 0);
		counter_write(&p->udpIn, 0);
		counter_write(&p->udpOut, 0);
		++p->epoch;
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
		++p->epoch;
		counter_write(&p->tcpIn, 0);
		counter_write(&p->tcpOut, 0);
		counter_write(&p->udpIn, 0);
		counter_write(&p->udpOut, 0);
		p->rttValid[0] = p->rttValid[1] = FALSE;
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
		p->rttValid[1] = FALSE;
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
BOOL freerdp_performance_get_snapshot(rdpContext* ctx, rdpPerformanceSnapshot* out)
{
	Performance* p = get(ctx);
	if (!p || !out || out->size < sizeof(*out) || out->version != FREERDP_PERFORMANCE_VERSION)
		return FALSE;
	EnterCriticalSection(&p->lock);
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->version = FREERDP_PERFORMANCE_VERSION;
	out->epoch = p->epoch;
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
	LeaveCriticalSection(&p->lock);
	return TRUE;
}
