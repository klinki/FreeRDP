/** Opt-in session transport measurements. Licensed under Apache-2.0. */
#ifndef FREERDP_PERFORMANCE_H
#define FREERDP_PERFORMANCE_H
#include <freerdp/api.h>
#include <freerdp/types.h>
#ifdef __cplusplus
extern "C"
{
#endif
#define FREERDP_PERFORMANCE_VERSION 1
#define FREERDP_PERFORMANCE_TCP 1
#define FREERDP_PERFORMANCE_UDP 2
#define FREERDP_PERFORMANCE_RTT_RDP 1
#define FREERDP_PERFORMANCE_RTT_TCP_ESTIMATE 2
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
	} rdpPerformanceSnapshot;
	FREERDP_API BOOL freerdp_performance_set_enabled(rdpContext* context, BOOL enabled);
	FREERDP_API BOOL freerdp_performance_get_snapshot(rdpContext* context,
	                                                  rdpPerformanceSnapshot* snapshot);
#ifdef __cplusplus
}
#endif
#endif
