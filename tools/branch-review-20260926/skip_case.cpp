#include "client/SDL/SDL3/sdl_render_metrics.hpp"
#include <cstdlib>
uint64_t clockFn(void* p) noexcept{return *static_cast<uint64_t*>(p);}
int main(){uint64_t now=1;SdlRenderMetrics metric(1,1,clockFn,&now);metric.beginFrame(1);metric.endFrame();for(int i=1;i<=60;i++){now=uint64_t(i)*1000000000;metric.notePresentSkip();}metric.flush();}
