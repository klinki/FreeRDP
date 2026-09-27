
#include "libfreerdp/core/rdpeudp.h"
#undef RDPEUDP_SEND_TIMEOUT_MS
#define RDPEUDP_SEND_TIMEOUT_MS 30
#include "libfreerdp/core/rdpeudp.c"
static rdpUdpTransport* sender;
static rdpUdpTransport* receiver;
static int acknowledgeAll;
static SSIZE_T deliver(void* context, const BYTE* data, size_t len)
{
	if (!rdpeudp_test_feed(receiver, data, len))
		return -1;
	for (size_t i = 0; i < ARRAYSIZE(sender->sent); ++i)
	{
		if (sender->sent[i].data && (acknowledgeAll || sender->sent[i].channelSeq == 1))
		{
			UINT16 seq = sender->sent[i].dataSeq;
			rdpeudp_ack_single_locked(sender, seq);
		}
	}
	return len;
}
int main(void)
{
	sender = rdpeudp_test_new();
	receiver = rdpeudp_test_new();
	sender->connected = TRUE;
	sender->useUdp2 = TRUE;
	sender->maxPayload = 4;
	rdpeudp_test_set_send(sender, deliver, NULL);
	BIO_METHOD* method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "Review");
	if (!method)
		return 1;
	BIO* bio = BIO_new(method);
	if (!bio)
		return 1;
	RdpUdpBio state = { sender, NULL };
	BIO_set_data(bio, &state);
	BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
	SSIZE_T first = rdpeudp_bio_write(bio, "ABCDEFGH", 8);
	if (BIO_should_retry(bio))
		return 1;
	acknowledgeAll = 1;
	SSIZE_T retry = rdpeudp_bio_write(bio, "ABCDEFGH", 8);
	BIO_free(bio);
	BIO_meth_free(method);
	if (first != -1 || retry != -1 || rdpeudp_check_health(sender) ||
	    Stream_Length(receiver->recvStream) != 8 ||
	    memcmp(Stream_Buffer(receiver->recvStream), "ABCDEFGH", 8) != 0)
		return 1;
	printf("first=%zd, retry=%zd, expected=ABCDEFGH, received=%.*s, bytes=%zu\n", first, retry,
	       (int)Stream_Length(receiver->recvStream), Stream_Buffer(receiver->recvStream),
	       Stream_Length(receiver->recvStream));
	rdpeudp_test_free(sender);
	rdpeudp_test_free(receiver);
	sender = rdpeudp_test_new();
	receiver = rdpeudp_test_new();
	sender->connected = TRUE;
	sender->useUdp2 = TRUE;
	sender->maxPayload = 4;
	rdpeudp_test_set_send(sender, deliver, NULL);
	const SSIZE_T success = rdpeudp2_send_reliable(sender, (const BYTE*)"ABCDEFGH", 8);
	if (success != 8 || !rdpeudp_check_health(sender) || Stream_Length(receiver->recvStream) != 8 ||
	    memcmp(Stream_Buffer(receiver->recvStream), "ABCDEFGH", 8) != 0)
		return 1;
	printf("Acknowledged write: received exactly eight bytes, transport healthy\n");
	rdpeudp_test_free(sender);
	rdpeudp_test_free(receiver);
	rdpMultitransport* multi = multitransport_test_new();
	const BYTE emptyList[] = { 0x80, 0, 14, 0, 0, 0, 3, 0, 1, 0, 1, 0, 0, 0, 0, 0 };
	multitransport_test_recv_feed(multi, emptyList, sizeof(emptyList),
	                              CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST);
	multitransport_test_response_sent(multi);
	printf("empty CHANNELLIST: migrated=%d, unrelated dvc42 routed=%d\n",
	       multitransport_test_sendmigrated(multi), multitransport_test_dvc_routed(multi, 42));
	if (multitransport_test_dvc_routed(multi, 42))
		return 1;
	multitransport_test_free(multi);
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
