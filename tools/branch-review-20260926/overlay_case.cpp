#include "client/SDL/SDL3/sdl_window.hpp"
#include <SDL3_ttf/SDL_ttf.h>
#include <cstdio>
class ReviewWindow:public SdlWindow { public: ReviewWindow(): SdlWindow(SDL_GetPrimaryDisplay(),"Review",{0,0,640,480},0) {} };
int main(){ SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");if(!SDL_Init(SDL_INIT_VIDEO)||!TTF_Init())return 1; {ReviewWindow window;printf("overlay=%d\n",window.updateStalledSurface(1));SDL_ClearError();} printf("destroy error=%s\n",SDL_GetError());TTF_Quit();SDL_Quit();}
