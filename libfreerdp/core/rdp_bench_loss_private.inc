/* Diagnostic-only reliable UDP2 trace. No file I/O or formatting happens in
 * the transport. Normal builds compile hooks away; optional benchmark traces
 * retain a bounded array and mark missing observations invalid on overflow. */
enum
{
	RDP_BENCH_LOSS_NONE = 0,
	RDP_BENCH_LOSS_ENQUEUE_BLOCK = 1,
	RDP_BENCH_LOSS_ENQUEUE_ACCEPT = 2,
	RDP_BENCH_LOSS_DATA_SEND = 3,
	RDP_BENCH_LOSS_RETRY_TIMER = 4,
	RDP_BENCH_LOSS_RETRY_FAST = 5,
	RDP_BENCH_LOSS_ACK_POSITIVE = 6,
	RDP_BENCH_LOSS_NACK_GAP = 7,
	RDP_BENCH_LOSS_RETIRE = 8,
	RDP_BENCH_LOSS_RECEIVE = 9,
	RDP_BENCH_LOSS_PUMP_ENTER = 10,
	RDP_BENCH_LOSS_PUMP_EXIT = 11,
	RDP_BENCH_LOSS_ADMISSION_BEGIN = 12,
	RDP_BENCH_LOSS_ADMISSION_END = 13,
	RDP_BENCH_LOSS_WAIT_BEGIN = 14,
	RDP_BENCH_LOSS_WAIT_END = 15
};

#if defined(WITH_RDP_BENCH)
#include <winpr/sysinfo.h>

#define RDP_BENCH_LOSS_TRACE_CAPACITY 524288ULL
#define RDP_BENCH_LOSS_TRACE_COLUMNS 10
#define RDP_BENCH_LOSS_TRACE_STATS 6

struct RdpBenchLossTrace
{
	BOOL valid;
	UINT64 count;
	UINT64 overflow;
	UINT64 lastClockNs;
	UINT64 rows[RDP_BENCH_LOSS_TRACE_CAPACITY][RDP_BENCH_LOSS_TRACE_COLUMNS];
};

static void rdp_bench_loss_free(rdpUdpTransport* udp)
{
	if (!udp)
		return;
	free(udp->benchLossTrace);
	udp->benchLossTrace = nullptr;
}

static BOOL rdp_bench_loss_configure_env(rdpUdpTransport* udp)
{
	const char* enabled = getenv("RDP_BENCH_LOSS_TRACE");
	const char* mode = getenv("RDP_BENCH_LOSS_RECOVERY_MODE");
	if (!udp || (enabled && strcmp(enabled, "0") && strcmp(enabled, "1")) ||
	    (mode && strcmp(mode, "fast") && strcmp(mode, "timer")))
		return FALSE;
	RdpBenchLossTrace* trace = enabled && strcmp(enabled, "1") == 0
	                               ? calloc(1, sizeof(*trace)) : nullptr;
	if (enabled && strcmp(enabled, "1") == 0 && !trace)
		return FALSE;
	rdp_bench_loss_free(udp);
	udp->benchLossTrace = trace;
	udp->benchTimerRecovery = mode && strcmp(mode, "timer") == 0;
	if (trace)
		trace->valid = TRUE;
	return TRUE;
}

static BOOL rdp_bench_loss_fast_recovery(const rdpUdpTransport* udp)
{
	return !udp->benchTimerRecovery;
}

/* Caller owns udp->lock. Sequence snapshots describe latest transmission
 * attempts, not historical aliases retained for delayed positive ACKs. */
static void rdp_bench_loss_event_locked(rdpUdpTransport* udp, UINT32 kind, UINT16 dataSeq,
	                                    UINT16 channelSeq, UINT64 detail, UINT32 retries)
{
	RdpBenchLossTrace* trace = udp ? udp->benchLossTrace : nullptr;
	if (!trace)
		return;
	if (trace->count == RDP_BENCH_LOSS_TRACE_CAPACITY)
	{
		trace->valid = FALSE;
		if (trace->overflow != UINT64_MAX)
			trace->overflow++;
		return;
	}
	UINT64 now = 0;
	if (udp->testClockEnabled)
	{
		if (udp->testClockMs > UINT64_MAX / 1000000ULL)
			trace->valid = FALSE;
		else
			now = udp->testClockMs * 1000000ULL;
	}
	else
		now = winpr_GetTickCount64NS();
	if (now < trace->lastClockNs)
		trace->valid = FALSE;
	trace->lastClockNs = now;
	UINT16 activeSpan = 0;
	for (size_t i = 0; i < ARRAYSIZE(udp->sent); i++)
	{
		if (!udp->sent[i].data)
			continue;
		const UINT16 age = (UINT16)(udp->nextDataSeq - udp->sent[i].dataSeq);
		if (age > activeSpan)
			activeSpan = age;
	}
	const size_t unsent = udp->sendQueue ? Stream_Length(udp->sendQueue) - udp->sendPos : 0;
	const UINT64 row[RDP_BENCH_LOSS_TRACE_COLUMNS] = {
		now, kind, dataSeq, channelSeq, detail, udp->sendBytes, unsent,
		udp->sentCount, activeSpan, retries
	};
	memcpy(trace->rows[trace->count++], row, sizeof(row));
}

/* For hooks outside an existing critical section. Configuration is immutable
 * after endpoint open, so disabled tracing needs neither a clock nor a lock. */
static void rdp_bench_loss_event(rdpUdpTransport* udp, UINT32 kind, UINT16 dataSeq,
	                             UINT16 channelSeq, UINT64 detail, UINT32 retries)
{
	if (!udp || !udp->benchLossTrace)
		return;
	EnterCriticalSection(&udp->lock);
	rdp_bench_loss_event_locked(udp, kind, dataSeq, channelSeq, detail, retries);
	LeaveCriticalSection(&udp->lock);
}

static BOOL rdp_bench_loss_stats_locked(const rdpUdpTransport* udp, UINT64* out, size_t count)
{
	if (!udp || !out || count < RDP_BENCH_LOSS_TRACE_STATS)
		return FALSE;
	const RdpBenchLossTrace* trace = udp->benchLossTrace;
	const UINT64 stats[RDP_BENCH_LOSS_TRACE_STATS] = {
		trace ? 1 : 0, trace ? trace->valid : 0, trace ? trace->count : 0,
		RDP_BENCH_LOSS_TRACE_CAPACITY, trace ? trace->overflow : 0,
		udp->benchTimerRecovery
	};
	memcpy(out, stats, sizeof(stats));
	return TRUE;
}

static size_t rdp_bench_loss_read_locked(const rdpUdpTransport* udp, size_t start,
	                                    UINT64* out, size_t maxRows)
{
	const RdpBenchLossTrace* trace = udp ? udp->benchLossTrace : nullptr;
	if (!trace || !out || !maxRows || start >= trace->count)
		return 0;
	const size_t remaining = (size_t)trace->count - start;
	const size_t rows = maxRows < remaining ? maxRows : remaining;
	memcpy(out, trace->rows[start], rows * sizeof(trace->rows[0]));
	return rows;
}
#else
#define rdp_bench_loss_fast_recovery(udp) TRUE
#define rdp_bench_loss_event_locked(udp, kind, dataSeq, channelSeq, detail, retries) \
	do { WINPR_UNUSED(udp); WINPR_UNUSED(kind); WINPR_UNUSED(dataSeq); \
	     WINPR_UNUSED(channelSeq); WINPR_UNUSED(detail); WINPR_UNUSED(retries); } while (0)
#define rdp_bench_loss_event(udp, kind, dataSeq, channelSeq, detail, retries) \
	rdp_bench_loss_event_locked(udp, kind, dataSeq, channelSeq, detail, retries)
#endif
