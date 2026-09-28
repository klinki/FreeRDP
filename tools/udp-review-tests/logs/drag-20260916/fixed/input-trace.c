#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
extern int freerdp_input_send_mouse_event(void*,uint16_t,uint16_t,uint16_t);
extern int freerdp_client_send_button_event(void*,int,uint16_t,int32_t,int32_t);
static double now(void){ struct timeval t; gettimeofday(&t,NULL); return t.tv_sec+t.tv_usec/1e6; }
static int trace_client_send(void* p,int relative,uint16_t flags,int32_t x,int32_t y){
 fprintf(stderr,"DRAG_MAPPED %.6f relative=%d flags=%04x x=%d y=%d\n",now(),relative,flags,x,y);
 return freerdp_client_send_button_event(p,relative,flags,x,y);
}
static int trace_send(void* p,uint16_t flags,uint16_t x,uint16_t y){
 fprintf(stderr,"DRAG_SENT %.6f flags=%04x x=%u y=%u\n",now(),flags,x,y);
 return freerdp_input_send_mouse_event(p,flags,x,y);
}
static bool trace_convert(SDL_Renderer* renderer,SDL_Event* event){
 SDL_Event before=*event;
 bool ok=SDL_ConvertEventToRenderCoordinates(renderer,event);
 if(before.type==SDL_EVENT_MOUSE_MOTION){
  float gx=0,gy=0; SDL_GetGlobalMouseState(&gx,&gy);
  fprintf(stderr,"DRAG_CONVERT %.6f age_ms=%.3f wid=%u state=%u raw=%.2f,%.2f converted=%.2f,%.2f global=%.2f,%.2f ok=%d\n",now(),(double)(SDL_GetTicksNS()-before.motion.timestamp)/1e6,before.motion.windowID,before.motion.state,before.motion.x,before.motion.y,event->motion.x,event->motion.y,gx,gy,ok);
 }
 return ok;
}
__attribute__((used)) static struct { const void* replacement; const void* original; } replacements[] __attribute__((section("__DATA,__interpose"))) = {
 {(const void*)trace_client_send,(const void*)freerdp_client_send_button_event},
 {(const void*)trace_send,(const void*)freerdp_input_send_mouse_event},
 {(const void*)trace_convert,(const void*)SDL_ConvertEventToRenderCoordinates}
};
