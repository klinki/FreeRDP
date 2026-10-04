/** Internal transport accounting. Licensed under Apache-2.0. */
#ifndef FREERDP_CORE_PERFORMANCE_H
#define FREERDP_CORE_PERFORMANCE_H
#include <freerdp/performance.h>
#include <freerdp/metrics.h>
FREERDP_LOCAL BOOL performance_init(rdpMetrics* metrics);
FREERDP_LOCAL void performance_free(rdpMetrics* metrics);
FREERDP_LOCAL void performance_account(rdpContext* context, BOOL udp, BOOL outbound, size_t bytes);
FREERDP_LOCAL void performance_tcp_socket(rdpContext* context, int fd);
FREERDP_LOCAL void performance_udp_connected(rdpContext* context, BOOL connected);
FREERDP_LOCAL void performance_rtt_at(rdpContext* context, BOOL udp, UINT32 milliseconds,
	                                  UINT64 timestamp);
FREERDP_LOCAL void performance_rtt(rdpContext* context, BOOL udp, UINT32 milliseconds);
FREERDP_LOCAL void performance_bandwidth_at(rdpContext* context, BOOL udp, double kbps,
	                                        UINT32 source, UINT64 timestamp);
FREERDP_LOCAL void performance_bandwidth(rdpContext* context, BOOL udp, UINT32 kbps);
FREERDP_LOCAL void performance_bandwidth_measurement(rdpContext* context, BOOL udp,
	                                                UINT32 milliseconds, UINT32 bytes);
typedef struct
{
    UINT64 startedNs;
    UINT64 epoch;
} performanceDecodeToken;
FREERDP_LOCAL performanceDecodeToken performance_decode_begin(rdpContext* context);
FREERDP_LOCAL void performance_decode_end(rdpContext* context, performanceDecodeToken token,
                                         BOOL success, const char* codec);
/* Also used by deterministic duration tests. Producers never wait for the snapshot lock. */
FREERDP_LOCAL void performance_decode_record(rdpContext* context, UINT64 epoch, UINT64 ns,
                                            BOOL success, const char* codec);
FREERDP_LOCAL void performance_udp_stats(rdpContext* context, UINT64 sent, UINT64 received,
                                        UINT64 retransmissions, UINT64 lossReports);
#endif
