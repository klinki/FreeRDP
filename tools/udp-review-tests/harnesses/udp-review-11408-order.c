#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winpr/stream.h>
#include <winpr/winsock.h>
#define RDPEUDP2_RECV_MAP 256
typedef struct {BOOL haveRecvData,haveRecvChannel; UINT16 recvDataBase,lastAckSent,expectedChannelSeq; BOOL recvDataSeen[256]; struct {unsigned recvPackets;} stats; struct {BOOL occupied; UINT16 channelSeq; BYTE* data; size_t len;} recvMap[256]; wStream* recvStream; HANDLE sockEvent,udpEvent;} rdpUdpTransport;
static void rdpeudp_note_recv_data_seq_locked(rdpUdpTransport* udp, UINT16 dseq)
{
	/* First DATA defines the base so 0-start (old) and 1-start (MS) peers
	 * both interoperate. */
	if (!udp->haveRecvData)
	{
		memset(udp->recvDataSeen, 0, sizeof(udp->recvDataSeen));
		udp->recvDataBase = dseq;
		udp->haveRecvData = TRUE;
	}
	const size_t WIN = ARRAYSIZE(udp->recvDataSeen);
	INT16 diff = (INT16)(dseq - udp->recvDataBase);
	if (diff < 0)
		return; /* duplicate/old, still acked via lastAckSent */
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

static void feed(rdpUdpTransport* udp, UINT16 ds,UINT16 cs,BYTE val){
 struct {BOOL hasDataHeader,hasDataBody;UINT16 dataSeq,channelSeq;const BYTE* dataBody;size_t dataBodyLen;} L={TRUE,TRUE,ds,cs,&val,1};
		if (L.hasDataHeader && L.hasDataBody)
		{
			const UINT16 dseq = L.dataSeq;
			const UINT16 cseq = L.channelSeq;
			const BYTE* dp = L.dataBody;
			const size_t drem = L.dataBodyLen;

			rdpeudp_note_recv_data_seq_locked(udp, dseq);

			/* First DATA defines channel base (MS starts at 1, old code at 0). */
			if (!udp->haveRecvChannel)
			{
				udp->expectedChannelSeq = cseq;
				udp->haveRecvChannel = TRUE;
			}
			/* Deduplicate by channel seq */
			BOOL dup = FALSE;
			if (((INT16)(cseq - udp->expectedChannelSeq) < 0))
			{
				/* Already delivered (within window) -> dup, still ACK */
				dup = TRUE;
			}
			else
			{
				const size_t slot = (size_t)(cseq % RDPEUDP2_RECV_MAP);
				if (udp->recvMap[slot].occupied &&
				    (udp->recvMap[slot].channelSeq == cseq))
					dup = TRUE;
			}

			if (!dup && (drem <= 65535))
			{
				const size_t slot = (size_t)(cseq % RDPEUDP2_RECV_MAP);
				free(udp->recvMap[slot].data);
				udp->recvMap[slot].data = nullptr;
				udp->recvMap[slot].len = 0;
				udp->recvMap[slot].occupied = FALSE;
				if ((drem == 0) || ((udp->recvMap[slot].data = malloc(drem)) != nullptr))
				{
					if (drem > 0)
						memcpy(udp->recvMap[slot].data, dp, drem);
					udp->recvMap[slot].len = drem;
					udp->recvMap[slot].channelSeq = cseq;
					udp->recvMap[slot].occupied = TRUE;
				}

				/* Deliver in-order channel stream */
				while (TRUE)
				{
					const size_t eslot =
					    (size_t)(udp->expectedChannelSeq % RDPEUDP2_RECV_MAP);
					if (!udp->recvMap[eslot].occupied ||
					    (udp->recvMap[eslot].channelSeq != udp->expectedChannelSeq))
						break;
					if (!Stream_EnsureRemainingCapacity(
					        udp->recvStream, udp->recvMap[eslot].len))
						break;
					if (udp->recvMap[eslot].len > 0)
						Stream_Write(udp->recvStream, udp->recvMap[eslot].data,
						             udp->recvMap[eslot].len);
					free(udp->recvMap[eslot].data);
					udp->recvMap[eslot].data = nullptr;
					udp->recvMap[eslot].len = 0;
					udp->recvMap[eslot].occupied = FALSE;
					udp->expectedChannelSeq++;
				}
				/* Stream_Write advances position only; publish bytes via length. */
				Stream_SealLength(udp->recvStream);
				if (udp->sockEvent && (udp->sockEvent != INVALID_HANDLE_VALUE))
					(void)WSASetEvent(udp->sockEvent);
				if (udp->udpEvent && (udp->udpEvent != INVALID_HANDLE_VALUE))
					(void)SetEvent(udp->udpEvent);
			}
		}

}
int main(void){rdpUdpTransport u={0};u.expectedChannelSeq=1;u.recvDataBase=1;u.recvStream=Stream_New(NULL,256);Stream_SetLength(u.recvStream,0);
 feed(&u,2,2,'B');feed(&u,1,1,'A');printf("arrival 2:B,1:A -> delivered %.*s length=%zu expectedChannel=%u\n",(int)Stream_Length(u.recvStream),Stream_Buffer(u.recvStream),Stream_Length(u.recvStream),u.expectedChannelSeq);
 rdpUdpTransport v={0};v.recvDataBase=1;rdpeudp_note_recv_data_seq_locked(&v,1);rdpeudp_note_recv_data_seq_locked(&v,3);rdpeudp_note_recv_data_seq_locked(&v,2);
 BOOL anySeen=FALSE;for(size_t i=0;i<256;i++)anySeen|=v.recvDataSeen[i];UINT16 ackBase=v.recvDataBase-1;if(!anySeen)ackBase=v.lastAckSent;
 printf("arrival dseq 1,3,2 -> recvBase=%u lastAckSent=%u selected ACK=%u (contiguous through 3)\n",v.recvDataBase,v.lastAckSent,ackBase);Stream_Free(u.recvStream,TRUE);return 0;}
