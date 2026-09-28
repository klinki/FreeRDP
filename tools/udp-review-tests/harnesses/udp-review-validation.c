#include <stdio.h>
#include <freerdp/utils/drdynvc.h>
#include <winpr/assert.h>
#include <winpr/stream.h>
#include <freerdp/log.h>
#include <freerdp/channels/drdynvc.h>
#include "libfreerdp/core/rdpeudp.h"
#undef WLog_Print
#define WLog_Print(...) ((void)0)
#define TAG "review"
typedef struct { void* log; } drdynvcPlugin;
static UINT drdynvc_send_soft_sync_response(drdynvcPlugin* d,BOOL migrate) { return migrate ? 123 : 456; }
static UINT drdynvc_process_soft_sync_request(drdynvcPlugin* drdynvc, int Sp, int cbChId,
                                              wStream* s)
{
	BOOL offersUdpFecr = FALSE;

	WINPR_ASSERT(drdynvc);
	WINPR_UNUSED(Sp);
	WINPR_UNUSED(cbChId);
	/* Header byte already consumed; rewind so the shared strict validator sees
	 * the whole PDU (U1: identical bytes as core snooping, so both sides always
	 * agree). The stream holds exactly one sealed PDU here. */
	Stream_Rewind(s, 1);
	{
		const BYTE* pdu = Stream_Pointer(s);
		const size_t len = Stream_GetRemainingLength(s);
		if (!drdynvc_soft_sync_request_validate(pdu, len))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "soft_sync_request: rejected");
			return ERROR_INVALID_DATA;
		}
		offersUdpFecr = drdynvc_soft_sync_request_offers_udp(pdu, len);
	}

	WLog_Print(drdynvc->log, WLOG_INFO, "soft_sync_request: offersUdpFecr=%d, responding",
	           offersUdpFecr);
	return drdynvc_send_soft_sync_response(drdynvc, offersUdpFecr);
}

int main(void) {
 BYTE cases[][21]={{0x80,0,19,0,0,0,3,0,1,0,1,0,0,0,1,0,7,0,0,0,0xff},{0x80,0,9,0,0,0,1,0,1,0,0xff},{0x80,0,8,0,0,0,1,0,1,0},{0x80,0,18,0,0,0,3,0,1,0,1,0,0,0,1,0,7,0,0,0},{0x80,0,18,0,0,0,3,0,1,0,3,0,0,0,1,0,7,0,0,0}};
 size_t sizes[]={21,11,10,20,20};UINT expected[]={ERROR_INVALID_DATA,ERROR_INVALID_DATA,123,123,456};
 for(int i=0;i<5;i++){drdynvcPlugin d={0};wStream sb={0};wStream* s=Stream_StaticInit(&sb,cases[i],sizes[i]);Stream_Seek(s,1);
 UINT rc=drdynvc_process_soft_sync_request(&d,0,0,s);
 printf("case=%d core offers=%d handler=%u (123=accept UDP,456=decline)\n",i,rdpeudp_soft_sync_request_offers_udp(cases[i],sizes[i]),rc);if(rc!=expected[i])return 1;}return 0;
}
