/* Private benchmark adapter, enabled ONLY by WITH_RDP_BENCH.
 * Include at the end of libfreerdp/core/rdpeudp.c.
 * Calls the actual production reliable sender/receiver. Does not implement ARQ.
 * Scope: post-negotiation raw reliable UDP2. NO TLS, tunnel, DVC, main RDP, or SYN.
 * Socket I/O and all production reliability code remain in the measured path.
 * Do not ship a production build with this switch enabled.
 */
#if defined(WITH_RDP_BENCH)
#include <freerdp/performance.h>
#include <string.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#endif

typedef struct {
    freerdp* instance;
    rdpUdpTransport* udp;
} FreeRdpBenchV1;

FREERDP_API void freerdp_bench_v1_close(void* handle);
FREERDP_API void* freerdp_bench_v1_open(const char* bindAddress, UINT16 port,
                                      const char* peerAddress, UINT16 peerPort)
{
    FreeRdpBenchV1* b = calloc(1, sizeof(*b));
    struct sockaddr_in local = { 0 }, peer = { 0 };
    const BYTE cookie[RDPEUDP_COOKIE_LEN] = { 0 };
    if (!b || !bindAddress || !peerAddress || !port || !peerPort) { free(b); return NULL; }
    b->instance = freerdp_new();
    if (!b->instance || !freerdp_context_new(b->instance)) goto fail;
    b->udp = rdpeudp_new(b->instance->context, peerAddress, peerPort, 1,
                        INITIATE_REQUEST_PROTOCOL_UDPFECR, cookie);
    if (!b->udp) goto fail;
    b->udp->sockfd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (b->udp->sockfd < 0) goto fail;
    local.sin_family = AF_INET; local.sin_port = htons(port);
    peer.sin_family = AF_INET; peer.sin_port = htons(peerPort);
    if (inet_pton(AF_INET, bindAddress, &local.sin_addr) != 1 ||
        inet_pton(AF_INET, peerAddress, &peer.sin_addr) != 1) goto fail;
    if (bind(b->udp->sockfd, (struct sockaddr*)&local, sizeof(local)) != 0 ||
        connect(b->udp->sockfd, (struct sockaddr*)&peer, sizeof(peer)) != 0 ||
        !freerdp_udp_set_nonblocking(b->udp->sockfd, TRUE)) goto fail;
    {
        int n = 4 * 1024 * 1024;
        (void)setsockopt(b->udp->sockfd, SOL_SOCKET, SO_RCVBUF, (const void*)&n, sizeof(n));
        (void)setsockopt(b->udp->sockfd, SOL_SOCKET, SO_SNDBUF, (const void*)&n, sizeof(n));
    }
    b->udp->connected = TRUE;
    b->udp->useUdp2 = TRUE;
    b->udp->negotiatedVer = RDPUDP_PROTOCOL_VERSION_3;
    b->udp->negotiatedUpMtu = RDPUDP2_MTU;
    b->udp->negotiatedDownMtu = RDPUDP2_MTU;
    b->udp->maxPayload = rdpeudp_payload_for_mtu(RDPUDP2_MTU);
    /* Calibration can measure counter overhead without changing ARQ/socket code. */
    const char* counters = getenv("RDP_BENCH_COUNTERS");
    const BOOL countersEnabled = !counters || (strcmp(counters, "0") != 0);
    (void)freerdp_performance_set_enabled(b->instance->context, countersEnabled);
    performance_udp_connected(b->instance->context, TRUE);
    return b;
fail:
    freerdp_bench_v1_close(b);
    return NULL;
}
FREERDP_API INT64 freerdp_bench_v1_send(void* handle, const BYTE* data, size_t len)
{
    FreeRdpBenchV1* b = handle;
    if (!b || !b->udp || !data || !len || len > 60000) return -1;
    return (INT64)rdpeudp2_send_reliable(b->udp, data, len);
}
FREERDP_API INT64 freerdp_bench_v1_read(void* handle, BYTE* data, size_t cap, UINT32 timeoutMs)
{
    FreeRdpBenchV1* b = handle;
    if (!b || !b->udp || !data || !cap) return -1;
    if (!rdpeudp_check_health(b->udp)) return -1;
    (void)rdpeudp_check_keepalive(b->udp, 5000);
    if (timeoutMs) return (INT64)rdpeudp2_recv_reliable(b->udp, data, cap, timeoutMs);
    /* Single owner. Drive the SAME receive function; no alternate ACK code.
     * Bound the drain to preserve fairness to application sends and timers. */
    for (unsigned i = 0; i < 64 && freerdp_udp_wait_readable(b->udp->sockfd, 0); ++i)
        (void)rdpeudp_recv_one(b->udp, 0, NULL, NULL, NULL, NULL);
    if (!rdpeudp_check_health(b->udp)) return -1;
    if (Stream_Length(b->udp->recvStream) <= b->udp->recvPos) return 0;
    return (INT64)rdpeudp2_recv_reliable(b->udp, data, cap, 1);
}
FREERDP_API int freerdp_bench_v1_stats(void* handle, UINT64* out, size_t count)
{
    FreeRdpBenchV1* b = handle;
    rdpPerformanceSnapshot s = { 0 };
    if (!b || !b->udp || !out || count < 8) return 0;
    s.size = sizeof(s); s.version = FREERDP_PERFORMANCE_VERSION;
    if (!freerdp_performance_get_snapshot(b->instance->context, &s)) return 0;
    if (!s.enabled) return 0; /* Disabled observations are unknown, not zero traffic. */
    out[0] = s.udpSentDatagrams; out[1] = s.udpReceivedDatagrams;
    out[2] = s.udpRetransmissions; out[3] = s.udpLossReports;
    out[4] = b->udp->sentCount;
    out[5] = Stream_Length(b->udp->recvStream) - b->udp->recvPos;
    out[6] = b->udp->sendFailed; out[7] = b->udp->recvFailed;
    return 1;
}
FREERDP_API void freerdp_bench_v1_close(void* handle)
{
    FreeRdpBenchV1* b = handle;
    if (!b) return;
    if (b->udp) rdpeudp_free(b->udp);
    if (b->instance) { freerdp_context_free(b->instance); freerdp_free(b->instance); }
    free(b);
}
#endif
