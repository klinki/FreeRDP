/* Extracted unchanged receive helper and first-AOA block from 11c4f822b.
 * No sockets/TLS; production 128-slot window; prints state and encoded ACKVEC, not a live ACK. */
#include <stdio.h>
#include <string.h>
#include "libfreerdp/core/rdpeudp.h"
typedef struct { UINT16 recvDataBase,lastAckSent; BOOL haveRecvData,haveSeenAoa;
 BOOL recvDataSeen[64 * 2]; struct { unsigned recvPackets; } stats; } ReviewUdp;
#define rdpUdpTransport ReviewUdp
static void rdpeudp_note_recv_data_seq_locked(rdpUdpTransport* udp, UINT16 dseq)
{
	/* V1: fixed 1-start base, never rebase from first arrival. Out-of-order
	 * chunks are buffered (seen bitmap); base advances only on contiguous
	 * prefix. haveRealData gates ACK emission; haveSeenAoa feeds the epoch
	 * rule in recv_one. */
	const size_t WIN = ARRAYSIZE(udp->recvDataSeen);
	INT16 diff = (INT16)(dseq - udp->recvDataBase);
	if (!udp->haveRecvData && !udp->haveSeenAoa && (diff >= (INT16)WIN))
	{
		/* Initial epoch sync: the very first DATA starts far ahead of the
		 * fixed base (peer counters start at 100+). Sliding would leave a
		 * phantom 1..N gap so every cumulative ACK references ancient
		 * history the peer ignores (observed: base stuck, zero valid ACKs,
		 * peer retransmits forever). Adopt the first far-ahead seq as the
		 * epoch instead. Scoped to pre-first-DATA only, so mid-session
		 * jumps keep slide semantics and in-window reordering still
		 * buffers (V1). Gated on !haveSeenAoa (P2): once an AOA fixed
		 * the epoch, DataSeq alone must never skip unreceived sequences
		 * again — the AOA block above owns that boundary now. */
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
 RdpUdp2Layout L={0}; L.hasAoa=TRUE;L.aoa=1;L.dataSeq=300;
			if (!udp->haveSeenAoa && L.hasAoa)
			{
				const INT16 adv = (INT16)(L.aoa - udp->recvDataBase);
				if (adv > 0)
				{
					const size_t WIN = ARRAYSIZE(udp->recvDataSeen);
					const size_t a = ((size_t)adv < WIN) ? (size_t)adv : WIN;
					memmove(udp->recvDataSeen, udp->recvDataSeen + a,
					        (WIN - a) * sizeof(BOOL));
					memset(udp->recvDataSeen + (WIN - a), 0, a * sizeof(BOOL));
					udp->recvDataBase = (UINT16)(udp->recvDataBase + adv);
				}
				udp->haveSeenAoa = TRUE;
			}
			rdpeudp_note_recv_data_seq_locked(udp, L.dataSeq);
 printf("First DATA seq=300 AOA=1: baseMinusOne=%u missingSeq1WasReceived=0\n",(UINT16)(udp->recvDataBase-1));
 size_t count=sizeof(udp->recvDataSeen)/sizeof(udp->recvDataSeen[0]);
 wStream* vec=rdpeudp_build_ackvec(udp->recvDataBase,udp->recvDataSeen,count,FALSE,0,0);
 if(!vec)return 1;
 UINT16 base=0;BOOL* seen=NULL;size_t decoded=0;
 if(!rdpeudp_parse_ackvec(Stream_Buffer(vec),Stream_Length(vec),&base,&seen,&decoded))return 2;
 size_t last=0;for(size_t i=0;i<decoded;i++)if(seen[i])last=i;
 printf("ACKVEC base=%u count=%zu firstMissing=%u lastReceived=%u; sequences below base are not represented\n",base,decoded,base,(unsigned)(base+last));
 free(seen);Stream_Release(vec);
 return 0;
}
