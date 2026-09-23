#include <winpr/pool.h>
static unsigned long reviewWaitCalls;
static void reviewWait(PTP_WORK work, BOOL cancel) { ++reviewWaitCalls; winpr_WaitForThreadpoolWorkCallbacks(work,cancel); }
#define winpr_WaitForThreadpoolWorkCallbacks reviewWait
#include "libfreerdp/codec/yuv.c"
#undef winpr_WaitForThreadpoolWorkCallbacks

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <freerdp/codec/yuv.h>
#include <freerdp/codec/color.h>
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9; }
int main(void) {
    const UINT32 w=3840,h=2160,strides[3]={w,w/2,w/2};
    BYTE* y=malloc(w*h); BYTE* u=malloc(w*h/4); BYTE* v=malloc(w*h/4); BYTE* out=malloc(w*h*4);
    memset(y,128,w*h);memset(u,128,w*h/4);memset(v,128,w*h/4);
    const BYTE* planes[3]={y,u,v}; const RECTANGLE_16 full={0,0,w,h},small={0,0,16,16};
    YUV_CONTEXT* ctx=yuv_context_new(FALSE,0);if(!ctx||!yuv_context_reset(ctx,w,h)) return 1;
    for(int phase=0;phase<2;phase++) {
        if(phase && !yuv420_context_decode(ctx,planes,strides,h,PIXEL_FORMAT_BGRA32,out,w*4,&full,1))return 1;
        reviewWaitCalls=0; double start=now();
        for(int i=0;i<2000;i++) if(!yuv420_context_decode(ctx,planes,strides,h,PIXEL_FORMAT_BGRA32,out,w*4,&small,1))return 1;
        printf("small rect, phase %d: %.3f us/call, wait calls=%lu/call\n",phase,(now()-start)*1e6/2000,reviewWaitCalls/2000);
    }
    yuv_context_free(ctx);free(y);free(u);free(v);free(out);return 0;
}
