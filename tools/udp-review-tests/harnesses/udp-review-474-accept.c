/* Preserved implementor N2 check, extended during review of 47459d93e: peek-only accept wait loop
 * against real localhost UDP sockets. Case 1: v2 DATA queued BEFORE the wait
 * starts -> must complete OK with datagram still queued. Case 2: v1 final
 * ACK queued -> must complete OK and consume it. Build with the commands in
 * the accompanying shell transcript; run, expect exit 0. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <winpr/stream.h>
#include <freerdp/api.h>
#include <libfreerdp/core/rdpeudp.h>

#define TAG "n2check"

/* ---- exact replica of the fixed wait-loop classification (no consume) ---- */
static BOOL wait_classify(int sockfd, UINT32 wantAck, UINT64 deadlineMs);

/* minimal ms clock */
#include <time.h>
static UINT64 now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (UINT64)ts.tv_sec * 1000 + (UINT64)ts.tv_nsec / 1000000;
}

static BOOL wait_readable(int fd, DWORD ms)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0;
}

static BOOL wait_classify(int sockfd, UINT32 wantAck, UINT64 deadlineMs)
{
	BOOL ok = FALSE;
	while (now_ms() < deadlineMs)
	{
		if (wait_readable(sockfd, 100))
		{
			BYTE peek[16] = { 0 };
			const SSIZE_T pr = recv(sockfd, (char*)peek, sizeof(peek), MSG_PEEK);
			if (pr > 8)
			{
				RdpUdpFecHeader h = { 0 };
				wStream sb = { 0 };
				wStream* s = Stream_StaticConstInit(&sb, peek, (size_t)pr);
				if (s && rdpeudp_read_fec_header(s, &h) &&
				    (h.flags & RDPUDP_FLAG_ACK) && !(h.flags & RDPUDP_FLAG_SYN) &&
				    (h.snSourceAck == wantAck))
				{
					BYTE drop[65535] = { 0 };
					(void)recv(sockfd, (char*)drop, sizeof(drop), 0);
					ok = TRUE;
					break;
				}
					{
						const size_t plen =
						    ((size_t)pr < sizeof(peek)) ? (size_t)pr : sizeof(peek);
						BOOL dummy = FALSE;
						size_t poff = 0;
						RdpUdp2Layout pl = { 0 };
						/* wire bytes straight in: unprotect undoes the 0/7
						 * swap itself; pre-swapping double-swaps. */
					if (rdpeudp2_unprotect(peek, plen, &dummy, &poff) && !dummy &&
					    rdpeudp2_parse_layout(peek + poff, plen - poff, &pl) &&
					    pl.hasDataHeader)
					{
						ok = TRUE;
						break;
					}
				}
			}
		}
	}
	return ok;
}

static int make_bound(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in a = { 0 };
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

static int peer_port(int fd)
{
	struct sockaddr_in a = { 0 };
	socklen_t n = sizeof(a);
	if (getsockname(fd, (struct sockaddr*)&a, &n) != 0)
		return -1;
	return ntohs(a.sin_port);
}

static int send_to(int fd, int port, const BYTE* b, size_t n)
{
	struct sockaddr_in a = { 0 };
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((UINT16)port);
	return (int)sendto(fd, (const char*)b, n, 0, (struct sockaddr*)&a, sizeof(a));
}

int main(void)
{
	int srv = -1;
	int cli = -1;
	int rc = 1;
	srv = make_bound();
	cli = make_bound();
	if ((srv < 0) || (cli < 0))
	{
		(void)fprintf(stderr, "socket setup failed\n");
		goto out;
	}
	const int sport = peer_port(srv);
	if (sport < 0)
	{
		(void)fprintf(stderr, "getsockname failed\n");
		goto out;
	}

	/* Case 1: v2 DATA (minimal DATA-only, dseq=9, cseq=4, "hi") queued first. */
	{
		RdpUdp2Layout in = { 0 };
		static const BYTE body[] = { 'h', 'i' };
		in.flags = RDPUDP2_FLAG_DATA;
		in.logWindow = 5;
		in.hasDataHeader = TRUE;
		in.dataSeq = 9;
		in.hasDataBody = TRUE;
		in.channelSeq = 4;
		in.dataBody = body;
		in.dataBodyLen = sizeof(body);
		wStream* s = rdpeudp2_encode_layout(&in);
		if (!s)
		{
			(void)fprintf(stderr, "case1 encode failed\n");
			goto out;
		}
		if (!Stream_SetPosition(s, Stream_Length(s)) || !rdpeudp2_protect(s, FALSE))
		{
			(void)fprintf(stderr, "case1 protect failed\n");
			Stream_Release(s);
			goto out;
		}
		const size_t wl = Stream_Length(s);
		if (send_to(cli, sport, Stream_Buffer(s), wl) != (int)wl)
		{
			(void)fprintf(stderr, "case1 presend failed\n");
			Stream_Release(s);
			goto out;
		}
		Stream_Release(s);
		if (!wait_classify(srv, 0xA11CE000, now_ms() + 2000))
		{
			(void)fprintf(stderr, "case1: queued DATA did not complete\n");
			goto out;
		}
		/* Datagram must still be queued and intact. */
		BYTE back[65535] = { 0 };
		const SSIZE_T got =
		    recv(srv, (char*)back, sizeof(back), MSG_DONTWAIT | MSG_PEEK);
		if (got != (SSIZE_T)wl)
		{
			(void)fprintf(stderr, "case1: queued DATA missing/consumed (%d vs %zu)\n",
			              (int)got, wl);
			goto out;
		}
		/* Drain it so case 2 starts clean. */
		(void)recv(srv, (char*)back, sizeof(back), 0);
		(void)printf("case1 ok: queued DATA completes, left queued\n");
	}

	/* Queue an dummy datagram before a valid ACK. The revised loop
         * must discard it to reach the ACK, but currently peeks it forever. */
        { RdpUdp2Layout in={0}; static const BYTE body[]={1,2};
          in.flags=RDPUDP2_FLAG_DATA;in.logWindow=5;in.hasDataHeader=TRUE;
          in.dataSeq=1;in.hasDataBody=TRUE;in.channelSeq=1;
          in.dataBody=body;in.dataBodyLen=sizeof(body);
          wStream* pkt=rdpeudp2_encode_layout(&in); if(!pkt)goto out;
          if(!Stream_SetPosition(pkt,Stream_Length(pkt))||!rdpeudp2_protect(pkt,TRUE))goto out;
          if(send_to(cli,sport,Stream_Buffer(pkt),Stream_Length(pkt))!=(int)Stream_Length(pkt))goto out;
          Stream_Release(pkt); }
        /* Case 2: v1 final ACK (snSourceAck match) queued -> completes + consumed. */
	{
		BYTE ack[12] = { 0 };
		/* snSourceAck BE, window BE, flags BE(ACK), ackvec-size 0 + pad. */
		ack[0] = 0x12;
		ack[1] = 0x34;
		ack[2] = 0x56;
		ack[3] = 0x78;
		ack[4] = 0x00;
		ack[5] = 0x80;
		ack[6] = 0x00;
		ack[7] = 0x04;
		if (send_to(cli, sport, ack, sizeof(ack)) != (int)sizeof(ack))
		{
			(void)fprintf(stderr, "case2 presend failed\n");
			goto out;
		}
		if (!wait_classify(srv, 0x12345678, now_ms() + 100))
		{
			(void)printf("REGRESSION: dummy datagram blocks valid queued ACK\n");
            rc=0; goto out;
		}
		BYTE back[64] = { 0 };
		const SSIZE_T got =
		    recv(srv, (char*)back, sizeof(back), MSG_DONTWAIT | MSG_PEEK);
		if (got > 0)
		{
			(void)fprintf(stderr, "case2: ACK not consumed (%d left)\n", (int)got);
			goto out;
		}
		(void)printf("case2 ok: queued ACK completes and is consumed\n");
	}

	rc = 0;
	(void)printf("N2-CHECK PASSED\n");
out:
	if (srv >= 0)
		close(srv);
	if (cli >= 0)
		close(cli);
	return rc;
}
