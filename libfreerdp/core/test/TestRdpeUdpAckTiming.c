/** Deterministic wire/clock regressions for RDP-UDP2 receiver ACK timing. */
#include <stdio.h>
#include <string.h>
#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <freerdp/error.h>
#include <freerdp/transport_io.h>
#include "../rdpeudp.h"
#include "../multitransport.h"

#define CHECK(x)                                                    \
	do                                                              \
	{                                                               \
		if (!(x))                                                   \
		{                                                           \
			fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); \
			goto fail;                                              \
		}                                                           \
	} while (0)
typedef struct
{
	BYTE packets[32][2048];
	size_t lengths[32];
	size_t count;
	rdpUdpTransport* reentrant;
	BOOL reentered;
} Capture;

static BOOL feed(rdpUdpTransport* udp, UINT16 seq, UINT16 channel, BOOL data, BOOL dummy, BOOL hint,
                 BYTE max, UINT16 timeout, BOOL aoa, UINT16 aoaSeq)
{
	static const BYTE body[] = { 'x' };
	RdpUdp2Layout layout = { 0 };
	layout.logWindow = 5;
	layout.flags = (UINT16)((data ? RDPUDP2_FLAG_DATA : 0) | (hint ? RDPUDP2_FLAG_DELAYACK : 0) |
	                        (aoa ? RDPUDP2_FLAG_AOA : 0));
	layout.hasDataHeader = layout.hasDataBody = data;
	layout.dataSeq = seq;
	layout.channelSeq = channel;
	layout.dataBody = body;
	layout.dataBodyLen = sizeof(body);
	layout.hasDelayAck = hint;
	layout.delayMax = max;
	layout.delayTimeout = timeout;
	layout.hasAoa = aoa;
	layout.aoa = aoaSeq;
	wStream* packet = rdpeudp2_encode_layout(&layout);
	if (!packet)
		return FALSE;
	BOOL ok = Stream_SetPosition(packet, Stream_Length(packet)) && rdpeudp2_protect(packet, dummy);
	if (ok)
		ok = rdpeudp_test_feed(udp, Stream_Buffer(packet), Stream_Length(packet));
	Stream_Release(packet);
	return ok;
}

static SSIZE_T capture(void* context, const BYTE* data, size_t len)
{
	Capture* out = context;
	if (out->count >= ARRAYSIZE(out->packets) || len > sizeof(out->packets[0]))
		return -1;
	memcpy(out->packets[out->count], data, len);
	out->lengths[out->count++] = len;
	/* A receive callback after encoding must create a new pending ACK, and must
	 * never mutate the packet being emitted or get cleared by its commit. */
	if (out->reentrant && !out->reentered)
	{
		out->reentered = TRUE;
		rdpeudp_test_set_time_ns(out->reentrant, 105000000);
		if (!feed(out->reentrant, 4, 4, TRUE, FALSE, FALSE, 0, 0, FALSE, 0))
			return -1;
	}
	return (SSIZE_T)len;
}

static BOOL decode(const Capture* out, size_t index, BYTE* storage, RdpUdp2Layout* layout)
{
	if (index >= out->count)
		return FALSE;
	memcpy(storage, out->packets[index], out->lengths[index]);
	size_t offset = 0;
	BOOL dummy = TRUE;
	if (!rdpeudp2_unprotect(storage, out->lengths[index], &dummy, &offset) || dummy)
		return FALSE;
	const BYTE shortLen = storage[0] >> 5;
	const size_t len = shortLen < 7 ? shortLen : out->lengths[index] - offset;
	return rdpeudp2_parse_layout(storage + offset, len, layout);
}

static UINT32 timestamp(const BYTE* bytes)
{
	return bytes[0] | ((UINT32)bytes[1] << 8) | ((UINT32)bytes[2] << 16);
}

/* Keep only the latest actual wire packet for long receive sequences. */
static SSIZE_T capture_last(void* context, const BYTE* data, size_t len)
{
	Capture* out = context;
	if (len > sizeof(out->packets[0]))
		return -1;
	memcpy(out->packets[0], data, len);
	out->lengths[0] = len;
	out->count = 1;
	return (SSIZE_T)len;
}

static int test_wide_vector_prefix_timestamps(void)
{
	rdpUdpTransport* udp = rdpeudp_test_new();
	Capture out = { 0 };
	BYTE storage[2048];
	RdpUdp2Layout ack = { 0 };
	RdpUdpTestRecvState state = { 0 };
	BOOL* received = nullptr;
	UINT16 base = 0;
	size_t count = 0;
	CHECK(udp);
	rdpeudp_test_set_send(udp, capture_last, &out);
	rdpeudp_test_set_time_ns(udp, 100000000);
	CHECK(feed(udp, 0, 0, FALSE, FALSE, FALSE, 0, 0, TRUE, 1));
	/* Alternating receipt is deliberately hostile to RLE: 1600 positions
	 * require more than the wire's 127 coded bytes. The first 889 positions
	 * are truthful; later receipt must not move the base or its timestamp. */
	for (UINT16 seq = 2; seq <= 1600; seq += 2)
	{
		rdpeudp_test_set_time_ns(udp, 100000000ULL + (UINT64)seq * 4000ULL);
		CHECK(feed(udp, seq, seq, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	}
	CHECK(rdpeudp_test_recv_state(udp, &state) && state.recvDataBase == 1 &&
	      state.recvDataWindowLimit == 32768 && state.recvDataCount == 800 &&
	      state.recvDataSpan == 1600);
	CHECK(decode(&out, 0, storage, &ack) && ack.hasAckvec && !ack.hasAck);
	CHECK((ack.ackvec[2] & 0x7F) == 127 && (ack.ackvec[2] & 0x80));
	CHECK(ack.ackvecLen == 134 && timestamp(ack.ackvec + 3) == 25888);
	CHECK(rdpeudp_parse_ackvec(ack.ackvec, ack.ackvecLen, &base, &received, &count));
	CHECK(base == 1 && count == 889);
	for (size_t i = 0; i < count; i++)
		CHECK(received[i] == ((i % 2) != 0));
	free(received);
	received = nullptr;
	/* The late original fills the receipt holes without peer retirement. */
	for (UINT16 seq = 1; seq < 1600; seq += 2)
	{
		rdpeudp_test_set_time_ns(udp, 120000000ULL + (UINT64)seq * 4000ULL);
		CHECK(feed(udp, seq, seq, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	}
	CHECK(rdpeudp_test_recv_state(udp, &state) && state.recvDataBase == 1601 &&
	      !state.recvDataCount && !state.recvDataSpan && state.expectedChannelSeq == 1601);
	CHECK(decode(&out, 0, storage, &ack) && ack.hasAck && ack.ackBase == 1600 &&
	      timestamp(ack.ackTs) == 26600);
	rdpeudp_test_free(udp);
	udp = rdpeudp_test_new();
	CHECK(udp);
	ZeroMemory(&out, sizeof(out));
	rdpeudp_test_set_send(udp, capture_last, &out);
	rdpeudp_test_set_time_ns(udp, 200000000);
	CHECK(feed(udp, 0, 0, FALSE, FALSE, FALSE, 0, 0, TRUE, 1));
	CHECK(feed(udp, 9000, 2, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(decode(&out, 0, storage, &ack) && ack.hasAckvec && !ack.hasAck);
	/* A truncated all-missing prefix has no receipt to timestamp. The far
	 * positive remains retained, but it is not represented by this packet. */
	CHECK(ack.ackvec[2] == 127 && ack.ackvecLen == 130);
	CHECK(rdpeudp_parse_ackvec(ack.ackvec, ack.ackvecLen, &base, &received, &count));
	CHECK(base == 1 && count == 8001);
	for (size_t i = 0; i < count; i++)
		CHECK(!received[i]);
	CHECK(rdpeudp_test_recv_state(udp, &state) && state.recvDataBase == 1 &&
	      state.recvDataCount == 1 && state.recvDataSpan == 9000);
	free(received);
	rdpeudp_test_free(udp);
	return 0;
fail:
	free(received);
	rdpeudp_test_free(udp);
	return -1;
}

static int test_receipt_ring_tagged_arrival(void)
{
	rdpUdpTransport* udp = rdpeudp_test_new();
	Capture out = { 0 };
	BYTE storage[2048];
	RdpUdp2Layout ack = { 0 };
	RdpUdpTestRecvState state = { 0 };
	CHECK(udp);
	/* With no output hook, the actual ACK stays pending while AoA retires
	 * the missing prefix. Sequence 0 shares the slot of valid 32768, but is
	 * old in this epoch and cannot replace its original arrival timestamp. */
	rdpeudp_test_set_time_ns(udp, 100000000);
	CHECK(feed(udp, 0, 0, FALSE, FALSE, FALSE, 0, 0, TRUE, 1));
	CHECK(feed(udp, 32768, 2, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	rdpeudp_test_set_time_ns(udp, 110000000);
	CHECK(feed(udp, 0, 1, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(rdpeudp_test_recv_state(udp, &state) && state.recvDataBase == 1 &&
	      state.recvDataCount == 1 && rdpeudp_test_seen(udp, 32767));
	CHECK(feed(udp, 0, 0, FALSE, FALSE, FALSE, 0, 0, TRUE, 32768));
	CHECK(rdpeudp_test_recv_state(udp, &state) && state.recvDataBase == 32769 &&
	      !state.recvDataCount && state.expectedChannelSeq == 3);
	rdpeudp_test_set_send(udp, capture, &out);
	CHECK(rdpeudp_test_check_health(udp) && out.count == 1 && decode(&out, 0, storage, &ack));
	CHECK(ack.hasAck && !ack.hasAckvec && ack.ackBase == 32768 && timestamp(ack.ackTs) == 25000);
	rdpeudp_test_free(udp);
	return 0;
fail:
	rdpeudp_test_free(udp);
	return -1;
}

static int test_deadline_and_batch(void)
{
	rdpUdpTransport* udp = rdpeudp_test_new();
	Capture out = { 0 };
	BYTE storage[2048];
	RdpUdp2Layout ack = { 0 };
	CHECK(udp);
	rdpeudp_test_set_send(udp, capture, &out);
	rdpeudp_test_set_time_ns(udp, 100004000);
	CHECK(feed(udp, 1, 1, TRUE, FALSE, TRUE, 2, 20, FALSE, 0));
	CHECK(out.count == 0 && rdpeudp_test_maintenance_interval(udp) == 20000000);
	rdpeudp_test_set_time_ns(udp, 102012000);
	CHECK(feed(udp, 2, 2, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(out.count == 0);
	rdpeudp_test_set_time_ns(udp, 103016000);
	CHECK(feed(udp, 3, 3, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(out.count == 1 && decode(&out, 0, storage, &ack));
	CHECK(ack.hasAck && ack.ackBase == 3 && timestamp(ack.ackTs) == 25754);
	CHECK(ack.ackGap == 0 && ack.ackNumDelayed == 2 && ack.ackScale == 3);
	CHECK(ack.ackDelayed[0] == 125 && ack.ackDelayed[1] == 251);
	CHECK(rdpeudp_test_maintenance_interval(udp) == 1000000000);
	rdpeudp_test_set_time_ns(udp, 110000000);
	CHECK(feed(udp, 4, 4, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	rdpeudp_test_set_time_ns(udp, 129999999);
	CHECK(rdpeudp_test_check_health(udp) && out.count == 1);
	CHECK(rdpeudp_test_maintenance_interval(udp) == 1);
	rdpeudp_test_set_time_ns(udp, 130000000);
	CHECK(rdpeudp_test_check_health(udp) && out.count == 2 && decode(&out, 1, storage, &ack));
	CHECK(ack.ackBase == 4 && timestamp(ack.ackTs) == 27500 && ack.ackGap == 20);
	CHECK(ack.ackNumDelayed == 0);
	rdpeudp_test_free(udp);
	return 0;
fail:
	rdpeudp_test_free(udp);
	return -1;
}

static int test_gap_arrivals_and_retirement(void)
{
	rdpUdpTransport* udp = rdpeudp_test_new();
	Capture out = { 0 };
	BYTE storage[2048];
	RdpUdp2Layout ack = { 0 };
	CHECK(udp);
	rdpeudp_test_set_send(udp, capture, &out);
	rdpeudp_test_set_time_ns(udp, 100000000);
	CHECK(feed(udp, 1, 1, TRUE, FALSE, TRUE, 2, 20, FALSE, 0));
	rdpeudp_test_set_time_ns(udp, 101008000);
	CHECK(feed(udp, 3, 3, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(out.count == 1 && decode(&out, 0, storage, &ack));
	CHECK(ack.hasAckvec && (ack.ackvec[2] & 0x80));
	CHECK(timestamp(ack.ackvec + 3) == 25252 && ack.ackvec[6] == 0);
	rdpeudp_test_set_time_ns(udp, 107000000);
	CHECK(feed(udp, 2, 2, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(out.count == 2 && decode(&out, 1, storage, &ack));
	CHECK(ack.hasAck && ack.ackBase == 3 && timestamp(ack.ackTs) == 25252);
	CHECK(ack.ackGap == 5 && ack.ackNumDelayed == 2 && ack.ackDelayed[0] == 0);
	CHECK(ack.ackScale == 5 && ack.ackDelayed[1] == 218);
	/* Retire an unseen sequence: ACK that synthetic boundary as now, never
	 * claim it arrived at the timestamp of the earlier genuine packet. */
	rdpeudp_test_set_time_ns(udp, 110000000);
	CHECK(feed(udp, 6, 4, TRUE, FALSE, FALSE, 0, 0, TRUE, 6));
	rdpeudp_test_set_time_ns(udp, 130000000);
	CHECK(rdpeudp_test_check_health(udp) && out.count == 3 && decode(&out, 2, storage, &ack));
	CHECK(ack.ackBase == 6 && ack.ackNumDelayed == 0);
	CHECK(timestamp(ack.ackTs) == 27500 && ack.ackGap == 20);
	rdpeudp_test_free(udp);
	return 0;
fail:
	rdpeudp_test_free(udp);
	return -1;
}

static int test_limits_dummy_and_snapshot(void)
{
	rdpUdpTransport* udp = rdpeudp_test_new();
	Capture out = { 0 };
	BYTE storage[2048];
	RdpUdp2Layout ack = { 0 };
	CHECK(udp);
	rdpeudp_test_set_send(udp, capture, &out);
	rdpeudp_test_set_time_ns(udp, 100000000);
	CHECK(feed(udp, 1, 1, TRUE, TRUE, TRUE, 0, 0, FALSE, 0) && out.count == 0);
	CHECK(feed(udp, 2, 1, TRUE, FALSE, FALSE, 0, 0, FALSE, 0) && out.count == 1);
	/* Feeding an ACK cannot produce an ACK loop. */
	CHECK(rdpeudp_test_feed(udp, out.packets[0], out.lengths[0]) && out.count == 1);
	CHECK(feed(udp, 0, 0, FALSE, FALSE, TRUE, 255, 1000, FALSE, 0));
	for (UINT16 seq = 3; seq < 18; seq++)
	{
		rdpeudp_test_set_time_ns(udp, 100000000 + seq * 4000);
		CHECK(feed(udp, seq, (UINT16)(seq - 1), TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	}
	CHECK(out.count == 1);
	rdpeudp_test_set_time_ns(udp, 100072000);
	CHECK(feed(udp, 18, 17, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(out.count == 2 && decode(&out, 1, storage, &ack));
	CHECK(ack.ackBase == 18 && ack.ackNumDelayed == 15 && ack.ackScale == 0);
	CHECK(out.lengths[1] <= RDPUDP2_MTU);
	rdpeudp_test_free(udp);
	udp = rdpeudp_test_new();
	CHECK(udp);
	ZeroMemory(&out, sizeof(out));
	rdpeudp_test_set_send(udp, capture, &out);
	CHECK(feed(udp, 0, 0, FALSE, FALSE, TRUE, 2, 20, FALSE, 0));
	for (UINT16 seq = 1; seq <= 2; seq++)
	{
		rdpeudp_test_set_time_ns(udp, 100000000 + seq * 1000000);
		CHECK(feed(udp, seq, seq, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	}
	out.reentrant = udp;
	rdpeudp_test_set_time_ns(udp, 103000000);
	CHECK(feed(udp, 3, 3, TRUE, FALSE, FALSE, 0, 0, FALSE, 0));
	CHECK(out.count == 1 && out.reentered && decode(&out, 0, storage, &ack));
	CHECK(ack.ackBase == 3 && ack.ackNumDelayed == 2);
	CHECK(rdpeudp_test_maintenance_interval(udp) == 20000000);
	rdpeudp_test_set_time_ns(udp, 125000000);
	CHECK(rdpeudp_test_check_health(udp) && out.count == 2 && decode(&out, 1, storage, &ack));
	CHECK(ack.ackBase == 4 && ack.ackGap == 20 && ack.ackNumDelayed == 0);
	rdpeudp_test_free(udp);
	return 0;
fail:
	rdpeudp_test_free(udp);
	return -1;
}

static int test_timestamp_wrap_tighter_hint_and_keepalive(void)
{
	rdpUdpTransport* udp = rdpeudp_test_new();
	Capture out = { 0 };
	BYTE storage[2048];
	RdpUdp2Layout ack = { 0 };
	CHECK(udp);
	rdpeudp_test_set_send(udp, capture, &out);
	const UINT64 arrival = (1ULL << 24) * 4000ULL + 12000;
	rdpeudp_test_set_time_ns(udp, arrival);
	CHECK(feed(udp, 1, 1, TRUE, FALSE, TRUE, 15, 1000, FALSE, 0));
	CHECK(out.count == 0);
	rdpeudp_test_set_time_ns(udp, arrival + 300000000);
	/* The tighter timeout is relative to the first pending packet's arrival,
	 * not to this later hint. The already-aged packet is acknowledged now. */
	CHECK(feed(udp, 0, 0, FALSE, FALSE, TRUE, 15, 20, FALSE, 0));
	CHECK(out.count == 1 && decode(&out, 0, storage, &ack));
	CHECK(ack.hasAck && timestamp(ack.ackTs) == 3 && ack.ackGap == 254);
	CHECK(ack.ackNumDelayed == 0);
	rdpeudp_test_free(udp);
	udp = rdpeudp_test_new();
	CHECK(udp);
	ZeroMemory(&out, sizeof(out));
	rdpeudp_test_set_send(udp, capture, &out);
	rdpeudp_test_set_time(udp, 100);
	rdpeudp_test_set_connected(udp, TRUE);
	CHECK(rdpeudp_test_keepalive(udp) && out.count == 1);
	memcpy(storage, out.packets[0], out.lengths[0]);
	BOOL dummy = FALSE;
	size_t offset = 0;
	CHECK(rdpeudp2_unprotect(storage, out.lengths[0], &dummy, &offset) && dummy);
	const BYTE shortLen = storage[0] >> 5;
	CHECK(rdpeudp2_parse_layout(storage + offset, shortLen < 7 ? shortLen : out.lengths[0] - offset,
	                            &ack));
	CHECK(ack.hasAoa && ack.aoa == 1 && !ack.hasAck && !ack.hasDataHeader);
	RdpUdpTestSendState state = { 0 };
	CHECK(rdpeudp_test_send_state(udp, &state) && state.nextDataSeq == 1);
	rdpeudp_test_free(udp);
	return 0;
fail:
	rdpeudp_test_free(udp);
	return -1;
}

static int idle_tcp(WINPR_ATTR_UNUSED rdpTransport* transport, WINPR_ATTR_UNUSED wStream* stream)
{
	return 0;
}

static int test_sparse_mainloop_timer(void)
{
	int rc = -1;
	freerdp* instance = freerdp_new();
	rdpUdpTransport* udp = nullptr;
	Capture out = { 0 };
	wStream* packet = nullptr;
	if (!instance || !freerdp_context_new(instance))
		goto done;
	rdpContext* context = instance->context;
	rdpTransportIo io = *freerdp_get_io_callbacks(context);
	io.ReadPdu = idle_tcp;
	if (!freerdp_set_io_callbacks(context, &io) || !freerdp_check_fds(instance))
		goto done;
	udp = rdpeudp_test_new();
	if (!udp)
		goto done;
	rdpeudp_test_set_time(udp, 100);
	rdpeudp_test_set_send(udp, capture, &out);
	if (!multitransport_test_attach_udp(context, udp))
		goto done;
	rdpUdpTransport* attached = udp;
	udp = nullptr;
	if (!freerdp_check_fds(instance) || !feed(attached, 0, 0, FALSE, FALSE, TRUE, 2, 20, FALSE, 0))
		goto done;
	/* Empty retained channel data keeps TLS idle, yet still requires receipt. */
	RdpUdp2Layout data = { 0 };
	data.flags = RDPUDP2_FLAG_DATA;
	data.logWindow = 5;
	data.hasDataHeader = data.hasDataBody = TRUE;
	data.dataSeq = data.channelSeq = 1;
	packet = rdpeudp2_encode_layout(&data);
	if (!packet || !Stream_SetPosition(packet, Stream_Length(packet)) ||
	    !rdpeudp2_protect(packet, FALSE) ||
	    !rdpeudp_test_feed(attached, Stream_Buffer(packet), Stream_Length(packet)) ||
	    out.count != 0 || !freerdp_check_fds(instance))
		goto done;
	HANDLE events[64] = { 0 };
	const DWORD count = freerdp_get_event_handles(context, events, ARRAYSIZE(events));
	if (!count)
		goto done;
	for (DWORD i = 0; i < count; i++)
		(void)ResetEvent(events[i]);
	rdpeudp_test_set_time(attached, 120);
	/* The former idle maintenance timer was one second. No socket/data wake
	 * remains; the production ACK deadline must wake this otherwise idle loop. */
	const DWORD wake = WaitForMultipleObjects(count, events, FALSE, 200);
	if (wake < WAIT_OBJECT_0 || wake >= WAIT_OBJECT_0 + count || !freerdp_check_fds(instance) ||
	    out.count != 1)
		goto done;
	rc = 0;
done:
	if (rc)
		fprintf(stderr, "sparse UDP ACK mainloop deadline FAILED\n");
	if (packet)
		Stream_Release(packet);
	rdpeudp_test_free(udp);
	if (instance)
	{
		freerdp_context_free(instance);
		freerdp_free(instance);
	}
	return rc;
}

int TestRdpeUdpAckTiming(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	return test_deadline_and_batch() || test_gap_arrivals_and_retirement() ||
	               test_limits_dummy_and_snapshot() ||
	               test_timestamp_wrap_tighter_hint_and_keepalive() ||
	               test_wide_vector_prefix_timestamps() || test_receipt_ring_tagged_arrival() ||
	               test_sparse_mainloop_timer()
	           ? -1
	           : 0;
}
