/* Replay the common pre-wrap capture prefix to compare receiver CPU cost.
 * Packet loading and TLS processing are outside the measurement. */
#include REVIEW_RDPEUDP_SOURCE

#include <stdio.h>
#include <time.h>

typedef struct
{
	BYTE* data;
	size_t size;
	UINT64 timeMs;
} ReviewPacket;

typedef struct
{
	ReviewPacket* packets;
	size_t count;
} ReviewFlow;

static double now(clockid_t clock)
{
	struct timespec value = { 0 };
	if (clock_gettime(clock, &value) != 0)
		abort();
	return value.tv_sec + value.tv_nsec * 1e-9;
}

static ReviewFlow load(const char* name)
{
	ReviewFlow flow = { 0 };
	FILE* file = fopen(name, "rb");
	if (!file)
		abort();
	BYTE header[22]; /* analyze_office_udp.py's little-endian <dIiiH cache */
	BOOL haveLastChannel = FALSE;
	while (fread(header, 1, sizeof(header), file) == sizeof(header))
	{
		UINT32 channel = 0;
		double timestamp = 0;
		memcpy(&timestamp, header, sizeof(timestamp));
		memcpy(&channel, header + 16, sizeof(channel));
		const size_t size = header[20] | ((size_t)header[21] << 8);
		/* The previous receiver stops at wrap; exclude it from both workloads. */
		if (haveLastChannel && channel == 1)
			break;
		haveLastChannel |= channel == UINT16_MAX;
		if (flow.count % 4096 == 0)
		{
			ReviewPacket* resized = realloc(flow.packets, (flow.count + 4096) * sizeof(*resized));
			if (!resized)
				abort();
			flow.packets = resized;
		}
		ReviewPacket* packet = &flow.packets[flow.count++];
		packet->data = malloc(size);
		packet->size = size;
		packet->timeMs = (UINT64)(timestamp * 1000);
		if (!packet->data || fread(packet->data, 1, size, file) != size)
			abort();
	}
	fclose(file);
	if (!flow.count || !haveLastChannel)
		abort();
	return flow;
}

int main(int argc, char** argv)
{
	if (argc < 3)
		return 1;
	const int rounds = atoi(argv[1]);
	if (rounds < 1)
		return 1;
	ReviewFlow* flows = calloc((size_t)argc - 2, sizeof(*flows));
	if (!flows)
		return 1;
	size_t packetCount = 0;
	for (int i = 2; i < argc; i++)
	{
		flows[i - 2] = load(argv[i]);
		packetCount += flows[i - 2].count;
	}
	double wall = 0, cpu = 0;
	size_t referenceBytes = 0;
	for (int round = 0; round <= rounds; round++)
	{
		const double wallStart = now(CLOCK_MONOTONIC);
		const double cpuStart = now(CLOCK_PROCESS_CPUTIME_ID);
		size_t bytes = 0;
		for (int i = 2; i < argc; i++)
		{
			const ReviewFlow* flow = &flows[i - 2];
			rdpUdpTransport* udp = rdpeudp_test_new();
			if (!udp)
				abort();
			udp->testClockEnabled = TRUE;
			for (size_t j = 0; j < flow->count; j++)
			{
				const ReviewPacket* packet = &flow->packets[j];
				udp->testClockMs = packet->timeMs;
				if (!rdpeudp_test_feed(udp, packet->data, packet->size))
					abort();
				/* Model a consumer draining delivered bytes, without timing TLS. */
				bytes += Stream_Length(udp->recvStream);
				if (!Stream_SetPosition(udp->recvStream, 0) || !Stream_SetLength(udp->recvStream, 0))
					abort();
				udp->recvPos = 0;
			}
			if (!rdpeudp_test_check_health(udp) || udp->expectedChannelSeq != 0)
				abort();
			rdpeudp_test_free(udp);
		}
		if (round == 0)
			referenceBytes = bytes;
		else
		{
			if (bytes != referenceBytes)
				abort();
			wall += now(CLOCK_MONOTONIC) - wallStart;
			cpu += now(CLOCK_PROCESS_CPUTIME_ID) - cpuStart;
		}
	}
	printf("{\"packets_per_round\":%zu,\"bytes_per_round\":%zu,\"rounds\":%d,"
	       "\"transport_bytes\":%zu,\"wall_ns_per_packet\":%.3f,\"cpu_ns_per_packet\":%.3f}\n",
	       packetCount, referenceBytes, rounds, sizeof(rdpUdpTransport),
	       wall * 1e9 / (packetCount * rounds), cpu * 1e9 / (packetCount * rounds));
	for (int i = 2; i < argc; i++)
	{
		for (size_t j = 0; j < flows[i - 2].count; j++)
			free(flows[i - 2].packets[j].data);
		free(flows[i - 2].packets);
	}
	free(flows);
	return 0;
}

SSIZE_T freerdp_udp_recv(int fd, BYTE* buf, size_t len, DWORD timeout)
{
	return -1;
}
SSIZE_T freerdp_udp_send(int fd, const BYTE* buf, size_t len)
{
	return -1;
}
BOOL freerdp_udp_close(int fd)
{
	return FALSE;
}
void freerdp_tls_free(rdpTls* tls)
{
	if (tls)
		abort();
}
