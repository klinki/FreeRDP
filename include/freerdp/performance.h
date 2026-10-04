/** Opt-in session transport measurements. Licensed under Apache-2.0. */
#ifndef FREERDP_PERFORMANCE_H
#define FREERDP_PERFORMANCE_H
#include <freerdp/api.h>
#include <freerdp/types.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C"
{
#endif
#define FREERDP_PERFORMANCE_VERSION 3
#define FREERDP_PERFORMANCE_TCP 1
#define FREERDP_PERFORMANCE_UDP 2
#define FREERDP_PERFORMANCE_RTT_RDP 1
#define FREERDP_PERFORMANCE_RTT_TCP_ESTIMATE 2
#define FREERDP_PERFORMANCE_BANDWIDTH_RDP_ESTIMATE 1
#define FREERDP_PERFORMANCE_BANDWIDTH_RDP_MEASUREMENT 2
	typedef struct
	{
		UINT32 size;
		UINT32 version;
		UINT64 epoch;
		UINT64 tcpInBytes, tcpOutBytes, udpInBytes, udpOutBytes;
		UINT32 connectedTransports;
		BOOL enabled;
		BOOL rttValid;
		UINT32 rttMilliseconds, rttSource, rttTransport;
		UINT64 rttAgeMilliseconds;
		/* Version 2 extension. Bandwidth is an RDP estimate, not current traffic usage. */
		BOOL bandwidthValid;
		BOOL bandwidthStale;
		double bandwidthKilobitsPerSecond;
		UINT32 bandwidthSource, bandwidthTransport;
		UINT64 bandwidthAgeMilliseconds;
        /* Version 3: successful datagram I/O and observed reliable-UDP events. */
        UINT64 udpSentDatagrams, udpReceivedDatagrams, udpRetransmissions, udpLossReports;
        /* Codec calls include color conversion, not presentation or server encoding. */
        UINT64 decodeCalls, decodeFailures, decodeWallNs, decodeSamplesSkipped;
        double decodeRecentP95Milliseconds;
        UINT32 decodeRecentSampleCount;
        char decodeCodec[32];
	} rdpPerformanceSnapshot;
#define FREERDP_PERFORMANCE_V1_SIZE offsetof(rdpPerformanceSnapshot, bandwidthValid)
#define FREERDP_PERFORMANCE_V2_SIZE offsetof(rdpPerformanceSnapshot, udpSentDatagrams)
	FREERDP_API BOOL freerdp_performance_set_enabled(rdpContext* context, BOOL enabled);
	FREERDP_API BOOL freerdp_performance_get_snapshot(rdpContext* context,
	                                                  rdpPerformanceSnapshot* snapshot);
#ifdef __cplusplus
}
#endif
#endif
