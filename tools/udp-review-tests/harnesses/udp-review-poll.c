#include <stdio.h>
#include <string.h>
#include <winpr/assert.h>
#include "libfreerdp/core/rdpeudp.h"
struct rdp_udp_transport { BOOL tunnelEstablished; wStream* tunnelBuf; CRITICAL_SECTION lock; wStream* recvStream; size_t recvPos; int sockfd; };
static int reads=0;
static UINT64 udp_now_ms(void) { return 100; }
static BOOL udp_aborted(const rdpUdpTransport* u) { return FALSE; }
static BOOL freerdp_udp_wait_readable(int fd,DWORD t) { return TRUE; }
static SSIZE_T tls_recv_some(rdpUdpTransport* u,BYTE* b,size_t n) { const BYTE p[]={2,1,0,4,65}; reads++; memcpy(b,p,5); return 5; }
int rdpeudp_tunnel_recv_full(rdpUdpTransport* udp, BYTE* subBuf, size_t subBufLen,
                             size_t* subLenOut, BYTE* payloadBuf, size_t payloadBufLen,
                             size_t* payloadLenOut, DWORD timeoutMs)
{
	BYTE tmp[16384] = { 0 };
	UINT64 deadline = 0;
	if (!udp || !udp->tunnelEstablished)
		return -1;
	deadline = udp_now_ms() + timeoutMs;

	while (TRUE)
	{
		BYTE action = 0;
		UINT16 plen = 0;
		UINT8 hlen = 0;
		size_t total = 0;
		size_t subLen = 0;
		size_t buffered = 0;
		BYTE* base = nullptr;

		/* 1. Check for a complete PDU in the reassembly buffer (pure
		 * framing via rdpemt_tunnel_consume; same helper covered by unit
		 * tests with split fragments). */
		EnterCriticalSection(&udp->lock);
		if (!udp->tunnelBuf)
		{
			LeaveCriticalSection(&udp->lock);
			return -1;
		}
		buffered = Stream_Length(udp->tunnelBuf);
		base = Stream_Buffer(udp->tunnelBuf);
		{
			size_t consumed = 0;
			size_t gotSub = 0;
			size_t gotPayload = 0;
			const int cr = rdpemt_tunnel_consume(
			    base, buffered, &consumed, subBuf, subBufLen, &gotSub, payloadBuf,
			    payloadBufLen, &gotPayload);
			if (cr == 1)
			{
				const size_t remain = buffered - consumed;
				if (remain > 0)
					memmove(base, base + consumed, remain);
				(void)Stream_SetPosition(udp->tunnelBuf, 0);
				(void)Stream_SetLength(udp->tunnelBuf, remain);
				(void)Stream_SetPosition(udp->tunnelBuf, 0);
				LeaveCriticalSection(&udp->lock);
				if (subLenOut)
					*subLenOut = gotSub;
				if (payloadLenOut)
					*payloadLenOut = gotPayload;
				return 1;
			}
			if (cr == -1)
			{
				/* Corrupt stream; resync by dropping buffered bytes. */
				(void)Stream_SetPosition(udp->tunnelBuf, 0);
				(void)Stream_SetLength(udp->tunnelBuf, 0);
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			if (cr == -2)
			{
				/* Complete but caller buffers too small; preserve bytes. */
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			/* cr == 0: incomplete, fall through to read more below. */
		}
		/* Incomplete: capture buffered count for fast-path check below. */
		buffered = Stream_Length(udp->tunnelBuf);
		(void)action;
		(void)plen;
		(void)hlen;
		(void)total;
		(void)subLen;
		LeaveCriticalSection(&udp->lock);

		/* 2. Timeout? Preserve progress and return "no PDU yet".
		 * NOTE: timeoutMs==0 means "one non-blocking attempt", not "already
		 * expired": fall through to the readiness/read attempt below and only
		 * return 0 after it (see step 4). Checking expiry first would return
		 * before ever reading, starving the event loop (S1). */
		if ((timeoutMs != 0) && (udp_now_ms() >= deadline))
			return 0;
		if (udp_aborted(udp))
			return -1;

		/* 3. Fast path for non-blocking drain: if nothing is buffered in TLS
		 * and the socket has nothing readable, avoid the BIO's internal wait
		 * and return immediately (progress preserved in tunnelBuf). */
		if (timeoutMs == 0)
		{
			BOOL tlsBuffered = FALSE;
			BOOL sockReadable = FALSE;
			int fd = -1;
			EnterCriticalSection(&udp->lock);
			if (udp->recvStream)
				tlsBuffered = (Stream_Length(udp->recvStream) > udp->recvPos);
			fd = udp->sockfd;
			LeaveCriticalSection(&udp->lock);
			if (!tlsBuffered && (buffered == 0))
			{
				if (fd >= 0)
					sockReadable = freerdp_udp_wait_readable(fd, 0);
				if (!sockReadable)
					return 0;
			}
			else if (!tlsBuffered && (buffered > 0))
			{
				/* Partial PDU buffered but TLS has nothing new yet; still try
				 * one read in case a record is mid-reassembly, else return. */
				if (fd >= 0)
					sockReadable = freerdp_udp_wait_readable(fd, 0);
				if (!sockReadable)
					return 0;
			}
		}

		/* 4. Read more TLS bytes (single attempt) and append to reassembly. */
		{
			UINT64 now = udp_now_ms();
			DWORD chunkWait = 0;
			if (deadline > now)
			{
				UINT64 left = deadline - now;
				chunkWait = (left > 50) ? 50 : (DWORD)left;
				WINPR_UNUSED(chunkWait);
			}
			/* tls_recv_some does one BIO_read (internal ~100ms max when data
			 * is pending; returns 0 quickly-ish otherwise). */
			const SSIZE_T got = tls_recv_some(udp, tmp, sizeof(tmp));
			if (got < 0)
				return -1;
			if (got == 0)
			{
				if (udp_now_ms() >= deadline)
					return 0;
				/* No progress; poll briefly before retrying (unless non-blocking
				 * drain, where one attempt suffices). */
				if (timeoutMs == 0)
					return 0;
				continue;
			}
			EnterCriticalSection(&udp->lock);
			if (!udp->tunnelBuf)
			{
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			if (Stream_Length(udp->tunnelBuf) + (size_t)got > (1u << 20))
			{
				LeaveCriticalSection(&udp->lock);
				return -1; /* runaway peer; avoid OOM */
			}
			if (!Stream_SetPosition(udp->tunnelBuf, Stream_Length(udp->tunnelBuf)) ||
			    !Stream_EnsureRemainingCapacity(udp->tunnelBuf, (size_t)got))
			{
				LeaveCriticalSection(&udp->lock);
				return -1;
			}
			Stream_Write(udp->tunnelBuf, tmp, (size_t)got);
			Stream_SealLength(udp->tunnelBuf);
			(void)Stream_SetPosition(udp->tunnelBuf, 0);
			LeaveCriticalSection(&udp->lock);
		}
	}
}

int main(void) { rdpUdpTransport u={0}; BYTE sub[256],payload[256];size_t sl=0,pl=0; u.tunnelEstablished=TRUE;u.sockfd=1; InitializeCriticalSection(&u.lock);u.tunnelBuf=Stream_New(NULL,256);Stream_SetLength(u.tunnelBuf,0);
int rc=rdpeudp_tunnel_recv_full(&u,sub,sizeof(sub),&sl,payload,sizeof(payload),&pl,0);
printf("zero-timeout result=%d TLS_reads=%d payload_length=%zu\n",rc,reads,pl); Stream_Release(u.tunnelBuf); DeleteCriticalSection(&u.lock);return 0; }
