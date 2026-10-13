/* Private raw-transport measurement state. Nothing is present in a normal build. */
#if defined(WITH_RDP_BENCH)
#define RDPUDP_BENCH_HIST_BINS 16384
#define RDPUDP_BENCH_HIST_NS 125000ULL
#define RDPUDP_BENCH_QUEUE_BYTES 131072
#define RDPUDP_BENCH_RECV_BYTES (64u * 1024u * 1024u)

typedef struct
{
	BOOL valid;
	UINT64 lastClock;
	UINT64 start;
	UINT64 stop;
	UINT64 hist[3][RDPUDP_BENCH_HIST_BINS];
	UINT64 samples[3];
	UINT64 minimum[3];
	UINT64 maximum[3];
	UINT64 total[3];
	UINT64 overflow[3];
	UINT64 peakOwned;
	UINT64 peakQueued;
	UINT64 peakFlightBytes;
	UINT64 peakFlightPackets;
	UINT64 acceptedBytes;
	UINT64 firstSentBytes;
	UINT64 clockErrors;
	UINT64 counterErrors;
} RdpUdpBenchObserver;

typedef struct
{
	RdpUdpBenchObserver* observer;
	size_t queueLimit;
	size_t cap;
	size_t ownedBytes;
	size_t queuedBytes;
	size_t firstPendingCount;
	size_t firstPendingBytes;
	size_t peakFlight;
	size_t peakSpan;
	UINT64 dequeuedAccepted;
	UINT64 sentDatagrams;
	UINT64 receivedDatagrams;
	UINT64 retransmissions;
	UINT64 deadline;
	BYTE peerLog;
	BOOL countersEnabled;
	BOOL lastRawSendComplete;
	BOOL deadlineReached;
} RdpUdpBench;
#endif
