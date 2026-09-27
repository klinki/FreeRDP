#include "libfreerdp/core/test/TestRdpeUdp.c"

static int review_late_zero(BOOL useChunks)
{
    rdpUdpTransport* udp = rdpeudp_test_new();
    RdpUdpTestRecvState st = { 0 };
    static const BYTE a[] = { 'A' }, b[] = { 'B' }, z[] = { 'Z' };
    rdpeudp_test_set_time(udp, WRAP_TEST_T0);
    if (rx_wrap_dirty_runup(udp)) return 1;
    const RxScriptPkt first = { 101, 1, a, 1, FALSE, 0, FALSE, TRUE };
    if (rx_feed(udp, &first)) return 1;
    UINT16 last = useChunks ? 64 : 2;
    if (!useChunks) rdpeudp_test_set_time(udp, WRAP_TEST_T0 + 251);
    for (UINT16 c = 2; c <= last; c++) {
        const RxScriptPkt pkt = { (UINT16)(100 + c), c, b, 1, FALSE, 0, FALSE, TRUE };
        if (rx_feed(udp, &pkt)) return 1;
    }
    const RxScriptPkt zero = { (UINT16)(101 + last), 0, z, 1, FALSE, 0, FALSE, TRUE };
    if (rx_feed(udp, &zero)) return 1;
    if (rx_state(udp, &st)) return 1;
    BYTE out[3] = {0};
    if (!rdpeudp_test_recv_data(udp, 65534, out, sizeof(out))) return 1;
    printf("late zero (%s): expected prefix FZA, actual %.3s; expected stream length %u, actual %zu; healthy=%d\n", useChunks ? "64 chunks, zero elapsed time" : "251ms", out, 65536U + last, st.recvStreamLen, rdpeudp_test_check_health(udp));
    const int rc = memcmp(out, "FZA", 3) != 0 || st.recvStreamLen != 65536U + last ||
                   !rdpeudp_test_check_health(udp);
    rdpeudp_test_free(udp);
    return rc;
}

static int review_quiet_timeout(void)
{
    rdpUdpTransport* udp = rdpeudp_test_new();
    RdpUdpTestRecvState st = { 0 };
    static const BYTE a[] = { 'A' };
    rdpeudp_test_set_time(udp, WRAP_TEST_T0);
    if (rx_wrap_dirty_runup(udp)) return 1;
    const RxScriptPkt first = { 101, 1, a, 1, FALSE, 0, FALSE, TRUE };
    if (rx_feed(udp, &first)) return 1;
    rdpeudp_test_set_time(udp, WRAP_TEST_T0 + 251);
    BOOL healthy = rdpeudp_test_check_health(udp);
    if (rx_state(udp, &st)) return 1;
    printf("quiet gap at 251ms: healthy=%d, expected next=0, actual next=%u; expected length=65535, actual length=%zu\n", healthy, st.expectedChannelSeq, st.recvStreamLen);
    if (!healthy || st.expectedChannelSeq != 0 || st.recvStreamLen != 65535) return 1;
    rdpeudp_test_set_time(udp, WRAP_TEST_T0 + RDPEUDP_REASSEMBLY_TIMEOUT_MS);
    healthy = rdpeudp_test_check_health(udp);
    printf("quiet gap at 10s: expected unhealthy, healthy=%d\n", healthy);
    const RxScriptPkt zero = { 102, 0, a, 1, FALSE, 0, FALSE, TRUE };
    if (rx_feed(udp, &zero) || rx_state(udp, &st)) return 1;
    const int rc = healthy || rdpeudp_test_check_health(udp) ||
                   st.expectedChannelSeq != 0 || st.recvStreamLen != 65535;
    rdpeudp_test_free(udp);
    return rc;
}

int main(void) { return test_rx_wrap_after_recovered_loss() || review_late_zero(FALSE) || review_late_zero(TRUE) || review_quiet_timeout(); }
