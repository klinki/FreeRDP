#include "client/SDL/SDL3/sdl_window.hpp"
#include <cstdio>

// Model SDL's documented Windows/X11 4K display at 200% UI scaling:
// display bounds are already physical pixels; display scale is 2, density is 1.
static float density = 1.0f;

extern "C" bool SDL_GetDisplayBounds(SDL_DisplayID, SDL_Rect* bounds)
{
    *bounds = density == 1.0f ? SDL_Rect{0, 0, 3840, 2160} : SDL_Rect{0, 0, 1920, 1080};
    return true;
}
extern "C" float SDL_GetWindowDisplayScale(SDL_Window*) { return 2.0f; }
extern "C" float SDL_GetDisplayContentScale(SDL_DisplayID) { return 2.0f / density; }
extern "C" float SDL_GetWindowPixelDensity(SDL_Window*) { return density; }

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO))
        return 1;
    int rc = 0;
    for (density = 1.0f; density <= 2.0f; density += 1.0f)
    {
        const auto monitor = SdlWindow::query(SDL_GetPrimaryDisplay());
        std::printf("Density %.0f: expected 3840x2160 at 200%%; actual %dx%d at %u%%\n",
                    density, monitor.width, monitor.height, monitor.attributes.desktopScaleFactor);
        if (monitor.width != 3840 || monitor.height != 2160 ||
            monitor.attributes.desktopScaleFactor != 200)
            rc = 1;
    }
    SDL_Quit();
    return rc;
}
