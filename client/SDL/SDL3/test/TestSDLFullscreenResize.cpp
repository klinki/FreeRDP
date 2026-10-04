/**
 * Regression coverage for fullscreen drawable size and delayed display-channel activation.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <memory>
#include <winpr/sysinfo.h>
#include <vector>

#include "sdl_context.hpp"
#include "sdl_types.hpp"
#include "dialogs/sdl_dialogs.hpp"

static std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> sentLayouts;
static unsigned layoutCalls = 0;

static bool expect(bool condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

static UINT captureLayout(DispClientContext*, UINT32 count,
                          DISPLAY_CONTROL_MONITOR_LAYOUT* layouts)
{
	if (!expect(SDL_IsMainThread(), "Display Control sends stay on SDL main thread"))
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;
	layoutCalls++;
	sentLayouts.assign(layouts, layouts + count);
	return CHANNEL_RC_OK;
}

static bool run()
{
	std::unique_ptr<freerdp, decltype(&freerdp_free)> instance(freerdp_new(), freerdp_free);
	if (!instance)
		return false;
	instance->ContextSize = sizeof(sdl_rdp_context);
	if (!freerdp_context_new(instance.get()))
		return false;
	struct ContextGuard
	{
		freerdp* instance;
		~ContextGuard() { freerdp_context_free(instance); }
	} contextGuard{ instance.get() };
	SdlContext sdl(instance->context);
	reinterpret_cast<sdl_rdp_context*>(instance->context)->sdl = &sdl;
	auto settings = instance->context->settings;
	if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "fullscreen-test.invalid") ||
	    !freerdp_settings_set_bool(settings, FreeRDP_Fullscreen, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE) ||
	    !sdl.detectDisplays())
		return false;
	const auto displays = sdl.getDisplayIds();
	if (displays.empty() || !sdl.addDisplayWindow(displays.front()))
		return false;
	auto window = sdl.getFirstWindow();
	if (!window || !window->window())
		return false;
	std::ignore = SDL_SyncWindow(window->window());
	const auto pixels = window->rect();
	printf("Fullscreen drawable: %dx%d pixels\n", pixels.w, pixels.h);
	if (!expect(pixels.w > 0 && pixels.h > 0, "drawable pixel dimensions available"))
		return false;

	// The initial full-display probe includes space that the actual fullscreen
	// content can exclude on a notched Mac. Seed an oversized cached definition.
	auto monitor = window->monitor(true);
	monitor.width = pixels.w + 64;
	monitor.height = pixels.h + 66;
	monitor.attributes.desktopScaleFactor = 175;
	monitor.attributes.deviceScaleFactor = 140;
	window->setMonitor(monitor);
	if (!sdl.updateWindowList())
		return false;

	DispClientContext channel{};
	channel.SendMonitorLayout = captureLayout;
	auto& disp = sdl.getDisplayChannelContext();
	if (!disp.init(&channel))
		return false;
	SDL_WindowEvent resized{};
	resized.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
	resized.windowID = window->id();
	if (!expect(disp.handleEvent(resized), "fullscreen pixel-size event handled"))
		return false;
	monitor = window->monitor(true);
	if (!expect(monitor.width == pixels.w && monitor.height == pixels.h,
	            "fullscreen uses drawable pixels instead of initial full-display bounds") ||
	    !expect(monitor.attributes.desktopScaleFactor == 175 &&
	            monitor.attributes.deviceScaleFactor == 140,
	            "fullscreen resize preserves scale overrides") ||
	    !expect(sentLayouts.empty(), "no layout sent before display-channel capabilities"))
		return false;

	if (!expect(channel.DisplayControlCaps != nullptr &&
	            channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "delayed fullscreen display-channel activation succeeds"))
		return false;
	disp.service();
	if (!expect(sentLayouts.size() == 1 && sentLayouts.front().Width == static_cast<UINT32>(pixels.w) &&
	            sentLayouts.front().Height == static_cast<UINT32>(pixels.h),
	            "activation sends the corrected fullscreen layout without another resize") ||
	    !expect(sentLayouts.front().Left == 0 && sentLayouts.front().Top == 0 &&
	            sentLayouts.front().Flags == DISPLAY_CONTROL_MONITOR_PRIMARY &&
	            sentLayouts.front().DesktopScaleFactor == 175 &&
	            sentLayouts.front().DeviceScaleFactor == 140,
	            "corrected layout retains primary origin and effective scaling"))
		return false;
	if (!expect(disp.handleEvent(resized) && layoutCalls == 1,
	            "repeated same-size events do not resend the layout"))
		return false;

	// Multimon keeps physical display ownership and geometry during migration.
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE))
		return false;
	monitor.width = pixels.w + 64;
	monitor.height = pixels.h + 66;
	window->setMonitor(monitor);
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(true).width == monitor.width &&
	            window->monitor(true).height == monitor.height,
	            "multimon retains cached physical display geometry"))
		return false;
	// Replug/layout changes refresh cached multimon dimensions and restore DPI.
	const auto physical = window->monitor(false).orig_screen;
	const std::vector<SDL_DisplayID> selected{ physical };
	const auto scale = std::to_string(physical) + "=175/180";
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, selected.data(),
	                                      selected.size()) ||
	    !sdl.parseMonitorScaleOverrides(scale.c_str()))
		return false;
	sdl.setMonitorIds(selected);
	SDL_DisplayEvent changed{};
	changed.type = SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED;
	changed.displayID = physical;
	const auto before = layoutCalls;
	const UINT64 now = GetTickCount64();
	if (!disp.handleEvent(changed))
		return false;
	disp.service(now);
	if (!expect(layoutCalls == before, "dock event bursts wait for stable geometry"))
		return false;
	disp.service(now + 500);
	const auto fresh = SdlWindow::query(physical, false);
	if (!expect(layoutCalls == before + 1 &&
	                sentLayouts.front().Width == static_cast<UINT32>(fresh.width) &&
	                sentLayouts.front().Height == static_cast<UINT32>(fresh.height) &&
	                sentLayouts.front().DesktopScaleFactor == 175 &&
	                sentLayouts.front().DeviceScaleFactor == 180,
	            "settled hotplug resends fresh dimensions with saved scaling"))
		return false;
	disp.service(now + 1500);
	if (!expect(layoutCalls == before + 1, "settling retries deduplicate unchanged layouts"))
		return false;
	changed.type = SDL_EVENT_DISPLAY_ADDED;
	if (!disp.handleEvent(changed))
		return false;
	disp.service(now + 1600);
	disp.service(now + 2100);
	if (!expect(layoutCalls == before + 2,
	            "reconnection triggers rescaling even when final layout matches the previous one"))
		return false;
	return disp.uninit(&channel);
}

int main(int argc, char**)
{
	if (argc == 1)
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1;
	sdl_dialogs_init();
	const bool success = run();
	sdl_dialogs_uninit();
	SDL_Quit();
	if (success)
		puts("PASS fullscreen drawable dimensions, delayed activation, scale preservation, and multimon geometry");
	return success ? 0 : 1;
}
