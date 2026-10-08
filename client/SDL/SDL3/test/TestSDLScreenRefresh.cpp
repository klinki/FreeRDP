/** Screen refresh requests and cached-pixel recovery. Apache-2.0. */
#include "sdl_context.hpp"
#include "sdl_types.hpp"
#include "dialogs/sdl_dialogs.hpp"
#include <freerdp/gdi/gdi.h>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

// Supply a completed desktop frame without running a protocol handshake.
// Loopback tests cover real GDI/codec/connection initialization separately.
struct SdlScreenRefreshFixture
{
    rdpGdi gdi{};
    std::vector<BYTE> pixels = std::vector<BYTE>(320 * 200 * 4);
    rdpContext* context;
    explicit SdlScreenRefreshFixture(rdpContext* value) : context(value)
    {
        gdi.context = context; gdi.width = 320; gdi.height = 200; gdi.stride = 320 * 4;
        gdi.primary_buffer = pixels.data(); context->gdi = &gdi;
    }
    ~SdlScreenRefreshFixture() { context->gdi = nullptr; }
    bool initialize(SdlContext& sdl)
    {
        sdl._sdlPixelFormat = SDL_PIXELFORMAT_BGRA32;
        sdl._primary.reset(SDL_CreateSurfaceFrom(gdi.width, gdi.height, sdl._sdlPixelFormat,
                                               pixels.data(), gdi.stride));
        return sdl._primary != nullptr;
    }
};

static unsigned calls = 0, remoteMouseEvents = 0;
static BOOL captureMouse(rdpInput*, UINT16, UINT16, UINT16) { ++remoteMouseEvents; return TRUE; }
static RECTANGLE_16 captured{};
static bool sendSucceeds = true;
static bool connectionThread = true;
static BOOL captureRefresh(rdpContext*, BYTE count, const RECTANGLE_16* areas)
{
    connectionThread = connectionThread && !SDL_IsMainThread();
    if (count != 1 || !areas) return FALSE;
    ++calls;
    captured = areas[0];
    return sendSucceeds;
}
static bool expect(bool value, const char* message)
{
    if (!value) fprintf(stderr, "FAIL: %s\n", message);
    return value;
}
static void service(SdlContext& sdl)
{
    std::thread worker([&] { sdl.serviceScreenRefresh(); });
    worker.join();
}
static bool pixelIsGreen(SdlWindow& window, bool green)
{
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels(
        SDL_RenderReadPixels(window.renderer(), nullptr), SDL_DestroySurface);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    return pixels && SDL_ReadSurfacePixel(pixels.get(), pixels->w / 2, pixels->h / 2, &r, &g, &b, &a) &&
        (green ? g == 170 && r == 0 && b == 0 : r == 0 && g == 0 && b == 0);
}
static bool topBar(const char* preview)
{
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels(
        SDL_CreateSurface(800, 160, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    if (!pixels) return false;
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateSoftwareRenderer(pixels.get()), SDL_DestroyRenderer);
    if (!renderer) return false;
    SdlTopBar bar(renderer.get(), "Remote Desktop");
    bar.setPerformanceAvailable(true);
    const SDL_Rect viewport{0, 0, 800, 160};
    SDL_SetRenderDrawColor(renderer.get(), 58, 85, 104, 255);
    SDL_RenderClear(renderer.get());
    for (const bool compact : {false, true})
    {
        auto rect = SdlTopBar::clampToViewport(SdlTopBar::defaultRect(viewport, compact), viewport, compact);
        rect.y = compact ? 85 : 10;
        SDL_FPoint refresh{-1, -1};
        for (float x = rect.x; x < rect.x + rect.w; ++x)
            if (bar.hitTest(rect, viewport, {x, rect.y + rect.h / 2}) == SdlTopBarButton::Refresh)
            { refresh = {x + 2, rect.y + rect.h / 2}; break; }
        if (!expect(refresh.x >= rect.x && !bar.hitMove(rect, viewport, refresh), "Refresh owns clicks in both bar densities") ||
            !bar.draw(rect, viewport, true, refresh)) return false;
    }
    SDL_RenderPresent(renderer.get());
    return !preview || SDL_SaveBMP(pixels.get(), preview);
}
static bool run()
{
    std::unique_ptr<freerdp, decltype(&freerdp_free)> instance(freerdp_new(), freerdp_free);
    if (!instance) return false;
    instance->ContextSize = sizeof(sdl_rdp_context);
    if (!freerdp_context_new(instance.get())) return false;
    struct Guard { freerdp* value; ~Guard() { freerdp_context_free(value); } } guard{instance.get()};
    SdlContext sdl(instance->context);
    reinterpret_cast<sdl_rdp_context*>(instance->context)->sdl = &sdl;
    const auto settings = instance->context->settings;
    freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "refresh-test.invalid");
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 320);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 200);
    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE);
    SdlScreenRefreshFixture framebuffer(instance->context);
    if (!framebuffer.initialize(sdl) || !sdl.detectDisplays()) return false;
    const auto ids = sdl.getDisplayIds();
    if (ids.empty() || !sdl.addDisplayWindow(ids.front())) return false;
    const auto window = sdl.getFirstWindow();
    if (!window) return false;
    const auto gdi = instance->context->gdi;
    for (int y = 0; y < gdi->height; ++y)
        for (int x = 0; x < gdi->width; ++x)
        {
            auto pixel = gdi->primary_buffer + y * gdi->stride + x * 4;
            pixel[0] = 0; pixel[1] = 170; pixel[2] = 0; pixel[3] = 255;
        }
    instance->context->update->RefreshRect = captureRefresh;
    sdl.requestScreenRefresh(); service(sdl);
    if (!expect(calls == 0, "Disconnected refresh is discarded")) return false;
    sdl.setConnected(true);
    if (!window->fill() || !window->updateSurface() || !pixelIsGreen(*window, false)) return false;
    sdl.requestScreenRefresh(); sdl.requestScreenRefresh();
    if (!expect(calls == 0, "SDL thread only queues the server request") ||
        !expect(pixelIsGreen(*window, true), "Refresh restores pixels from the desktop buffer")) return false;
    service(sdl); service(sdl);
    if (!expect(calls == 1 && connectionThread, "Requests coalesce and send on the connection thread") ||
        !expect(captured.left == 0 && captured.top == 0 && captured.right == 319 && captured.bottom == 199,
                "Server refresh covers the complete composed desktop with inclusive coordinates")) return false;
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 3840);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 2160);
    sdl.requestScreenRefresh(); service(sdl);
    if (!expect(calls == 2 && captured.right == 3839 && captured.bottom == 2159,
                "Server request uses current desktop dimensions instead of Mac window size")) return false;
    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, FALSE);
    if (!window->fill() || !window->updateSurface()) return false;
    sdl.requestScreenRefresh(); service(sdl);
    if (!expect(calls == 2 && pixelIsGreen(*window, true), "Unsupported servers still receive a local repaint")) return false;
    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
    for (const UINT32 dimension : {0U, 65537U})
    {
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, dimension);
        sdl.requestScreenRefresh(); service(sdl);
    }
    if (!expect(calls == 2, "Invalid rectangle dimensions are discarded")) return false;
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 320);
    sdl.requestScreenRefresh(); sdl.setConnected(false); sdl.setConnected(true); service(sdl);
    if (!expect(calls == 2, "Pending refresh cannot cross a disconnect")) return false;
    sdl.requestScreenRefresh(); sdl.setReconnecting(true); service(sdl);
    sdl.requestScreenRefresh(); sdl.setReconnecting(false); service(sdl);
    if (!expect(calls == 2, "Reconnecting refresh requests are discarded")) return false;
    sendSucceeds = false;
    sdl.requestScreenRefresh(); service(sdl);
    if (!expect(calls == 3 && sdl.isConnected(), "Refresh failure does not terminate the session")) return false;
    sendSucceeds = true;
    freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE);
    instance->context->input->MouseEvent = captureMouse;
    if (!sdl.repaintAll()) return false;
    const auto bar = window->topBarRect();
    SDL_FPoint refresh{-1, -1};
    for (float x = bar.x; x < bar.x + bar.w; ++x)
        if (window->topBarButtonAt(x, bar.y + bar.h / 2) == SdlTopBarButton::Refresh)
        { refresh = {x + 2, bar.y + bar.h / 2}; break; }
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = window->id(); event.button.button = SDL_BUTTON_LEFT;
    event.button.x = refresh.x; event.button.y = refresh.y;
    if (!expect(refresh.x >= bar.x && sdl.handleEvent(event), "Refresh press is handled locally")) return false;
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    if (!expect(sdl.handleEvent(event), "Refresh release is handled locally")) return false;
    service(sdl);
    if (!expect(calls == 4 && remoteMouseEvents == 0, "Top-bar click triggers refresh without remote mouse input")) return false;
    sdl.setConnected(false);
    return true;
}
int main(int argc, char** argv)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    sdl_dialogs_init();
    const bool success = run() && topBar(argc > 1 ? argv[1] : nullptr);
    sdl_dialogs_uninit(); SDL_Quit();
    if (success) puts("PASS screen refresh requests, pixel recovery, reconnect safety, and top-bar controls");
    return success ? 0 : 1;
}
