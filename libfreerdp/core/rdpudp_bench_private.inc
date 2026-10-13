/* Development-only post-negotiation raw UDP2 adapter. Calls upstream rdpudp_send/check;
 * no TLS, SYN, alternative ARQ, ACK repair, alias tracking, or retry policy lives here.
 * One owner serializes every entry point, as required by rdpudp.h. */
#if defined(WITH_RDP_BENCH)
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>

typedef struct
{
	rdpUdp* udp;
	RdpUdpBench state;
	wStream* received;
	size_t readPosition;
	const char* driverError;
} FreeRdpBenchV1;

FREERDP_API void freerdp_bench_v1_close(void* handle);

static BOOL rdpudp_bench_receive(void* custom, const BYTE* data, size_t length)
{
	FreeRdpBenchV1* b = custom;
	if (!b || !b->received || !data)
		return FALSE;
	const size_t unread = Stream_Length(b->received) - b->readPosition;
	if ((length > RDPUDP_BENCH_RECV_BYTES) || (unread > RDPUDP_BENCH_RECV_BYTES - length))
	{
		b->driverError = "BENCHMARK_DRIVER_UNREAD_LIMIT: 64 MiB callback buffer exhausted";
		return FALSE;
	}
	if (unread && b->readPosition)
		memmove(Stream_Buffer(b->received), Stream_Buffer(b->received) + b->readPosition, unread);
	b->readPosition = 0;
	if (!Stream_SetPosition(b->received, unread) || !Stream_SetLength(b->received, unread) ||
	    !Stream_EnsureRemainingCapacity(b->received, length))
	{
		b->driverError = "BENCHMARK_DRIVER_UNREAD_ALLOCATION: callback buffer growth failed";
		return FALSE;
	}
	Stream_Write(b->received, data, length);
	Stream_SealLength(b->received);
	return TRUE;
}

static BOOL rdpudp_bench_env(RdpUdpBench* b)
{
	const char* cap = getenv("RDP_BENCH_MAX_INFLIGHT_PACKETS");
	const char* queue = getenv("RDP_BENCH_SEND_QUEUE_BYTES");
	const char* observer = getenv("RDP_BENCH_QUEUE_OBSERVER");
	const char* counters = getenv("RDP_BENCH_COUNTERS");
	const size_t caps[] = { 16, 32, 48, 64 };
	const char* capNames[] = { "16", "32", "48", "64" };
	const size_t queues[] = { 32768, 49152, 65536, 131072 };
	const char* queueNames[] = { "32768", "49152", "65536", "131072" };
	b->queueLimit = RDPUDP_BENCH_QUEUE_BYTES;
	b->peerLog = RDPUDP2_LOG_WINDOW_SIZE;
	b->countersEnabled = !counters || strcmp(counters, "0");
	if (cap)
	{
		for (size_t x = 0; x < ARRAYSIZE(caps); x++)
		{
			if (strcmp(cap, capNames[x]) == 0)
				b->cap = caps[x];
		}
		if (!b->cap)
			return FALSE;
	}
	if (queue)
	{
		BOOL found = FALSE;
		for (size_t x = 0; x < ARRAYSIZE(queues); x++)
		{
			if (strcmp(queue, queueNames[x]) == 0)
			{
				b->queueLimit = queues[x];
				found = TRUE;
			}
		}
		if (!found)
			return FALSE;
	}
	if (observer && strcmp(observer, "0") && strcmp(observer, "1"))
		return FALSE;
	if (!observer || strcmp(observer, "0"))
	{
		b->observer = calloc(1, sizeof(*b->observer));
		if (!b->observer)
			return FALSE;
		b->observer->valid = TRUE;
		b->observer->lastClock = winpr_GetTickCount64NS();
		for (size_t x = 0; x < 3; x++)
			b->observer->minimum[x] = UINT64_MAX;
	}
	return TRUE;
}

static BOOL rdpudp_bench_wait(FreeRdpBenchV1* b, UINT32 timeout)
{
	if (!b || !b->udp || rdpudp_bench_deadline(b->udp))
		return FALSE;
	const SOCKET socket = b->udp->sockfd;
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(socket, &readSet);
	struct timeval tv = { .tv_sec = timeout / 1000, .tv_usec = (timeout % 1000) * 1000 };
	const int result = select(socket + 1, &readSet, nullptr, nullptr, &tv);
	if ((result >= 0) || (errno == EINTR))
		return TRUE;
	b->driverError = "BENCHMARK_DRIVER_WAIT: select failed";
	return FALSE;
}

FREERDP_API const char* freerdp_bench_v1_driver_error(void* handle)
{
	const FreeRdpBenchV1* b = handle;
	return b ? b->driverError : nullptr;
}

FREERDP_API int freerdp_bench_v1_backend(void)
{
	/* Backend 2: the upstream PR has one FAILED state and no loss-report counter. */
	return 2;
}

FREERDP_API void* freerdp_bench_v1_open(const char* bindAddress, UINT16 port,
                                        const char* peerAddress, UINT16 peerPort)
{
	if (!bindAddress || !peerAddress || !port || !peerPort)
		return nullptr;
	FreeRdpBenchV1* b = calloc(1, sizeof(*b));
	if (!b)
		return nullptr;
	if (!rdpudp_bench_env(&b->state))
		goto fail;
	b->received = Stream_New(nullptr, 65536);
	if (!b->received)
		goto fail;
	if (!Stream_SetLength(b->received, 0))
		goto fail;
	const BYTE cookie[32] = WINPR_C_ARRAY_INIT;
	b->udp = rdpudp_new(WLog_Get("com.freerdp.core.rdpudp.bench"), cookie, rdpudp_bench_receive, b);
	if (!b->udp)
		goto fail;
	b->udp->bench = &b->state;
	b->udp->sockfd = _socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if ((b->udp->sockfd == INVALID_SOCKET) || (b->udp->sockfd >= FD_SETSIZE))
		goto fail;
	struct sockaddr_in local = { 0 }, peer = { 0 };
	local.sin_family = peer.sin_family = AF_INET;
	local.sin_port = htons(port);
	peer.sin_port = htons(peerPort);
	if ((inet_pton(AF_INET, bindAddress, &local.sin_addr) != 1) ||
	    (inet_pton(AF_INET, peerAddress, &peer.sin_addr) != 1))
		goto fail;
	if ((bind(b->udp->sockfd, (struct sockaddr*)&local, sizeof(local)) != 0) ||
	    (_connect(b->udp->sockfd, (struct sockaddr*)&peer, sizeof(peer)) != 0) ||
	    !set_non_blocking(b->udp->sockfd))
		goto fail;
	const int bufferBytes = 4 * 1024 * 1024;
	(void)_setsockopt(b->udp->sockfd, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferBytes,
	                  sizeof(bufferBytes));
	(void)_setsockopt(b->udp->sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&bufferBytes,
	                  sizeof(bufferBytes));
	/* Both raw benchmark peers declare version3/MTU1232. This skips connection setup,
	 * preserving upstream's natural Log15, initial sequences, timers and codec. */
	b->udp->version = RDPUDP_PROTOCOL_VERSION_3;
	b->udp->mtu = RDPUDP_MTU;
	b->udp->state = RDPUDP_STATE_ESTABLISHED;
	b->udp->nextCoded = RDPUDP2_INITIAL_SEQUENCE;
	b->udp->nextChannelSeq = b->udp->v3NextChannel = 1;
	b->udp->v3MaxDelayedAcks = RDPUDP2_DEFAULT_DELAYED_ACKS;
	b->udp->v3DelayedAckTimeout = UINT32_MAX;
	b->udp->lastSent = b->udp->lastReceived = b->udp->lastDelivery = now_ms();
	return b;
fail:
	freerdp_bench_v1_close(b);
	return nullptr;
}

FREERDP_API INT64 freerdp_bench_v1_send(void* handle, const BYTE* data, size_t length)
{
	FreeRdpBenchV1* b = handle;
	if (!b || !b->udp || !data || !length || (length > 60000) || (length > b->state.queueLimit))
		return -1;
	const UINT64 started = rdpudp_bench_now(b->udp);
	BOOL blocked = FALSE;
	/* Budget covers native queued chunks plus every logical pending chunk. It is
	 * independent of packet count. No upper layer retries a partially accepted call. */
	while (b->state.ownedBytes > b->state.queueLimit - length)
	{
		blocked = TRUE;
		if (!rdpudp_check(b->udp))
			return -1;
		/* Native ACK processing may already have made the whole write fit. */
		if (b->state.ownedBytes <= b->state.queueLimit - length)
			break;
		if (!rdpudp_bench_wait(b, 10))
			return -1;
	}
	if (rdpudp_bench_deadline(b->udp) || !rdpudp_check(b->udp))
		return -1;
	const size_t maximum = rdpudp_get_max_payload(b->udp);
	if (!maximum)
		return -1;
	for (size_t offset = 0; offset < length;)
	{
		const size_t remaining = length - offset;
		const size_t chunk = remaining < maximum ? remaining : maximum;
		if (!rdpudp_send(b->udp, data + offset, chunk))
			return -1;
		offset += chunk;
	}
	/* Only a fully accepted application call contributes an admission sample. */
	rdpudp_bench_admission(b->udp, started, rdpudp_bench_now(b->udp), blocked);
	return WINPR_ASSERTING_INT_CAST(INT64, length);
}

FREERDP_API INT64 freerdp_bench_v1_read(void* handle, BYTE* data, size_t capacity, UINT32 timeoutMs)
{
	FreeRdpBenchV1* b = handle;
	if (!b || !b->udp || !data || !capacity)
		return -1;
	const UINT64 start = now_ms();
	do
	{
		if (!rdpudp_check(b->udp))
			return -1;
		const size_t unread = Stream_Length(b->received) - b->readPosition;
		if (unread)
		{
			const size_t copied = unread < capacity ? unread : capacity;
			memcpy(data, Stream_Buffer(b->received) + b->readPosition, copied);
			b->readPosition += copied;
			if (b->readPosition == Stream_Length(b->received))
			{
				b->readPosition = 0;
				(void)Stream_SetLength(b->received, 0);
				(void)Stream_SetPosition(b->received, 0);
			}
			return WINPR_ASSERTING_INT_CAST(INT64, copied);
		}
		const UINT64 elapsed = now_ms() - start;
		if (elapsed >= timeoutMs)
			return 0;
		const UINT64 remaining = timeoutMs - elapsed;
		UINT32 wait = remaining > 10 ? 10 : WINPR_ASSERTING_INT_CAST(UINT32, remaining);
		const UINT32 nativeTimeout = rdpudp_get_timeout(b->udp);
		if (nativeTimeout < wait)
			wait = nativeTimeout;
		if (!rdpudp_bench_wait(b, wait))
			return -1;
	} while (TRUE);
}

FREERDP_API int freerdp_bench_v1_stats(void* handle, UINT64* out, size_t count)
{
	FreeRdpBenchV1* b = handle;
	if (!b || !b->udp || !out || (count < 8) || !b->state.countersEnabled)
		return 0;
	out[0] = b->state.sentDatagrams;
	out[1] = b->state.receivedDatagrams;
	out[2] = b->state.retransmissions;
	out[3] = UINT64_MAX; /* upstream exposes no loss-report event */
	out[4] = b->udp->inFlight;
	out[5] = Stream_Length(b->received) - b->readPosition;
	out[6] = b->udp->state == RDPUDP_STATE_FAILED; /* one native FAILED state */
	out[7] = UINT64_MAX;                           /* no independent native receive-failure state */
	return 1;
}

FREERDP_API UINT64 freerdp_bench_v1_clock_ns(void)
{
	return winpr_GetTickCount64NS();
}

FREERDP_API int freerdp_bench_v1_deadline(void* handle, UINT64 deadline)
{
	FreeRdpBenchV1* b = handle;
	if (!b || !b->udp || !deadline)
		return 0;
	b->state.deadline = deadline;
	b->state.deadlineReached = FALSE;
	return 1;
}

FREERDP_API int freerdp_bench_v1_deadline_reached(void* handle)
{
	FreeRdpBenchV1* b = handle;
	return b && b->state.deadlineReached;
}

FREERDP_API int freerdp_bench_v1_queue_measure(void* handle, UINT64 start, UINT64 stop)
{
	FreeRdpBenchV1* b = handle;
	return b && rdpudp_bench_measure(b->udp, start, stop);
}

FREERDP_API int freerdp_bench_v1_queue_stats(void* handle, UINT64* out, size_t count)
{
	FreeRdpBenchV1* b = handle;
	return b && rdpudp_bench_queue_stats(b->udp, out, count);
}

FREERDP_API int freerdp_bench_v1_queue_hist(void* handle, UINT32 kind, UINT64* out, size_t count)
{
	FreeRdpBenchV1* b = handle;
	if (!b || !b->state.observer || !out || (kind >= 3) || (count < RDPUDP_BENCH_HIST_BINS))
		return 0;
	memcpy(out, b->state.observer->hist[kind], sizeof(b->state.observer->hist[kind]));
	return 1;
}

FREERDP_API int freerdp_bench_v1_flight_config(void* handle, UINT64* out, size_t count)
{
	FreeRdpBenchV1* b = handle;
	return b && rdpudp_bench_flight_stats(b->udp, out, count);
}

FREERDP_API void freerdp_bench_v1_close(void* handle)
{
	FreeRdpBenchV1* b = handle;
	if (!b)
		return;
	rdpudp_free(b->udp);
	Stream_Free(b->received, TRUE);
	free(b->state.observer);
	free(b);
}
#endif
