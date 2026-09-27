/* Production codec plus the channels.c VCChunkSize split arithmetic.
 * VCChunkSize=512, a 600-byte DATA PDU (within drdynvc's 1600-byte limit).
 * This is not a socket/TLS test. */
#include <stdio.h>
#include <string.h>
#include "libfreerdp/core/rdpeudp.h"
int main(void) { BYTE pdu[600]; memset(pdu,0xff,sizeof(pdu));pdu[0]=0x30;pdu[1]=7;
 size_t n=0;BOOL a=rdpeudp_dvc_pdu_length(pdu,512,FALSE,&n);
 printf("first SVC chunk accepted=%d consumed=%zu of full 600-byte DVC\n",a,n);
 BOOL b=rdpeudp_dvc_pdu_length(pdu+512,sizeof(pdu)-512,FALSE,&n);
 printf("continuation SVC chunk accepted=%d (payload byte misread as command)\n",b);
 return 0; }
