/** Screen refresh variants, safe output restart and hover controls. Apache-2.0. */
#include "sdl_context.hpp"
#include "sdl_types.hpp"
#include "dialogs/sdl_dialogs.hpp"
#include <freerdp/client/cmdline.h>
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
    static void suppress(SdlContext& sdl, bool value) { sdl._screenOutputSuppressed = value; }
    bool initialize(SdlContext& sdl)
    {
        sdl._sdlPixelFormat = SDL_PIXELFORMAT_BGRA32;
        sdl._primary.reset(SDL_CreateSurfaceFrom(gdi.width, gdi.height, sdl._sdlPixelFormat,
                                               pixels.data(), gdi.stride));
        return sdl._primary != nullptr;
    }
};


struct Call { char kind; RECTANGLE_16 area{}; };
static std::vector<Call> calls;
static unsigned remoteMouse = 0, remoteKeys = 0;
static bool redrawOK = true, pauseOK = true, onConnectionThread = true;
static int resumeFailures = 0;
static BOOL mouse(rdpInput*, UINT16, UINT16, UINT16) { ++remoteMouse; return TRUE; }
static BOOL key(rdpInput*, UINT16, UINT8) { ++remoteKeys; return TRUE; }
static BOOL refresh(rdpContext*, BYTE count, const RECTANGLE_16* areas)
{
    onConnectionThread &= !SDL_IsMainThread();
    if (count != 1 || !areas) return FALSE;
    calls.push_back({'R', areas[0]}); return redrawOK;
}
static BOOL suppress(rdpContext*, BYTE allow, const RECTANGLE_16* area)
{
    onConnectionThread &= !SDL_IsMainThread();
    if ((allow == 0 && area) || (allow != 0 && !area)) return FALSE;
    calls.push_back({allow ? 'U' : 'P', area ? *area : RECTANGLE_16{}});
    if (!allow) return pauseOK;
    if (resumeFailures > 0) { --resumeFailures; return FALSE; }
    return TRUE;
}
static bool expect(bool ok, const char* message)
{
    if (!ok) fprintf(stderr, "FAIL: %s\n", message);
    return ok;
}
static bool service(SdlContext& context)
{
    bool result = false;
    std::thread worker([&] { result = context.serviceScreenRefresh(); }); worker.join();
    return result;
}
static std::string order()
{
    std::string result; for (const auto& call : calls) result += call.kind; return result;
}
static bool green(SdlWindow& window, bool expected)
{
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels(SDL_RenderReadPixels(window.renderer(), nullptr), SDL_DestroySurface);
    Uint8 r=0,g=0,b=0,a=0;
    return pixels && SDL_ReadSurfacePixel(pixels.get(), pixels->w/2, pixels->h/2, &r,&g,&b,&a) &&
        (expected ? r==0 && g==170 && b==0 : r==0 && g==0 && b==0);
}
static SDL_FPoint findButton(SdlTopBar& bar, const SdlTopBarRect& rect, const SDL_Rect& viewport, SdlTopBarButton button)
{
    for (float x=rect.x; x<rect.x+rect.w; ++x)
        if (bar.hitTest(rect, viewport, {x, rect.y+rect.h/2})==button) return {x+2, rect.y+rect.h/2};
    return {-1,-1};
}
static bool layoutAndPreview(const char* preview, bool menuEnabled)
{
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels(SDL_CreateSurface(1300, 320, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    if (!pixels) return false;
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(SDL_CreateSoftwareRenderer(pixels.get()), SDL_DestroyRenderer);
    if (!renderer) return false;
    SdlTopBar bar(renderer.get(), "Remote Desktop"); bar.setPerformanceAvailable(true);
    SDL_SetRenderDrawColor(renderer.get(),58,85,104,255); SDL_RenderClear(renderer.get());
    for (const bool compact : {false,true}) {
        const SDL_Rect viewport{0,0,1300,320};
        auto rect=SdlTopBar::defaultRect(viewport,compact); rect.x=compact?700:20; rect.y=20;
        bar.closeRefreshMenu();
        const auto point=findButton(bar,rect,viewport,SdlTopBarButton::Refresh);
        if (!expect(point.x >= 0 && bar.updateHover(rect, viewport, point, 100) == menuEnabled &&
                    bar.refreshMenuOpen() == menuEnabled && bar.selectedRefresh() == SdlTopBarButton::RefreshCombined,
                    "The menu is opt-in and E is the initial method in both bar densities") ||
            !bar.draw(rect,viewport,true,point)) return false;
        if (!menuEnabled) {
            if (!SDL_RenderPresent(renderer.get())) return false;
            const auto menu = bar.refreshMenuRect(rect, viewport);
            const SDL_FPoint hidden{menu.x + menu.w / 2, menu.y + menu.h / 2};
            Uint8 r = 0, g = 0, b = 0, a = 0;
            if (!expect(!bar.contains(rect, viewport, hidden) &&
                        bar.hitTest(rect, viewport, hidden) == SdlTopBarButton::None,
                        "The disabled menu has no invisible input area") ||
                !SDL_ReadSurfacePixel(pixels.get(), static_cast<int>(hidden.x), static_cast<int>(hidden.y), &r, &g, &b, &a) ||
                !expect(r == 58 && g == 85 && b == 104, "Hovering paints no menu when its flag is off")) return false;
        }
    }
    SDL_RenderPresent(renderer.get());
    if (preview && !SDL_SaveBMP(pixels.get(),preview)) return false;
    if (!menuEnabled) return true;
    for (const auto viewport : {SDL_Rect{0,0,320,200},SDL_Rect{0,0,800,600},SDL_Rect{0,0,3840,2160}})
        for (const bool compact : {false,true}) for (const bool bottom : {false,true}) for (const bool right : {false,true}) {
            auto rect=SdlTopBar::clampToViewport(SdlTopBar::defaultRect(viewport,compact),viewport,compact);
            rect.x=right?viewport.w-rect.w:0; rect.y=bottom?viewport.h-rect.h:0;
            bar.closeRefreshMenu(); const auto point=findButton(bar,rect,viewport,SdlTopBarButton::Refresh);
            if (!bar.updateHover(rect,viewport,point,100)) return false;
            const auto menu=bar.refreshMenuRect(rect,viewport);
            if (!expect(menu.x>=0 && menu.y>=0 && menu.x+menu.w<=viewport.w && menu.y+menu.h<=viewport.h,
                        "Popup stays inside small, Retina, and edge-positioned viewports")) return false;
            const float bridgeY=bottom?rect.y-1:rect.y+rect.h+1;
            const SDL_FPoint bridge{point.x,bridgeY};
            if (!expect(bar.contains(rect,viewport,bridge),"The gap between icon and panel remains local")) return false;
            std::ignore=bar.updateHover(rect,viewport,bridge,110);
            for (size_t i=0;i<sdlRefreshModes.size();++i) {
                const SDL_FPoint row{menu.x+menu.w/2,menu.y+(i+0.5f)*menu.h/5};
                const auto button=SdlTopBar::refreshButton(sdlRefreshModes[i]);
                if (!expect(bar.hitTest(rect,viewport,row)==button && !bar.hitMove(rect,viewport,row),"Each popup button owns its row without moving the bar")) return false;
            }
            std::ignore=bar.updateHover(rect,viewport,{-5,-5},200);
            if (!expect(bar.refreshMenuOpen() && !bar.serviceHover(499) && bar.serviceHover(500) && !bar.refreshMenuOpen(),
                        "Leaving the hover region closes the panel after a bounded grace period")) return false;
        }
    bar.setRefreshCapabilities(false,false);
    return expect(!bar.refreshEnabled(SdlTopBarButton::RefreshServer) && !bar.refreshEnabled(SdlTopBarButton::RefreshUpdates) &&
                  bar.refreshEnabled(SdlTopBarButton::RefreshCombined),"Unsupported server methods are disabled individually");
}
static bool run(bool menuEnabled)
{
    calls.clear(); remoteMouse = remoteKeys = 0; onConnectionThread = true;
    std::unique_ptr<freerdp,decltype(&freerdp_free)> instance(freerdp_new(),freerdp_free);
    if (!instance) return false;
    instance->ContextSize=sizeof(sdl_rdp_context);
    if (!freerdp_context_new(instance.get())) return false;
    struct Guard { freerdp* value; ~Guard(){freerdp_context_free(value);} } guard{instance.get()};
    SdlContext sdl(instance->context); reinterpret_cast<sdl_rdp_context*>(instance->context)->sdl=&sdl;
    const auto settings=instance->context->settings;
    if (!freerdp_settings_set_string(settings,FreeRDP_ServerHostname,"refresh-test.invalid") ||
        !freerdp_settings_set_uint32(settings,FreeRDP_DesktopWidth,320) ||
        !freerdp_settings_set_uint32(settings,FreeRDP_DesktopHeight,200) ||
        !freerdp_settings_set_bool(settings,FreeRDP_RefreshRect,TRUE) ||
        !freerdp_settings_set_bool(settings,FreeRDP_SuppressOutput,TRUE) ||
        !freerdp_settings_set_bool(settings,FreeRDP_UseMultimon,FALSE)) return false;
    SdlScreenRefreshFixture frame(instance->context);
    if (!frame.initialize(sdl) || !sdl.detectDisplays()) return false;
    const auto displays=sdl.getDisplayIds(); if (displays.empty() || !sdl.addDisplayWindow(displays.front())) return false;
    const auto window=sdl.getFirstWindow(); if (!window) return false;
    for (size_t p=0;p<frame.pixels.size();p+=4) {frame.pixels[p]=0;frame.pixels[p+1]=170;frame.pixels[p+2]=0;frame.pixels[p+3]=255;}
    instance->context->update->RefreshRect=refresh; instance->context->update->SuppressOutput=suppress;
    instance->context->input->MouseEvent=mouse; instance->context->input->KeyboardEvent=key;
    sdl.requestScreenRefresh(); if (!service(sdl) || !expect(calls.empty(),"Disconnected requests are discarded")) return false;
    sdl.setConnected(true);
    for (const auto mode : {SdlRefreshMode::Local,SdlRefreshMode::Textures}) {
        if (!window->fill()) return false;
        const auto target=SDL_GetRenderTarget(window->renderer());
        if (!target || !SDL_SetBooleanProperty(SDL_GetTextureProperties(target),"refresh.test.marker",true) ||
            !window->updateSurface() || !green(*window,false)) return false;
        sdl.requestScreenRefresh(mode);
        if (!expect(green(*window,true) && calls.empty(),"Local methods repair the cached display without remote packets")) return false;
        if (!window->fill()) return false;
        const bool retained=SDL_GetBooleanProperty(SDL_GetTextureProperties(SDL_GetRenderTarget(window->renderer())),"refresh.test.marker",false);
        if (!expect(retained==(mode==SdlRefreshMode::Local),"Texture rebuild recreates targets; simple repaint retains them")) return false;
        sdl.requestScreenRefresh(SdlRefreshMode::Local);
    }
    sdl.requestScreenRefresh(); sdl.requestScreenRefresh();
    if (!expect(calls.empty(),"Server requests remain off the SDL thread") || !service(sdl) || !service(sdl) ||
        !expect(order()=="R" && calls.back().area.right==319 && calls.back().area.bottom==199,"Legacy click coalesces into an inclusive full-desktop request")) return false;
    calls.clear(); sdl.requestScreenRefresh(SdlRefreshMode::Updates); if (!service(sdl) || !expect(order()=="PU","Restart updates pauses then resumes without a Refresh Rect")) return false;
    calls.clear(); sdl.requestScreenRefresh(SdlRefreshMode::Combined); if (!service(sdl) || !expect(order()=="PUR","Combined recovery resumes before requesting a redraw")) return false;
    if (!freerdp_settings_set_uint32(settings,FreeRDP_DesktopWidth,3840) || !freerdp_settings_set_uint32(settings,FreeRDP_DesktopHeight,2160)) return false;
    calls.clear(); sdl.requestScreenRefresh(SdlRefreshMode::Combined); if (!service(sdl) || !expect(calls.back().area.right==3839 && calls.back().area.bottom==2159,"Server methods use the current composed desktop dimensions")) return false;
    for (const UINT32 dimension : {0U,65537U}) {
        if (!freerdp_settings_set_uint32(settings,FreeRDP_DesktopWidth,dimension)) return false;
        calls.clear(); sdl.requestScreenRefresh(SdlRefreshMode::Combined); if (!service(sdl) || !expect(calls.empty(),"Invalid dimensions never pause output")) return false;
    }
    if (!freerdp_settings_set_uint32(settings,FreeRDP_DesktopWidth,320) || !freerdp_settings_set_uint32(settings,FreeRDP_DesktopHeight,200)) return false;
    for (const auto setting : {FreeRDP_RefreshRect,FreeRDP_SuppressOutput}) {
        if (!freerdp_settings_set_bool(settings,setting,FALSE)) return false;
        calls.clear(); sdl.requestScreenRefresh(SdlRefreshMode::Combined); if (!service(sdl) || !expect(order()==(setting==FreeRDP_RefreshRect?"PU":"R"),"Combined recovery retains supported components")) return false;
        if (!freerdp_settings_set_bool(settings,setting,TRUE)) return false;
    }
    calls.clear(); pauseOK=false; sdl.requestScreenRefresh(SdlRefreshMode::Updates);
    if (!service(sdl) || !expect(order()=="PU","A failed pause never skips the resume attempt")) return false; pauseOK=true;
    calls.clear(); resumeFailures=1; sdl.requestScreenRefresh(SdlRefreshMode::Updates);
    if (!service(sdl) || !service(sdl) || !expect(order()=="PUU","A failed resume is retried without pausing again")) return false;
    calls.clear(); resumeFailures=1; sdl.requestScreenRefresh(SdlRefreshMode::Combined);
    if (!service(sdl) || !service(sdl) || !expect(order()=="PUUR","Combined recovery defers its redraw until resume succeeds")) return false;
    calls.clear(); resumeFailures=20; sdl.requestScreenRefresh(SdlRefreshMode::Updates);
    if (!service(sdl) || !service(sdl) || !service(sdl) || service(sdl) || !expect(order()=="PUUUU","Repeated resume failure triggers bounded transport recovery")) return false;
    resumeFailures=0; sdl.setConnected(false); sdl.setConnected(true);
    calls.clear(); SdlScreenRefreshFixture::suppress(sdl,true); sdl.requestScreenRefresh(SdlRefreshMode::Combined);
    if (!service(sdl) || !expect(calls.empty(),"Refresh does not resume intentionally hidden/minimized output")) return false;
    SdlScreenRefreshFixture::suppress(sdl,false);
    for (const bool reconnect : {false,true}) {
        calls.clear(); sdl.requestScreenRefresh(SdlRefreshMode::Combined);
        if (reconnect) {sdl.setReconnecting(true);sdl.requestScreenRefresh(SdlRefreshMode::Combined);sdl.setReconnecting(false);}
        else {sdl.setConnected(false);sdl.setConnected(true);}
        if (!service(sdl) || !expect(calls.empty(),"Pending methods cannot cross disconnect/reconnect")) return false;
    }
    calls.clear(); redrawOK=false; sdl.requestScreenRefresh();
    if (!service(sdl) || !expect(sdl.isConnected(),"Redraw failure remains nonfatal")) return false; redrawOK=true;
    if (!freerdp_settings_set_bool(settings,FreeRDP_UseMultimon,TRUE) || !sdl.repaintAll()) return false;
    SdlTopBar geometry(nullptr,"");
    window->setPerformanceOverlay(true,"FPS: 60\nRTT: 12 ms\nRx: 2 MiB/s",SdlPerformanceOverlay::Style::Panel);
    if (!sdl.repaintAll()) return false;
    // Place the performance overlay underneath the hover panel; the popup must own the click.
    const auto popup=geometry.refreshMenuRect(window->topBarRect(),window->pixelViewport());
    auto overlay=window->performanceOverlay();
    const auto bounds=overlay->bounds();
    SDL_MouseButtonEvent drag{};drag.type=SDL_EVENT_MOUSE_BUTTON_DOWN;drag.button=SDL_BUTTON_LEFT;
    SdlPerformanceOverlay::Action action;
    if (!overlay->button(drag,{bounds.x+10,bounds.y+10},action) ||
        !overlay->motion({popup.x+10,popup.y+10})) return false;
    drag.type=SDL_EVENT_MOUSE_BUTTON_UP;
    if (!overlay->button(drag,{popup.x+10,popup.y+10},action)) return false;
    if (!sdl.repaintAll()) return false;
    // The server may hide its cursor; the reconnect overlay and the connection bar still need the arrow.
    {
        if (!sdl.setCursor(SdlContext::CURSOR_NULL) || !expect(!SDL_CursorVisible(),"A hidden server cursor hides the pointer")) return false;
        sdl.setReconnecting(true);
        if (!sdl.restoreCursor() || !expect(SDL_CursorVisible() && !sdl.hasCursor(),"Reconnecting shows the local arrow and keeps the server cursor state")) return false;
        if (!sdl.setCursor(SdlContext::CURSOR_NULL) || !expect(SDL_CursorVisible(),"A server cursor change cannot hide the arrow while reconnecting")) return false;
        sdl.setReconnecting(false);
        if (!sdl.restoreCursor() || !expect(!SDL_CursorVisible(),"Recovery restores the hidden server cursor")) return false;
        const auto mouseBefore=remoteMouse;
        const auto onBar=findButton(geometry,window->topBarRect(),window->pixelViewport(),SdlTopBarButton::Refresh);
        SDL_Event event{}; event.type=SDL_EVENT_MOUSE_MOTION; event.motion.windowID=window->id(); event.motion.x=onBar.x; event.motion.y=onBar.y;
        if (!sdl.handleEvent(event) || !expect(SDL_CursorVisible() && remoteMouse==mouseBefore,"The connection bar shows the local arrow over a hidden server cursor")) return false;
        event={}; event.type=SDL_EVENT_WINDOW_MOUSE_LEAVE; event.window.windowID=window->id();
        if (!sdl.handleEvent(event) || !expect(!SDL_CursorVisible(),"Leaving the connection bar restores the hidden server cursor")) return false;
        event={}; event.type=SDL_EVENT_MOUSE_MOTION; event.motion.windowID=window->id(); event.motion.x=onBar.x; event.motion.y=onBar.y;
        if (!sdl.handleEvent(event) || !expect(SDL_CursorVisible(),"Returning to the connection bar shows the arrow again")) return false;
        // Moving straight into a window without the bar must not leave the arrow behind.
        event.motion.windowID=window->id()+1000;
        if (!sdl.handleEvent(event) || !expect(!SDL_CursorVisible(),"Motion outside the bar's window restores the hidden server cursor")) return false;
        if (!sdl.setCursor(SdlContext::CURSOR_DEFAULT)) return false;
    }
    const auto clickRefreshIcon = [&](const SDL_FPoint& point) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION; event.motion.windowID = window->id();
        event.motion.x = point.x; event.motion.y = point.y;
        if (!sdl.handleEvent(event)) return false;
        event = {}; event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = window->id(); event.button.button = SDL_BUTTON_LEFT;
        event.button.x = point.x; event.button.y = point.y;
        if (!sdl.handleEvent(event)) return false;
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        return sdl.handleEvent(event) && service(sdl);
    };
    calls.clear();
    const auto defaultIcon = findButton(geometry, window->topBarRect(), window->pixelViewport(), SdlTopBarButton::Refresh);
    if (!expect(window->selectedRefresh() == SdlTopBarButton::RefreshCombined,
                "A new connection bar defaults to combined recovery") ||
        !clickRefreshIcon(defaultIcon) || !expect(order() == "PUR", "The initial icon click runs combined recovery")) return false;
    if (!menuEnabled) {
        if (!expect(!window->refreshMenuOpen() && remoteMouse == 0 && onConnectionThread,
                    "With the flag unset the plain refresh icon runs E without opening a menu or sending remote input")) return false;
        sdl.setConnected(false); return true;
    }
    for (const auto mode : sdlRefreshModes) {
        window->closeRefreshMenu(); calls.clear();
        const auto bar=window->topBarRect();
        const auto anchor=findButton(geometry,bar,window->pixelViewport(),SdlTopBarButton::Refresh);
        SDL_Event event{}; event.type=SDL_EVENT_MOUSE_MOTION;event.motion.windowID=window->id();event.motion.x=anchor.x;event.motion.y=anchor.y;
        if (!sdl.handleEvent(event) || !expect(window->refreshMenuOpen(),"Real SDL pointer movement opens the panel")) return false;
        const auto menu=geometry.refreshMenuRect(bar,window->pixelViewport());
        const auto i=static_cast<unsigned>(mode);
        const SDL_FPoint point{menu.x+menu.w/2,menu.y+(i+0.5f)*menu.h/5};
        event={};event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;event.button.windowID=window->id();event.button.button=SDL_BUTTON_LEFT;event.button.x=point.x;event.button.y=point.y;
        if (!sdl.handleEvent(event)) return false;
        event.type=SDL_EVENT_MOUSE_BUTTON_UP;if (!sdl.handleEvent(event) || !service(sdl)) return false;
        const std::string expected=mode==SdlRefreshMode::Server?"R":mode==SdlRefreshMode::Updates?"PU":mode==SdlRefreshMode::Combined?"PUR":"";
        if (!expect(order()==expected && remoteMouse==0 && !overlay->capturing(),
                    "Each hover button invokes its own method without remote input or underlying overlay capture")) return false;
        const auto selected = SdlTopBar::refreshButton(mode);
        if (!expect(window->selectedRefresh() == selected, "The chosen method becomes the icon action")) return false;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            window->closeRefreshMenu(); calls.clear();
            if (!window->fill()) return false;
            const auto target = SDL_GetRenderTarget(window->renderer());
            if (!target || !SDL_SetBooleanProperty(SDL_GetTextureProperties(target), "refresh.repeat.marker", true) ||
                !window->updateSurface() || !clickRefreshIcon(anchor)) return false;
            if (!expect(order() == expected && green(*window, true) && window->selectedRefresh() == selected && remoteMouse == 0,
                        "Icon clicks repeat the selected method after closing and reopening the panel")) return false;
            if (!window->fill()) return false;
            const bool retained = SDL_GetBooleanProperty(SDL_GetTextureProperties(SDL_GetRenderTarget(window->renderer())),
                                                        "refresh.repeat.marker", false);
            if (!expect(retained == (mode != SdlRefreshMode::Textures && mode != SdlRefreshMode::Combined),
                        "Repeating B/E rebuilds textures while A/C/D retain them") || !sdl.repaintAll()) return false;
        }
    }
    // Pressing one row and releasing elsewhere cancels locally and still closes the hover panel.
    window->closeRefreshMenu(); calls.clear();
    const auto barRect=window->topBarRect();
    const auto anchor=findButton(geometry,barRect,window->pixelViewport(),SdlTopBarButton::Refresh);
    SDL_Event move{};move.type=SDL_EVENT_MOUSE_MOTION;move.motion.windowID=window->id();move.motion.x=anchor.x;move.motion.y=anchor.y;
    if (!sdl.handleEvent(move)) return false;
    const auto menuRect=geometry.refreshMenuRect(barRect,window->pixelViewport());
    SDL_Event press{};press.type=SDL_EVENT_MOUSE_BUTTON_DOWN;press.button.windowID=window->id();press.button.button=SDL_BUTTON_LEFT;
    press.button.x=menuRect.x+menuRect.w/2;press.button.y=menuRect.y+menuRect.h*0.5f;
    if (!sdl.handleEvent(press)) return false;
    press.type=SDL_EVENT_MOUSE_BUTTON_UP;press.button.x=-10;press.button.y=-10;
    if (!sdl.handleEvent(press) || !service(sdl) || !expect(calls.empty() && remoteMouse==0 && window->selectedRefresh()==SdlTopBarButton::RefreshCombined,"Dragging off a popup row cancels without remote input or changing the selected method")) return false;
    SDL_Delay(350);sdl.serviceRefreshHover();
    if (!expect(!window->refreshMenuOpen(),"An outside release does not leave the popup stuck open")) return false;
    if (!sdl.handleEvent(move) || !expect(window->refreshMenuOpen(),"Hover reopens the popup after dismissal")) return false;
    SDL_Event escape{};escape.type=SDL_EVENT_KEY_DOWN;escape.key.windowID=window->id();escape.key.scancode=SDL_SCANCODE_ESCAPE;
    if (!sdl.handleEvent(escape)) return false;escape.type=SDL_EVENT_KEY_UP;
    if (!sdl.handleEvent(escape) || !expect(!window->refreshMenuOpen() && remoteKeys==0 && window->selectedRefresh()==SdlTopBarButton::RefreshCombined,"Escape closes the panel without remote input or changing the selected method")) return false;
    if (!expect(onConnectionThread,"All refresh control packets stay on the RDP thread")) return false;
    sdl.setConnected(false);return true;
}
// The panel is controlled only by the client's command line, never by the environment.
static bool parseRefreshMenu(std::vector<const char*> extra, bool initial, bool expected)
{
    SdlTopBar::setRefreshMenuEnabled(initial);
    std::unique_ptr<freerdp,decltype(&freerdp_free)> instance(freerdp_new(),freerdp_free);
    if (!instance) return false;
    instance->ContextSize=sizeof(sdl_rdp_context);
    if (!freerdp_context_new(instance.get())) return false;
    struct Guard { freerdp* value; ~Guard(){freerdp_context_free(value);} } guard{instance.get()};
    SdlContext sdl(instance->context);
    std::vector<char*> args{const_cast<char*>("sdl-freerdp"),const_cast<char*>("/v:refresh-test.invalid")};
    for (auto value : extra) args.push_back(const_cast<char*>(value));
    const auto status=freerdp_client_settings_parse_command_line_ex(instance->context->settings,
        static_cast<int>(args.size()),args.data(),FALSE,sdl.args(),sdl.argsCount(),&SdlContext::argumentHandler,&sdl);
    return expect(status==0 && SdlTopBar::refreshMenuEnabledByDefault()==expected,
                  "/sdl-refresh-menu controls the refresh method panel");
}
int main(int argc,char** argv)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy"); if (!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)) return 1;
    sdl_dialogs_init();
    // The former environment flag must have no effect.
    std::ignore = SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "FREERDP_SDL_REFRESH_MENU", "1", true);
    const std::string defaultPreview = argc > 1 ? std::string(argv[1]) + ".default.bmp" : "";
    const bool ok = expect(!SdlTopBar::refreshMenuEnabledByDefault(), "The refresh method panel is off by default") &&
        parseRefreshMenu({}, false, false) && parseRefreshMenu({"-sdl-refresh-menu"}, true, false) &&
        parseRefreshMenu({"/sdl-refresh-menu"}, false, true) && parseRefreshMenu({"+sdl-refresh-menu"}, false, true) &&
        (SdlTopBar::setRefreshMenuEnabled(false), run(false)) &&
        layoutAndPreview(defaultPreview.empty() ? nullptr : defaultPreview.c_str(), false) &&
        (SdlTopBar::setRefreshMenuEnabled(true), run(true)) &&
        layoutAndPreview(argc > 1 ? argv[1] : nullptr, true);
    sdl_dialogs_uninit();SDL_Quit();
    if (ok) puts("PASS default combined refresh, opt-in hover menu, remembered methods, and failure-safe recovery");
    return ok?0:1;
}
