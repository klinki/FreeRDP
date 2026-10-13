/* Included after rdpUdp's private definition. The native sender owns all chunks;
 * these hooks measure ownership and emissions without implementing reliability. */
#if defined(WITH_RDP_BENCH)
static UINT32 sender_base(const rdpUdp* udp);
static size_t window_limit(const rdpUdp* udp);

static UINT64 rdpudp_bench_now(rdpUdp* udp)
{
	const UINT64 now = winpr_GetTickCount64NS();
	RdpUdpBenchObserver* o = udp->bench ? udp->bench->observer : nullptr;
	if (o)
	{
		if (now < o->lastClock)
		{
			o->valid = FALSE;
			o->clockErrors++;
		}
		o->lastClock = now;
	}
	return now;
}

static BOOL rdpudp_bench_cohort(const RdpUdpBenchObserver* o, UINT64 now)
{
	return o && (now >= o->start) && (now < o->stop);
}

static void rdpudp_bench_add(RdpUdpBenchObserver* o, UINT64* value, UINT64 increment)
{
	if (increment > UINT64_MAX - *value)
	{
		o->valid = FALSE;
		o->counterErrors++;
		*value = UINT64_MAX;
	}
	else
		*value += increment;
}

static void rdpudp_bench_sample(RdpUdpBenchObserver* o, size_t kind, UINT64 duration, UINT64 weight)
{
	rdpudp_bench_add(o, &o->samples[kind], weight);
	if (duration < o->minimum[kind])
		o->minimum[kind] = duration;
	if (duration > o->maximum[kind])
		o->maximum[kind] = duration;
	if (weight && (duration > UINT64_MAX / weight))
	{
		o->valid = FALSE;
		o->counterErrors++;
		o->total[kind] = UINT64_MAX;
	}
	else
		rdpudp_bench_add(o, &o->total[kind], duration * weight);
	const UINT64 bin = duration / RDPUDP_BENCH_HIST_NS;
	if (bin >= RDPUDP_BENCH_HIST_BINS)
	{
		o->valid = FALSE;
		rdpudp_bench_add(o, &o->overflow[kind], weight);
	}
	else
		rdpudp_bench_add(o, &o->hist[kind][bin], weight);
}

static size_t rdpudp_bench_span(const rdpUdp* udp)
{
	return (size_t)((udp->nextCoded - sender_base(udp)) & 0xFFFFu);
}

static void rdpudp_bench_peak(rdpUdp* udp, UINT64 now)
{
	RdpUdpBench* b = udp->bench;
	if (!b)
		return;
	if (udp->inFlight > b->peakFlight)
		b->peakFlight = udp->inFlight;
	const size_t span = rdpudp_bench_span(udp);
	if (span > b->peakSpan)
		b->peakSpan = span;
	RdpUdpBenchObserver* o = b->observer;
	if (!rdpudp_bench_cohort(o, now))
		return;
	const size_t flight = b->ownedBytes - b->queuedBytes;
	if (b->ownedBytes > o->peakOwned)
		o->peakOwned = b->ownedBytes;
	if (b->queuedBytes > o->peakQueued)
		o->peakQueued = b->queuedBytes;
	if (flight > o->peakFlightBytes)
		o->peakFlightBytes = flight;
	if (udp->inFlight > o->peakFlightPackets)
		o->peakFlightPackets = udp->inFlight;
}

static void rdpudp_bench_accepted(rdpUdp* udp, rdpudp_chunk* chunk)
{
	RdpUdpBench* b = udp->bench;
	if (!b)
		return;
	chunk->benchAccepted = rdpudp_bench_now(udp);
	b->ownedBytes += chunk->length;
	b->queuedBytes += chunk->length;
	b->firstPendingBytes += chunk->length;
	b->firstPendingCount++;
	if (rdpudp_bench_cohort(b->observer, chunk->benchAccepted))
		rdpudp_bench_add(b->observer, &b->observer->acceptedBytes, chunk->length);
	rdpudp_bench_peak(udp, chunk->benchAccepted);
}

static void rdpudp_bench_dequeued(rdpUdp* udp, const rdpudp_chunk* chunk)
{
	if (!udp->bench)
		return;
	udp->bench->dequeuedAccepted = chunk->benchAccepted;
	udp->bench->queuedBytes -= chunk->length;
}

static void rdpudp_bench_transmitted(rdpUdp* udp, rdpudp_pending* p)
{
	RdpUdpBench* b = udp->bench;
	if (!b)
		return;
	const UINT64 now = rdpudp_bench_now(udp);
	rdpudp_bench_peak(udp, now);
	/* Upstream treats transient socket failures as lost attempts. They remain
	 * native pending chunks, but have not yet had a first actual wire emission. */
	if (!b->lastRawSendComplete || p->benchFirstSent)
		return;
	p->benchFirstSent = TRUE;
	b->firstPendingCount--;
	b->firstPendingBytes -= p->length;
	RdpUdpBenchObserver* o = b->observer;
	if (rdpudp_bench_cohort(o, p->benchAccepted))
	{
		if (now < p->benchAccepted)
		{
			o->valid = FALSE;
			o->clockErrors++;
		}
		else
		{
			rdpudp_bench_sample(o, 0, now - p->benchAccepted, p->length);
			rdpudp_bench_add(o, &o->firstSentBytes, p->length);
		}
	}
}

static void rdpudp_bench_retired(rdpUdp* udp, const rdpudp_pending* p)
{
	if (!udp->bench)
		return;
	udp->bench->ownedBytes -= p->length;
	if (!p->benchFirstSent)
	{
		udp->bench->firstPendingCount--;
		udp->bench->firstPendingBytes -= p->length;
	}
}

static BOOL rdpudp_bench_deadline(rdpUdp* udp)
{
	RdpUdpBench* b = udp->bench;
	if (!b || !b->deadline || (rdpudp_bench_now(udp) < b->deadline))
		return FALSE;
	b->deadlineReached = TRUE;
	return TRUE;
}

static BOOL rdpudp_bench_measure(rdpUdp* udp, UINT64 start, UINT64 stop)
{
	if (!udp || (start >= stop))
		return FALSE;
	RdpUdpBenchObserver* o = udp->bench ? udp->bench->observer : nullptr;
	if (!o)
		return TRUE;
	memset(o, 0, sizeof(*o));
	o->valid = TRUE;
	o->start = start;
	o->stop = stop;
	o->lastClock = winpr_GetTickCount64NS();
	for (size_t i = 0; i < 3; i++)
		o->minimum[i] = UINT64_MAX;
	return TRUE;
}

static void rdpudp_bench_admission(rdpUdp* udp, UINT64 start, UINT64 admitted, BOOL blocked)
{
	RdpUdpBenchObserver* o = udp->bench ? udp->bench->observer : nullptr;
	if (!rdpudp_bench_cohort(o, start))
		return;
	if (admitted < start)
	{
		o->valid = FALSE;
		o->clockErrors++;
		return;
	}
	rdpudp_bench_sample(o, 1, admitted - start, 1);
	if (blocked)
		rdpudp_bench_sample(o, 2, admitted - start, 1);
}

static BOOL rdpudp_bench_queue_stats(rdpUdp* udp, UINT64* out, size_t count)
{
	if (!udp || !udp->bench || !out || (count < 40))
		return FALSE;
	RdpUdpBench* b = udp->bench;
	RdpUdpBenchObserver* o = b->observer;
	memset(out, 0, 40 * sizeof(*out));
	out[1] = b->queueLimit;
	out[2] = RDPUDP_BENCH_HIST_BINS;
	out[3] = RDPUDP_BENCH_HIST_NS;
	out[25] = b->firstPendingCount;
	out[26] = b->firstPendingBytes;
	out[32] = RDPUDP2_LOG_WINDOW_SIZE;
	out[33] = b->peerLog;
	out[34] = rdpudp_get_max_payload(udp);
	out[35] = window_limit(udp);
	out[36] = o ? 1 : 0;
	if (!o)
		return TRUE;
	const UINT64 fields[32] = { o->valid,
		                        b->queueLimit,
		                        RDPUDP_BENCH_HIST_BINS,
		                        RDPUDP_BENCH_HIST_NS,
		                        o->samples[0],
		                        o->samples[0] ? o->minimum[0] : 0,
		                        o->maximum[0],
		                        o->total[0],
		                        o->samples[1],
		                        o->samples[1] ? o->minimum[1] : 0,
		                        o->maximum[1],
		                        o->total[1],
		                        o->samples[2],
		                        o->samples[2] ? o->minimum[2] : 0,
		                        o->maximum[2],
		                        o->total[2],
		                        o->overflow[0],
		                        o->overflow[1],
		                        o->overflow[2],
		                        o->peakOwned,
		                        o->peakQueued,
		                        o->peakFlightBytes,
		                        o->peakFlightPackets,
		                        o->acceptedBytes,
		                        o->firstSentBytes,
		                        b->firstPendingCount,
		                        b->firstPendingBytes,
		                        o->clockErrors,
		                        0,
		                        o->counterErrors,
		                        o->start,
		                        o->stop };
	memcpy(out, fields, sizeof(fields));
	return TRUE;
}

static BOOL rdpudp_bench_flight_stats(rdpUdp* udp, UINT64* out, size_t count)
{
	if (!udp || !udp->bench || !out || (count < 16))
		return FALSE;
	RdpUdpBench* b = udp->bench;
	RdpUdpBenchObserver* o = b->observer;
	const UINT64 fields[16] = { 1,
		                        b->cap,
		                        window_limit(udp),
		                        ((size_t)1 << RDPUDP2_LOG_WINDOW_SIZE) - 1,
		                        ((size_t)1 << b->peerLog) - 1,
		                        RDPUDP_MAX_IN_FLIGHT,
		                        RDPUDP_MAX_IN_FLIGHT,
		                        RDPUDP2_LOG_WINDOW_SIZE,
		                        b->peerLog,
		                        b->queueLimit,
		                        udp->inFlight,
		                        rdpudp_bench_span(udp),
		                        b->peakFlight,
		                        b->peakSpan,
		                        o ? o->peakFlightPackets : 0,
		                        o && o->valid && (o->start < o->stop) ? 1 : 0 };
	memcpy(out, fields, sizeof(fields));
	return TRUE;
}
#endif
