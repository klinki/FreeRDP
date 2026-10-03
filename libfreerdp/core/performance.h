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
#endif
