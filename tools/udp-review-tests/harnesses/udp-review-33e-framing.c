/* Current-API diagnostics derived from 33e410c56. Original preserved in support/.
 * CREATE response is received by a SERVER; legacy envelope is no longer sent.
 * Prints observed lengths/rejections; not a passing conformance test. */
#include <stdio.h>
#include <string.h>
#include "libfreerdp/core/rdpeudp.h"
int main(void)
{
 size_t n=0;
 const BYTE response[]={0x10,7,0,0,0,0};
 BOOL ok=rdpeudp_dvc_pdu_length(response,sizeof(response),TRUE,&n);
 printf("CREATE success response: accepted=%d consumed=%zu expected=%zu\n",ok,n,sizeof(response));
 const BYTE raw[]={0x30,7,'A'};
 wStream* s=rdpeudp_build_channel_packet(1007,sizeof(raw),3,raw,sizeof(raw));
 if(!s) return 1;
 n=0; ok=rdpeudp_dvc_pdu_length(Stream_Buffer(s),Stream_Length(s),FALSE,&n);
 printf("Legacy utility envelope -> raw receive parser: accepted=%d firstByte=%02x\n",ok,Stream_Buffer(s)[0]);
 Stream_Release(s);
 return 0;
}
