#include <stdio.h>
#include <winpr/assert.h>
#include <winpr/wtsapi.h>
#include <stdlib.h>
#include <string.h>
#include "libfreerdp/core/rdpeudp.h"
typedef struct { CRITICAL_SECTION lock; BOOL softSyncNegotiated; rdpUdpTransport* udp; UINT32* udpDvcIds; size_t udpDvcCount; BOOL mappingActive; BOOL mappingInstalled; BYTE* ssReqBuf; size_t ssReqLen, ssReqCap; BOOL ssReqActive; BOOL udpRecvMigrated; BOOL udpSendMigrated; BOOL softSyncComplete; HANDLE stateEvent; } rdpMultitransport;
BOOL rdpeudp_is_connected(const rdpUdpTransport* u) { return u != NULL; }
static BOOL multitransport_install_mapping_locked(rdpMultitransport* multi, const BYTE* pdu,
                                                  size_t len)
{
	size_t count = 0;
	WINPR_ASSERT(multi);
	/* Phase 1: strict-validate and count. NOTE: never touches ssReqBuf here;
	 * the feed path passes its own reassembly buffer as pdu. */
	if (!rdpeudp_soft_sync_request_udp_dvcs(pdu, len, nullptr, 0, &count))
		return FALSE;
	free(multi->udpDvcIds);
	multi->udpDvcIds = nullptr;
	multi->udpDvcCount = 0;
	multi->mappingActive = FALSE;
	multi->mappingInstalled = FALSE;
	if (count > 0)
	{
		UINT32* ids = malloc(count * sizeof(UINT32));
		size_t got = 0;
		if (!ids)
			return FALSE; /* OOM: stay TCP-safe */
		/* Phase 2: fill (revalidates; cannot fail after phase 1). */
		if (!rdpeudp_soft_sync_request_udp_dvcs(pdu, len, ids, count, &got) || (got != count))
		{
			free(ids);
			return FALSE;
		}
		multi->udpDvcIds = ids;
		multi->udpDvcCount = count;
		multi->mappingActive = TRUE;
	}
	multi->mappingInstalled = TRUE;
	return TRUE;
}

void multitransport_on_soft_sync_request_sent(rdpMultitransport* multi, const BYTE* pdu,
                                                size_t len)
{
	if (!multi || !pdu || (len == 0))
		return;
	EnterCriticalSection(&multi->lock);
	/* Server: after sending Soft-Sync Request it starts sending on UDP
	 * (MS-RDPEDYC 3.1.5.3). Recv still on TCP until Response arrives. */
	if (multi->softSyncNegotiated && multi->udp && rdpeudp_is_connected(multi->udp))
	{
		if (multitransport_install_mapping_locked(multi, pdu, len))
			multi->udpSendMigrated = TRUE;
	}
	LeaveCriticalSection(&multi->lock);
}

/* Feed one drdynvc static-channel chunk (T1: handles fragmented Soft-Sync
 * requests the single-chunk fast path cannot see). Passive: normal dispatch is
 * unaffected. Accumulates FIRST..LAST when the first chunk opens a Soft-Sync
 * Request; on LAST the validated mapping is installed and recv is enabled. */
void multitransport_soft_sync_recv_feed(rdpMultitransport* multi, const BYTE* chunk,
                                        size_t chunkLen, UINT32 flags)
{
	BOOL installed = FALSE;
	if (!multi || !chunk || (chunkLen == 0))
		return;
	EnterCriticalSection(&multi->lock);
	if (!multi->softSyncNegotiated || !multi->udp || !rdpeudp_is_connected(multi->udp))
	{
		LeaveCriticalSection(&multi->lock);
		return;
	}
	if (!multi->ssReqActive)
	{
		/* Start only on a chunk opening a Soft-Sync Request. */
		if (!(flags & CHANNEL_FLAG_FIRST) || (((chunk[0] >> 4) & 0x0F) != 0x08))
		{
			LeaveCriticalSection(&multi->lock);
			return;
		}
		free(multi->ssReqBuf);
		multi->ssReqBuf = malloc(chunkLen);
		if (!multi->ssReqBuf)
		{
			LeaveCriticalSection(&multi->lock);
			return; /* OOM: stay TCP-safe */
		}
		memcpy(multi->ssReqBuf, chunk, chunkLen);
		multi->ssReqLen = chunkLen;
		multi->ssReqCap = chunkLen;
		multi->ssReqActive = TRUE;
	}
	else
	{
		/* Continuation: a new FIRST means the previous PDU never completed;
		 * resync by restarting (TCP-safe: nothing was installed for it). */
		if (flags & CHANNEL_FLAG_FIRST)
		{
			free(multi->ssReqBuf);
			multi->ssReqBuf = nullptr;
			multi->ssReqLen = 0;
			multi->ssReqCap = 0;
			multi->ssReqActive = FALSE;
			if ((((chunk[0] >> 4) & 0x0F) != 0x08))
			{
				LeaveCriticalSection(&multi->lock);
				return;
			}
			multi->ssReqBuf = malloc(chunkLen);
			if (!multi->ssReqBuf)
			{
				LeaveCriticalSection(&multi->lock);
				return;
			}
			memcpy(multi->ssReqBuf, chunk, chunkLen);
			multi->ssReqLen = chunkLen;
			multi->ssReqCap = chunkLen;
			multi->ssReqActive = TRUE;
		}
		else
		{
			if (multi->ssReqLen + chunkLen > 65536)
			{
				/* Runaway accumulation: drop and stay TCP-safe. */
				free(multi->ssReqBuf);
				multi->ssReqBuf = nullptr;
				multi->ssReqLen = 0;
				multi->ssReqCap = 0;
				multi->ssReqActive = FALSE;
				LeaveCriticalSection(&multi->lock);
				return;
			}
			BYTE* grown = realloc(multi->ssReqBuf, multi->ssReqLen + chunkLen);
			if (!grown)
			{
				free(multi->ssReqBuf);
				multi->ssReqBuf = nullptr;
				multi->ssReqLen = 0;
				multi->ssReqCap = 0;
				multi->ssReqActive = FALSE;
				LeaveCriticalSection(&multi->lock);
				return;
			}
			multi->ssReqBuf = grown;
			memcpy(multi->ssReqBuf + multi->ssReqLen, chunk, chunkLen);
			multi->ssReqLen += chunkLen;
			multi->ssReqCap = multi->ssReqLen;
		}
	}
	if (flags & CHANNEL_FLAG_LAST)
	{
		/* Complete: install mapping and enable recv (early server UDP data
		 * must not be missed); send waits for our Response. */
		if (multitransport_install_mapping_locked(multi, multi->ssReqBuf, multi->ssReqLen))
		{
			multi->udpRecvMigrated = TRUE;
			installed = TRUE;
		}
		free(multi->ssReqBuf);
		multi->ssReqBuf = nullptr;
		multi->ssReqLen = 0;
		multi->ssReqCap = 0;
		multi->ssReqActive = FALSE;
	}
	LeaveCriticalSection(&multi->lock);
	if (installed && multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
		(void)SetEvent(multi->stateEvent);
}

BOOL multitransport_is_dvc_migrated(const rdpMultitransport* multi, UINT32 dvcId)
{
	BOOL rc = FALSE;
	if (!multi)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&multi->lock);
	if (multi->udp && rdpeudp_is_connected(multi->udp) && multi->udpSendMigrated)
	{
		if (!multi->mappingActive)
			rc = TRUE; /* migrate-all (no lists / no Soft-Sync pre-ACTIVE) */
		else
		{
			for (size_t i = 0; i < multi->udpDvcCount; i++)
			{
				if (multi->udpDvcIds[i] == dvcId)
				{
					rc = TRUE;
					break;
				}
			}
		}
	}
	LeaveCriticalSection((CRITICAL_SECTION*)&multi->lock);
	return rc;
}

void multitransport_on_soft_sync_response_sent(rdpMultitransport* multi)
{
	if (!multi)
		return;
	EnterCriticalSection(&multi->lock);
	/* Client: after sending Response, start sending (and ensure recv) on UDP,
	 * but only when a validated mapping was installed first (T1: a failed or
	 * skipped install must never become migrate-all). */
	if (multi->softSyncNegotiated && multi->mappingInstalled && multi->udp &&
	    rdpeudp_is_connected(multi->udp))
	{
		multi->softSyncComplete = TRUE;
		multi->udpSendMigrated = TRUE;
		multi->udpRecvMigrated = TRUE;
	}
	LeaveCriticalSection(&multi->lock);
	if (multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
		(void)SetEvent(multi->stateEvent);
}

int main(void) {
 rdpMultitransport m={0};InitializeCriticalSection(&m.lock);m.softSyncNegotiated=TRUE;m.udp=(rdpUdpTransport*)1;
 BYTE p[16+257*4]={0x80,0};size_t n=sizeof(p);UINT32 l=n-2;memcpy(p+2,&l,4);p[6]=3;p[8]=1;p[10]=1;p[14]=1;p[15]=1;
 for(UINT32 i=0;i<257;i++){UINT32 id=i+1;memcpy(p+16+i*4,&id,4);}
 printf("strict offers=%d\n",rdpeudp_soft_sync_request_offers_udp(p,n));
 multitransport_on_soft_sync_response_sent(&m);
 printf("without request: migrated=%d\n",m.udpSendMigrated);
 multitransport_soft_sync_recv_feed(&m,p,10,CHANNEL_FLAG_FIRST);
 printf("after first: installed=%d recv=%d\n",m.mappingInstalled,m.udpRecvMigrated);
 multitransport_soft_sync_recv_feed(&m,p+10,n-10,CHANNEL_FLAG_LAST);
 printf("map before response: active=%d count=%zu\n",m.mappingActive,m.udpDvcCount);
 multitransport_on_soft_sync_response_sent(&m);
 printf("unlisted DVC 999 migrated=%d\n",multitransport_is_dvc_migrated(&m,999));
 free(m.udpDvcIds);DeleteCriticalSection(&m.lock);return 0;
}
