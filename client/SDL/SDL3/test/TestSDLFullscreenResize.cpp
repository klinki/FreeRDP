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
	if (displays.empty())
		return false;
	// Prefer the laptop for native runs even when external displays are attached.
	auto display = displays.front();
	for (const auto id : displays)
	{
		const auto name = SDL_GetDisplayName(id);
		if (name && SDL_strstr(name, "Built-in"))
			display = id;
	}
	printf("Fullscreen display: %s\n", SDL_GetDisplayName(display));
	if (!sdl.addDisplayWindow(display))
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
	// Windows ignores a layout sent right after the capabilities.
	const UINT64 activated = GetTickCount64();
	disp.service(activated);
	if (!expect(sentLayouts.empty(), "activation waits the minimum resize delay"))
		return false;
	disp.service(activated + 500);
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

	// A removed display's window may already have migrated to the laptop. Do not
	// replace its physical ownership/geometry until topology reconciliation.
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE))
		return false;
	const auto physical = window->monitor(false).orig_screen;
	constexpr SDL_DisplayID removed = 0xfffffff0;
	monitor.width = pixels.w + 64;
	monitor.height = pixels.h + 66;
	monitor.orig_screen = removed;
	window->setMonitor(monitor);
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(false).orig_screen == removed &&
	            window->monitor(false).width == monitor.width &&
	            window->monitor(false).height == monitor.height,
	            "migrated multimon window retains removed display ownership and geometry"))
		return false;

	// Two windows can coexist on the remaining display during a removal burst.
	// The live display's own window must still refresh while the migrated one waits.
	if (!sdl.addDisplayWindow(physical))
		return false;
	int count = 0;
	auto windows = SDL_GetWindows(&count);
	SDL_WindowID second = 0;
	for (int x = 0; x < count; x++)
	{
		const auto id = SDL_GetWindowID(windows[x]);
		if (id != window->id() && sdl.getWindowForId(id))
			second = id;
	}
	SDL_free(windows);
	if (!expect(second != 0, "second multimon window created"))
		return false;
	monitor.orig_screen = physical;
	monitor.x = -pixels.w;
	monitor.y = 120;
	window->setMonitor(monitor);
	auto secondMonitor = sdl.getWindowForId(second)->monitor(false);
	secondMonitor.orig_screen = removed;
	sdl.getWindowForId(second)->setMonitor(secondMonitor);
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(false).width == window->rect().w &&
	            window->monitor(false).height == window->rect().h &&
	            window->monitor(false).x == monitor.x &&
	            window->monitor(false).y == monitor.y &&
	            window->monitor(false).attributes.desktopScaleFactor == 175 &&
	            window->monitor(false).attributes.deviceScaleFactor == 140,
	            "owning multimon window uses drawable pixels while retaining origin and scaling") ||
	    !expect(sdl.removeDisplayWindow(removed) && sdl.getWindowForId(second) == nullptr,
	            "removed display window released before fallback sizing"))
		return false;

	// Recreate another owned window (also works on the one-display dummy driver).
	// Distinct origins and DPI catch accidentally promoting each window to primary.
	monitor = window->monitor(false);
	monitor.orig_screen = removed;
	window->setMonitor(monitor);
	if (!sdl.addDisplayWindow(physical))
		return false;
	windows = SDL_GetWindows(&count);
	for (int x = 0; x < count; x++)
	{
		const auto id = SDL_GetWindowID(windows[x]);
		if (id != window->id() && sdl.getWindowForId(id))
			second = id;
	}
	SDL_free(windows);
	auto other = sdl.getWindowForId(second);
	if (!other)
		return false;
	std::ignore = SDL_SyncWindow(other->window());
	monitor.orig_screen = physical;
	monitor.is_primary = true;
	window->setMonitor(monitor);
	secondMonitor = other->monitor(false);
	secondMonitor.x = 0;
	secondMonitor.y = 0;
	secondMonitor.is_primary = false;
	secondMonitor.width = other->rect().w + 32;
	secondMonitor.height = other->rect().h + 66;
	secondMonitor.attributes.desktopScaleFactor = 100;
	secondMonitor.attributes.deviceScaleFactor = 100;
	other->setMonitor(secondMonitor);
	resized.windowID = other->id();
	const auto beforeMultimon = layoutCalls;
	if (!disp.handleEvent(resized))
		return false;
	disp.service(activated + 1000);
	if (!expect(layoutCalls == beforeMultimon + 1 && sentLayouts.size() == 2,
	            "pixel-size event sends both multimon layout entries") ||
	    !expect(window->monitor(false).x == monitor.x && window->monitor(false).y == monitor.y &&
	                other->monitor(false).x == secondMonitor.x && other->monitor(false).y == secondMonitor.y &&
	                other->monitor(false).width == other->rect().w &&
	                other->monitor(false).height == other->rect().h,
	            "each owned fullscreen window retains its origin and uses its own drawable"))
		return false;
	for (const auto& layout : sentLayouts)
	{
		const bool primary = layout.Flags == DISPLAY_CONTROL_MONITOR_PRIMARY;
		if (!expect(layout.Width == static_cast<UINT32>(primary ? window->rect().w : other->rect().w) &&
		                layout.Height == static_cast<UINT32>(primary ? window->rect().h : other->rect().h) &&
		                layout.DesktopScaleFactor == (primary ? 175u : 100u) &&
		                layout.DeviceScaleFactor == (primary ? 140u : 100u) &&
		                layout.Left == (primary ? 0 : pixels.w) &&
		                layout.Top == (primary ? 0 : -120),
		            "wire layout preserves relative monitor positions, primary and per-monitor DPI"))
			return false;
	}
	if (!disp.handleEvent(resized))
		return false;
	disp.service(activated + 2000);
	if (!expect(layoutCalls == beforeMultimon + 1, "duplicate multimon size events are deduplicated"))
		return false;
	secondMonitor = other->monitor(false);
	secondMonitor.orig_screen = removed;
	other->setMonitor(secondMonitor);
	if (!sdl.removeDisplayWindow(removed))
		return false;

	std::ignore = SDL_SyncWindow(window->window());
	const auto fallbackPixels = window->rect();
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(false).width == fallbackPixels.w &&
	            window->monitor(false).height == fallbackPixels.h &&
	            window->monitor(false).attributes.physicalWidth == monitor.attributes.physicalWidth &&
	            window->monitor(false).attributes.physicalHeight == monitor.attributes.physicalHeight &&
	            window->monitor(false).attributes.desktopScaleFactor == 175 &&
	            window->monitor(false).attributes.deviceScaleFactor == 140,
	            "single remaining fullscreen multimon window uses drawable pixels and retains DPI"))
		return false;
	// Replug/layout changes refresh cached multimon dimensions and restore DPI.
	SdlMonitorScaleOverrides scales{ { physical, { physical, 175, 180 } } };
	if (!sdl.setHotplugMonitorSelection({ physical }, scales))
		return false;
	SDL_DisplayEvent changed{};
	changed.type = SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED;
	changed.displayID = physical;
	const auto before = layoutCalls;
	// Continue the simulated clock after the activation send.
	const UINT64 now = activated + 3000;
	if (!disp.handleEvent(changed))
		return false;
	disp.service(now);
	if (!expect(layoutCalls == before, "dock event bursts wait for stable geometry"))
		return false;
	disp.service(now + 500);
	const auto fresh = window->rect();
	if (!expect(layoutCalls == before + 1 &&
	                sentLayouts.front().Width == static_cast<UINT32>(fresh.w) &&
	                sentLayouts.front().Height == static_cast<UINT32>(fresh.h) &&
	                sentLayouts.front().DesktopScaleFactor == 175 &&
	                sentLayouts.front().DeviceScaleFactor == 180,
	            "settled hotplug sends fullscreen fallback drawable dimensions with saved scaling"))
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

#if SDL_VERSION_ATLEAST(3, 3, 2)
	// Focus/work-area notifications must refresh inventory without reconfiguring
	// the server when the effective remote layout has not changed.
	SDL_DisplayEvent bounds{};
	bounds.type = SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED;
	bounds.displayID = physical;
	const auto beforeBounds = layoutCalls;
	if (!disp.handleEvent(bounds))
		return false;
	disp.service(now + 2200);
	if (!disp.handleEvent(bounds))
		return false;
	disp.service(now + 2300);
	disp.service(now + 2799);
	if (!expect(layoutCalls == beforeBounds, "usable-bounds bursts retain the settling delay"))
		return false;
	for (UINT64 offset = 2800; offset <= 7800; offset += 1000)
	{
		disp.service(now + offset);
		if (!expect(layoutCalls == beforeBounds,
		            "unchanged usable bounds and settling retries do not resend the layout"))
			return false;
	}

	// A real effective-layout change must still be discovered by this event.
	const SdlMonitorScaleOverrides newScales{ { physical, { physical, 150, 140 } } };
	if (!sdl.setHotplugMonitorSelection({ physical }, newScales) || !disp.handleEvent(bounds))
		return false;
	disp.service(now + 8000);
	if (!expect(layoutCalls == beforeBounds, "changed usable bounds still wait for settling"))
		return false;
	disp.service(now + 8500);
	if (!expect(layoutCalls == beforeBounds + 1 && sentLayouts.size() == 1 &&
	                sentLayouts.front().Width == static_cast<UINT32>(fresh.w) &&
	                sentLayouts.front().Height == static_cast<UINT32>(fresh.h) &&
	                sentLayouts.front().DesktopScaleFactor == 150 &&
	                sentLayouts.front().DeviceScaleFactor == 140,
	            "usable-bounds refresh sends changed effective scaling with drawable dimensions"))
		return false;
	disp.service(now + 9500);
	if (!expect(layoutCalls == beforeBounds + 1, "changed usable bounds send only one layout"))
		return false;

	// Test both queue orders, and a bounds notification after the force request
	// has already been consumed by service. Neither may cancel reconnect rescaling.
	UINT64 burstTime = now + 10000;
	for (const bool reconnectFirst : { false, true })
	{
		const auto beforeBurst = layoutCalls;
		if (!disp.handleEvent(reconnectFirst ? changed : bounds) ||
		    !disp.handleEvent(reconnectFirst ? bounds : changed))
			return false;
		disp.service(burstTime);
		if (!disp.handleEvent(bounds))
			return false;
		disp.service(burstTime + 100);
		disp.service(burstTime + 599);
		if (!expect(layoutCalls == beforeBurst, "mixed reconnect/bounds bursts wait for settling"))
			return false;
		disp.service(burstTime + 600);
		if (!expect(layoutCalls == beforeBurst + 1 &&
		                sentLayouts.front().DesktopScaleFactor == 150 &&
		                sentLayouts.front().DeviceScaleFactor == 140,
		            "usable-bounds events preserve forced same-layout reconnect rescaling"))
			return false;
		disp.service(burstTime + 1600);
		if (!expect(layoutCalls == beforeBurst + 1, "mixed-burst settling retries deduplicate layouts"))
			return false;
		burstTime += 3000;
	}
	const auto beforeActivation = layoutCalls;
	if (!expect(channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK &&
	                disp.handleEvent(bounds),
	            "display-channel reactivation can coalesce with a work-area notification"))
		return false;
	disp.service(burstTime);
	disp.service(burstTime + 500);
	if (!expect(layoutCalls == beforeActivation + 1,
	            "usable-bounds events preserve forced same-layout channel activation"))
		return false;
#endif
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
		puts("PASS fullscreen drawable dimensions, delayed activation, per-window multimon layouts, fallback, scaling, and migration geometry");
	return success ? 0 : 1;
}
