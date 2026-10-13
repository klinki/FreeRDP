/* Benchmark-only ownership/residence observer. Included after the private
 * transport definition; no allocation, clock reads, or cap override survives
 * a build without WITH_RDP_BENCH. This observer never changes ARQ decisions. */
#if defined(WITH_RDP_BENCH)
#include <winpr/sysinfo.h>

#define RDP_BENCH_QUEUE_SEGMENTS 1024
#define RDP_BENCH_QUEUE_HIST_BINS 16384
#define RDP_BENCH_QUEUE_HIST_WIDTH_NS 125000ULL
#define RDP_BENCH_QUEUE_STATS_COUNT 40
#define RDP_BENCH_FLIGHT_STATS_COUNT 16

typedef struct
{
	UINT64 acceptedNs;
	size_t remaining;
} RdpBenchQueueSegment;

struct RdpBenchQueueObserver
{
	BOOL valid;
	BOOL tracking;
	UINT64 lastClockNs;
	UINT64 measureStartNs;
	UINT64 measureStopNs;
	size_t head;
	size_t count;
	size_t segmentBytes;
	RdpBenchQueueSegment segments[RDP_BENCH_QUEUE_SEGMENTS];
	UINT64 hist[3][RDP_BENCH_QUEUE_HIST_BINS];
	UINT64 samples[3];
	UINT64 minimum[3];
	UINT64 maximum[3];
	UINT64 totals[3];
	UINT64 overflow[3];
	UINT64 peakOwned;
	UINT64 peakUnsent;
	UINT64 peakInFlight;
	UINT64 peakInFlightPackets;
	UINT64 acceptedCohort;
	UINT64 firstSentCohort;
	UINT64 clockErrors;
	UINT64 segmentOverflows;
	UINT64 counterOverflows;
};

static UINT64 rdp_bench_queue_raw_clock(const rdpUdpTransport* udp)
{
	return udp->testClockEnabled ? udp->testClockMs * 1000000ULL : winpr_GetTickCount64NS();
}

static UINT64 rdp_bench_queue_now_locked(rdpUdpTransport* udp)
{
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	if (!observer)
		return 0;
	if (udp->testClockEnabled && udp->testClockMs > UINT64_MAX / 1000000ULL)
	{
		observer->valid = FALSE;
		if (observer->clockErrors != UINT64_MAX)
			observer->clockErrors++;
		return observer->lastClockNs;
	}
	const UINT64 now = rdp_bench_queue_raw_clock(udp);
	if (now < observer->lastClockNs)
	{
		observer->valid = FALSE;
		if (observer->clockErrors != UINT64_MAX)
			observer->clockErrors++;
	}
	observer->lastClockNs = now;
	return now;
}

static BOOL rdp_bench_queue_cohort(const RdpBenchQueueObserver* observer, UINT64 now)
{
	return now >= observer->measureStartNs && now < observer->measureStopNs;
}

static void rdp_bench_queue_add(RdpBenchQueueObserver* observer, UINT64* target, UINT64 value)
{
	if (value > UINT64_MAX - *target)
	{
		observer->valid = FALSE;
		if (observer->counterOverflows != UINT64_MAX)
			observer->counterOverflows++;
		*target = UINT64_MAX;
	}
	else
		*target += value;
}

static void rdp_bench_queue_sample(RdpBenchQueueObserver* observer, UINT32 kind,
	                               UINT64 duration, UINT64 weight)
{
	rdp_bench_queue_add(observer, &observer->samples[kind], weight);
	if (duration < observer->minimum[kind])
		observer->minimum[kind] = duration;
	if (duration > observer->maximum[kind])
		observer->maximum[kind] = duration;
	if (weight && duration > UINT64_MAX / weight)
	{
		observer->valid = FALSE;
		if (observer->counterOverflows != UINT64_MAX)
			observer->counterOverflows++;
		observer->totals[kind] = UINT64_MAX;
	}
	else
		rdp_bench_queue_add(observer, &observer->totals[kind], duration * weight);
	const UINT64 bin = duration / RDP_BENCH_QUEUE_HIST_WIDTH_NS;
	if (bin >= RDP_BENCH_QUEUE_HIST_BINS)
	{
		observer->valid = FALSE;
		rdp_bench_queue_add(observer, &observer->overflow[kind], weight);
	}
	else
		rdp_bench_queue_add(observer, &observer->hist[kind][bin], weight);
}

static void rdp_bench_queue_peak_locked(rdpUdpTransport* udp, UINT64 now)
{
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	if (!observer || !rdp_bench_queue_cohort(observer, now))
		return;
	const size_t unsent = udp->sendQueue ? Stream_Length(udp->sendQueue) - udp->sendPos : 0;
	const size_t flight = udp->sendBytes >= unsent ? udp->sendBytes - unsent : 0;
	if (udp->sendBytes > observer->peakOwned)
		observer->peakOwned = udp->sendBytes;
	if (unsent > observer->peakUnsent)
		observer->peakUnsent = unsent;
	if (flight > observer->peakInFlight)
		observer->peakInFlight = flight;
	if (udp->sentCount > observer->peakInFlightPackets)
		observer->peakInFlightPackets = udp->sentCount;
}

static void rdp_bench_queue_accepted_locked(rdpUdpTransport* udp, size_t bytes, UINT64 now)
{
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	if (!observer)
		return;
	if (rdp_bench_queue_cohort(observer, now))
		rdp_bench_queue_add(observer, &observer->acceptedCohort, bytes);
	if (!observer->tracking)
		return;
	if (observer->count == RDP_BENCH_QUEUE_SEGMENTS)
	{
		observer->valid = FALSE;
		observer->tracking = FALSE;
		observer->segmentOverflows++;
		return;
	}
	const size_t index = (observer->head + observer->count) % RDP_BENCH_QUEUE_SEGMENTS;
	observer->segments[index].acceptedNs = now;
	observer->segments[index].remaining = bytes;
	observer->count++;
	observer->segmentBytes += bytes;
}

/* Called after successful first emission, using byte count and timestamp
 * captured before a socketless callback could acknowledge/free its entry. */
static void rdp_bench_queue_first_send_locked(rdpUdpTransport* udp, size_t bytes, UINT64 now)
{
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	if (!observer || !observer->tracking)
		return;
	while (bytes && observer->count)
	{
		RdpBenchQueueSegment* segment = &observer->segments[observer->head];
		const size_t consumed = bytes < segment->remaining ? bytes : segment->remaining;
		if (rdp_bench_queue_cohort(observer, segment->acceptedNs))
		{
			if (now < segment->acceptedNs)
			{
				observer->valid = FALSE;
				observer->clockErrors++;
			}
			else
			{
				rdp_bench_queue_sample(observer, 0, now - segment->acceptedNs, consumed);
				rdp_bench_queue_add(observer, &observer->firstSentCohort, consumed);
			}
		}
		bytes -= consumed;
		segment->remaining -= consumed;
		observer->segmentBytes -= consumed;
		if (!segment->remaining)
		{
			observer->head = (observer->head + 1) % RDP_BENCH_QUEUE_SEGMENTS;
			observer->count--;
		}
	}
	if (bytes)
	{
		observer->valid = FALSE;
		observer->tracking = FALSE;
		observer->segmentOverflows++;
	}
}

static void rdp_bench_queue_admission_locked(rdpUdpTransport* udp, UINT64 start,
	                                        UINT64 admitted, BOOL blocked)
{
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	if (!observer || !rdp_bench_queue_cohort(observer, start))
		return;
	if (admitted < start)
	{
		observer->valid = FALSE;
		observer->clockErrors++;
		return;
	}
	rdp_bench_queue_sample(observer, 1, admitted - start, 1);
	if (blocked)
		rdp_bench_queue_sample(observer, 2, admitted - start, 1);
}

static BOOL rdp_bench_queue_measure_locked(rdpUdpTransport* udp, UINT64 start, UINT64 stop)
{
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	if (start >= stop)
		return FALSE;
	if (!observer)
		return TRUE;
	/* Reset observations, preserving timestamps for previously accepted bytes. */
	memset(observer->hist, 0, sizeof(observer->hist));
	memset(observer->samples, 0, sizeof(observer->samples));
	memset(observer->maximum, 0, sizeof(observer->maximum));
	memset(observer->totals, 0, sizeof(observer->totals));
	memset(observer->overflow, 0, sizeof(observer->overflow));
	for (size_t i = 0; i < ARRAYSIZE(observer->minimum); i++)
		observer->minimum[i] = UINT64_MAX;
	observer->valid = observer->tracking;
	observer->measureStartNs = start;
	observer->measureStopNs = stop;
	observer->peakOwned = observer->peakUnsent = observer->peakInFlight = 0;
	observer->peakInFlightPackets = observer->acceptedCohort = observer->firstSentCohort = 0;
	observer->clockErrors = observer->segmentOverflows = observer->counterOverflows = 0;
	return TRUE;
}

static BOOL rdp_bench_queue_cap(const char* value, size_t* capacity)
{
	if (!capacity)
		return FALSE;
	*capacity = RDPEUDP2_SEND_QUEUE_BYTES;
	if (!value)
		return TRUE;
	const size_t allowed[] = { 32768, 49152, 65536, 131072 };
	const char* names[] = { "32768", "49152", "65536", "131072" };
	for (size_t i = 0; i < ARRAYSIZE(allowed); i++)
	{
		if (strcmp(value, names[i]) == 0)
		{
			*capacity = allowed[i];
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL rdp_bench_queue_configure(rdpUdpTransport* udp, size_t capacity, BOOL enabled)
{
	if (!udp || udp->sendBytes || udp->sentCount)
		return FALSE;
	RdpBenchQueueObserver* observer = enabled ? calloc(1, sizeof(*observer)) : nullptr;
	if (enabled && !observer)
		return FALSE;
	free(udp->benchQueueObserver);
	udp->benchQueueObserver = observer;
	udp->benchSendQueueBytes = capacity;
	if (observer)
	{
		observer->valid = observer->tracking = TRUE;
		for (size_t i = 0; i < ARRAYSIZE(observer->minimum); i++)
			observer->minimum[i] = UINT64_MAX;
		observer->lastClockNs = rdp_bench_queue_raw_clock(udp);
	}
	return TRUE;
}

static BOOL rdp_bench_queue_configure_env(rdpUdpTransport* udp)
{
	size_t capacity = 0;
	const char* enabled = getenv("RDP_BENCH_QUEUE_OBSERVER");
	if ((enabled && strcmp(enabled, "0") && strcmp(enabled, "1")) ||
	    !rdp_bench_queue_cap(getenv("RDP_BENCH_SEND_QUEUE_BYTES"), &capacity))
		return FALSE;
	return rdp_bench_queue_configure(udp, capacity, !enabled || strcmp(enabled, "0") != 0);
}

static BOOL rdp_bench_flight_configure_locked(rdpUdpTransport* udp, size_t cap)
{
	if (!udp || udp->sendBytes || udp->sentCount || udp->sendFailed || udp->recvFailed ||
	    (cap != 0 && cap != 16 && cap != 32 && cap != 48 && cap != 64))
		return FALSE;
	if (cap)
	{
		if (!udp->benchOriginalLogWindowSaved)
		{
			udp->benchOriginalLogWindow = udp->logWindow;
			udp->benchOriginalLogWindowSaved = TRUE;
		}
		/* All packet-count variants share the same protocol sequence credit.
		 * The independent cap limits outstanding logical DATA chunks. */
		udp->logWindow = 7;
	}
	else if (udp->benchOriginalLogWindowSaved)
	{
		udp->logWindow = udp->benchOriginalLogWindow;
		udp->benchOriginalLogWindowSaved = FALSE;
	}
	udp->benchMaxInFlightPackets = cap;
	udp->benchPeakInFlightPackets = udp->benchPeakActiveSpan = 0;
	return TRUE;
}

static BOOL rdp_bench_flight_configure_env(rdpUdpTransport* udp)
{
	const char* value = getenv("RDP_BENCH_MAX_INFLIGHT_PACKETS");
	size_t cap = 0;
	if (value)
	{
		const char* names[] = { "16", "32", "48", "64" };
		const size_t caps[] = { 16, 32, 48, 64 };
		BOOL found = FALSE;
		for (size_t i = 0; i < ARRAYSIZE(names); i++)
		{
			if (strcmp(value, names[i]) == 0)
			{
				cap = caps[i];
				found = TRUE;
				break;
			}
		}
		if (!found)
			return FALSE;
	}
	EnterCriticalSection(&udp->lock);
	const BOOL result = rdp_bench_flight_configure_locked(udp, cap);
	LeaveCriticalSection(&udp->lock);
	return result;
}

static void rdp_bench_flight_sent_locked(rdpUdpTransport* udp, size_t packets, size_t span)
{
	/* Values were captured before a socketless callback can retire this entry.
	 * Count them only after the complete DATA datagram was accepted by send. */
	if (packets > udp->benchPeakInFlightPackets)
		udp->benchPeakInFlightPackets = packets;
	if (span > udp->benchPeakActiveSpan)
		udp->benchPeakActiveSpan = span;
}

static BOOL rdp_bench_flight_stats_locked(rdpUdpTransport* udp, UINT64* out, size_t count)
{
	if (!udp || !out || count < RDP_BENCH_FLIGHT_STATS_COUNT)
		return FALSE;
	RdpBenchQueueObserver* observer = udp->benchQueueObserver;
	const UINT64 fields[RDP_BENCH_FLIGHT_STATS_COUNT] = {
		1,
		udp->benchMaxInFlightPackets,
		rdpeudp2_send_window_locked(udp),
		rdpeudp2_local_protocol_capacity_locked(udp),
		rdpeudp2_peer_protocol_capacity_locked(udp),
		ARRAYSIZE(udp->sent),
		rdpeudp2_usable_sent_storage_locked(udp),
		udp->logWindow,
		udp->peerLogWindow,
		udp->benchSendQueueBytes ? udp->benchSendQueueBytes : RDPEUDP2_SEND_QUEUE_BYTES,
		udp->sentCount,
		(UINT16)(udp->nextDataSeq - rdpeudp2_send_base_locked(udp, ARRAYSIZE(udp->sent))),
		udp->benchPeakInFlightPackets,
		udp->benchPeakActiveSpan,
		observer ? observer->peakInFlightPackets : 0,
		observer && observer->valid && observer->measureStopNs > observer->measureStartNs ? 1 : 0
	};
	memcpy(out, fields, sizeof(fields));
	return TRUE;
}

static BOOL rdp_bench_queue_stats_locked(rdpUdpTransport* udp, UINT64* out, size_t count)
{
	RdpBenchQueueObserver* o = udp->benchQueueObserver;
	if (!out || count < RDP_BENCH_QUEUE_STATS_COUNT)
		return FALSE;
	memset(out, 0, RDP_BENCH_QUEUE_STATS_COUNT * sizeof(*out));
	out[1] = udp->benchSendQueueBytes ? udp->benchSendQueueBytes : RDPEUDP2_SEND_QUEUE_BYTES;
	out[2] = RDP_BENCH_QUEUE_HIST_BINS;
	out[3] = RDP_BENCH_QUEUE_HIST_WIDTH_NS;
	out[32] = udp->logWindow;
	out[33] = udp->peerLogWindow;
	out[34] = udp->maxPayload;
	out[35] = rdpeudp2_send_window_locked(udp);
	out[36] = o ? 1 : 0;
	if (!o)
		return TRUE;
	const UINT64 values[32] = {
		o->valid, udp->benchSendQueueBytes, RDP_BENCH_QUEUE_HIST_BINS,
		RDP_BENCH_QUEUE_HIST_WIDTH_NS, o->samples[0],
		o->samples[0] ? o->minimum[0] : 0, o->maximum[0], o->totals[0],
		o->samples[1], o->samples[1] ? o->minimum[1] : 0, o->maximum[1], o->totals[1],
		o->samples[2], o->samples[2] ? o->minimum[2] : 0, o->maximum[2], o->totals[2],
		o->overflow[0], o->overflow[1], o->overflow[2], o->peakOwned, o->peakUnsent,
		o->peakInFlight, o->peakInFlightPackets, o->acceptedCohort, o->firstSentCohort,
		o->count, o->segmentBytes, o->clockErrors, o->segmentOverflows,
		o->counterOverflows, o->measureStartNs, o->measureStopNs
	};
	memcpy(out, values, sizeof(values));
	return TRUE;
}

FREERDP_API BOOL rdpeudp_test_queue_enable(rdpUdpTransport* udp, size_t capacity)
{
	if (capacity != 32768 && capacity != 49152 && capacity != 65536 && capacity != 131072)
		return FALSE;
	return rdp_bench_queue_configure(udp, capacity, TRUE);
}

FREERDP_API BOOL rdpeudp_test_queue_configure_env(rdpUdpTransport* udp)
{
	return rdp_bench_queue_configure_env(udp);
}

FREERDP_API BOOL rdpeudp_test_flight_configure(rdpUdpTransport* udp, size_t cap)
{
	if (!udp)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	const BOOL result = rdp_bench_flight_configure_locked(udp, cap);
	LeaveCriticalSection(&udp->lock);
	return result;
}

FREERDP_API BOOL rdpeudp_test_flight_configure_env(rdpUdpTransport* udp)
{
	return udp && rdp_bench_flight_configure_env(udp);
}

FREERDP_API BOOL rdpeudp_test_flight_stats(rdpUdpTransport* udp, UINT64* out, size_t count)
{
	if (!udp)
		return FALSE;
	EnterCriticalSection(&udp->lock);
	const BOOL result = rdp_bench_flight_stats_locked(udp, out, count);
	LeaveCriticalSection(&udp->lock);
	return result;
}

FREERDP_API BOOL rdpeudp_test_queue_measure(rdpUdpTransport* udp, UINT64 start, UINT64 stop)
{
	return rdp_bench_queue_measure_locked(udp, start, stop);
}

FREERDP_API BOOL rdpeudp_test_queue_stats(rdpUdpTransport* udp, UINT64* out, size_t count)
{
	return rdp_bench_queue_stats_locked(udp, out, count);
}

FREERDP_API BOOL rdpeudp_test_queue_hist(rdpUdpTransport* udp, UINT32 kind, UINT64* out, size_t count)
{
	if (!udp || !udp->benchQueueObserver || kind >= 3 || !out || count < RDP_BENCH_QUEUE_HIST_BINS)
		return FALSE;
	memcpy(out, udp->benchQueueObserver->hist[kind], sizeof(udp->benchQueueObserver->hist[kind]));
	return TRUE;
}

#else
#define rdp_bench_queue_now_locked(udp) 0ULL
#define rdp_bench_queue_peak_locked(udp, now) do { WINPR_UNUSED(udp); WINPR_UNUSED(now); } while (0)
#define rdp_bench_queue_accepted_locked(udp, bytes, now) \
	do { WINPR_UNUSED(udp); WINPR_UNUSED(bytes); WINPR_UNUSED(now); } while (0)
#define rdp_bench_queue_first_send_locked(udp, bytes, now) \
	do { WINPR_UNUSED(udp); WINPR_UNUSED(bytes); WINPR_UNUSED(now); } while (0)
#define rdp_bench_queue_admission_locked(udp, start, admitted, blocked) \
	do { WINPR_UNUSED(udp); WINPR_UNUSED(start); WINPR_UNUSED(admitted); WINPR_UNUSED(blocked); } while (0)
#endif
