/* Extracted unchanged receive helper and first-AOA block from 33e410c56.
 * No sockets/TLS; demonstrates cumulative ACK covering a missing DataSeq. */
#include <stdio.h>
#include <string.h>
#include "libfreerdp/core/rdpeudp.h"
typedef struct { UINT16 recvDataBase,lastAckSent; BOOL haveRecvData,haveSeenAoa;
 BOOL recvDataSeen[256]; struct { unsigned recvPackets; } stats; } ReviewUdp;
#define rdpUdpTransport ReviewUdp
static void rdpeudp_note_recv_data_seq_locked(rdpUdpTransport* udp, UINT16 dseq)
{
	/* V1: fixed 1-start base, never rebase from first arrival. Out-of-order
	 * chunks are buffered (seen bitmap); base advances only on contiguous
	 * prefix. haveRealData gates ACK emission; haveSeenAoa feeds the epoch
	 * rule in recv_one. */
	const size_t WIN = ARRAYSIZE(udp->recvDataSeen);
	INT16 diff = (INT16)(dseq - udp->recvDataBase);
	if (!udp->haveRecvData && (diff >= (INT16)WIN))
	{
		/* Initial epoch sync: the very first DATA starts far ahead of the
		 * fixed base (peer counters start at 100+). Sliding would leave a
		 * phantom 1..N gap so every cumulative ACK references ancient
		 * history the peer ignores (observed: base stuck, zero valid ACKs,
		 * peer retransmits forever). Adopt the first far-ahead seq as the
		 * epoch instead. Scoped to pre-first-DATA only, so mid-session
		 * jumps keep slide semantics and in-window reordering still
		 * buffers (V1). */
		udp->recvDataBase = dseq;
		diff = 0;
	}
	if (diff < 0)
		return; /* duplicate/old, still acked via base-1 */
	if ((size_t)diff >= WIN)
	{
		/* Window slide: advance base to dseq-WIN+1, dropping old */
		const UINT16 newBase = (UINT16)(dseq - (WIN - 1));
		const INT16 adv = (INT16)(newBase - udp->recvDataBase);
		if (adv > 0)
		{
			const size_t a = ((size_t)adv < WIN) ? (size_t)adv : WIN;
			memmove(udp->recvDataSeen, udp->recvDataSeen + a, (WIN - a) * sizeof(BOOL));
			memset(udp->recvDataSeen + (WIN - a), 0, a * sizeof(BOOL));
			udp->recvDataBase = newBase;
			diff = (INT16)(dseq - udp->recvDataBase);
		}
		else
			return;
	}
	udp->recvDataSeen[diff] = TRUE;
	udp->haveRecvData = TRUE;
	/* Advance base while contiguous */
	while (udp->recvDataSeen[0])
	{
		memmove(udp->recvDataSeen, udp->recvDataSeen + 1, (WIN - 1) * sizeof(BOOL));
		udp->recvDataSeen[WIN - 1] = FALSE;
		udp->recvDataBase++;
	}
	udp->lastAckSent = dseq;
	udp->stats.recvPackets++;
}


int main(void) {
 ReviewUdp state={0}; ReviewUdp* udp=&state; udp->recvDataBase=1;
 RdpUdp2Layout L={0}; L.hasAoa=TRUE;L.aoa=1;L.dataSeq=2;
			if (!udp->haveSeenAoa && L.hasAoa &&
			    ((INT16)(L.dataSeq - udp->recvDataBase) != 0))
			{
				memset(udp->recvDataSeen, 0, sizeof(udp->recvDataSeen));
				udp->recvDataBase = L.dataSeq;
			}
			udp->haveSeenAoa = udp->haveSeenAoa || L.hasAoa;
			rdpeudp_note_recv_data_seq_locked(udp, L.dataSeq);
 printf("First DATA seq=2 AOA=1: cumulativeACK=%u missingSeq1WasReceived=0\n",(UINT16)(udp->recvDataBase-1));
 return 0;
}
