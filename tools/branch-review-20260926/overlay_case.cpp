#include "client/SDL/SDL3/sdl_window.hpp"
#include <SDL3_ttf/SDL_ttf.h>
#include <cstdio>

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init())
        return 1;
    {
        auto window = SdlWindow::create(SDL_GetPrimaryDisplay(), "Review", 0, 640, 480);
        if (!window.updateStalledSurface(1))
            return 1;
        SDL_ClearError();
    }
    const int rc = SDL_GetError()[0] != '\0';
    std::printf("destroy error=%s\n", SDL_GetError());
    TTF_Quit();
    SDL_Quit();
    return rc;
}
