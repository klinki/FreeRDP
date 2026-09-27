#include <stdio.h>
#include "libfreerdp/core/rdpeudp.h"
int main(void) {
 RdpUdp2Layout in = {0};
 in.flags=RDPUDP2_FLAG_ACK; in.logWindow=5; in.hasAck=TRUE; in.ackBase=0x1234;
 wStream* s=rdpeudp2_encode_layout(&in);
 if (!s) return 1;
 printf("Encoded: length=%zu position=%zu\n", Stream_Length(s), Stream_GetPosition(s));
 if (!rdpeudp2_protect(s,FALSE)) return 2;
 printf("Protected:");
 for (size_t i=0;i<Stream_Length(s);i++) printf(" %02x",Stream_Buffer(s)[i]);
 puts(""); Stream_Release(s);
 const BYTE bw[]={6,0,1,0,0x14,0};
 s=rdpemt_build_subheader(RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ,bw,sizeof(bw));
 printf("Subheader:");
 for(size_t i=0;i<Stream_Length(s);i++) printf(" %02x",Stream_Buffer(s)[i]);
 puts("");Stream_Release(s);
 size_t off=0,n=0; BYTE type=0; const BYTE* data=NULL;
 printf("Parse direct BW_START subheader: %d\n",rdpemt_next_subheader(bw,sizeof(bw),&off,&type,&data,&n));
 return 0;
}
