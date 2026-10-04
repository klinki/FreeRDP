/**
 * Regression coverage for monitor selection, detection retries and physical display removal.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <algorithm>
#include <cstdio>
#include <memory>
#include <winpr/sysinfo.h>

#include "sdl_context.hpp"
#include "sdl_monitor.hpp"
#include "sdl_monitor_selection.hpp"
#include "sdl_types.hpp"
#include "dialogs/sdl_dialogs.hpp"

static bool expect(bool condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

static bool detect(SdlContext& sdl)
{
	UINT32 width = 0, height = 0;
	return sdl_detect_monitors(&sdl, &width, &height);
}

static bool hotplug(SdlContext& sdl, SDL_DisplayID display)
{
	auto settings = sdl.context()->settings;
	if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname,
	                                "monitor-regression.invalid") ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE))
		return false;
	// These changes can already be queued when SDL removes the display.
	constexpr SDL_DisplayID removed = 0xfffffff0;
	for (Uint32 type = SDL_EVENT_DISPLAY_FIRST; type <= SDL_EVENT_DISPLAY_LAST; type++)
	{
		if (type == SDL_EVENT_DISPLAY_REMOVED)
			continue;
		SDL_Event event{};
		event.display.type = static_cast<SDL_EventType>(type);
		event.display.displayID = removed;
		if (!expect(sdl.handleEvent(event), "queued event for a removed display is harmless"))
			return false;
	}

	// A display probe must not consume pending topology or input events.
	SDL_Event marker{};
	marker.type = SDL_EVENT_USER;
	marker.user.code = 42;
	if (!SDL_PushEvent(&marker))
		return false;
	std::ignore = SdlWindow::query(display);
	SDL_Event pending{};
	if (!expect(SDL_PeepEvents(&pending, 1, SDL_GETEVENT, SDL_EVENT_USER, SDL_EVENT_USER) == 1 &&
	            pending.user.code == 42, "display probe preserves the event queue"))
		return false;

	// The OS has moved both removed-monitor windows onto a live display.
	SDL_WindowID oldWindows[2]{};
	for (auto& id : oldWindows)
	{
		if (!expect(sdl.addDisplayWindow(display), "hotplug fixture window"))
			return false;
		auto window = sdl.getFirstWindow();
		// Find the newly created SDL window, not the previous fixture window.
		int count = 0;
		auto windows = SDL_GetWindows(&count);
		for (int x = 0; x < count; x++)
		{
			auto candidate = sdl.getWindowForId(SDL_GetWindowID(windows[x]));
			if (candidate && candidate->monitor(false).orig_screen == display)
				window = candidate;
		}
		SDL_free(windows);
		id = window->id();
		auto monitor = window->monitor(false);
		monitor.orig_screen = removed;
		window->setMonitor(monitor);
		if (!expect(window->displayIndex() == display, "removed window migrated to live display"))
			return false;
	}
	// SDL window IDs are independent of display IDs. Rediscovery must look up
	// the cached physical owner and tolerate an owner that has disappeared.
	if (!expect(sdl.detectDisplays(), "rediscovery with independent window/display IDs"))
		return false;
	for (const auto id : oldWindows)
	{
		if (!expect(sdl.getWindowForId(id)->monitor(false).orig_screen == removed,
		            "rediscovery preserves removed-window ownership"))
			return false;
	}
	SDL_Event removal{};
	removal.display.type = SDL_EVENT_DISPLAY_REMOVED;
	removal.display.displayID = removed;
	if (!expect(sdl.handleEvent(removal), "remove migrated windows through display channel"))
		return false;
	const auto now = GetTickCount64();
	sdl.getDisplayChannelContext().service(now);
	sdl.getDisplayChannelContext().service(now + 500);
	for (const auto id : oldWindows)
	{
		if (!expect(sdl.getWindowForId(id) == nullptr, "all windows owned by removed monitor released"))
			return false;
	}
	if (!expect(sdl.getFirstWindow() != nullptr &&
	            sdl.getFirstWindow()->monitor(false).orig_screen == display,
	            "last selected display removal falls back to remaining display"))
		return false;
	if (!expect(sdl.updateWindowList(), "fallback monitor layout"))
		return false;
	auto monitor = static_cast<const rdpMonitor*>(
	    freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray));
	return expect(freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) == 1 &&
	              monitor && monitor->is_primary && monitor->x == 0 && monitor->y == 0,
	              "fallback layout has one primary monitor at the origin");
}

static bool primarySelection(SdlContext& sdl)
{
	auto settings = sdl.context()->settings;
	const auto originalIds = sdl.monitorIds();
	const UINT32 selected[] = { 33, 22, 11 };
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, selected, 3))
		return false;
	sdl.setMonitorIds({ 33, 22, 11 });
	std::vector<rdpMonitor> monitors(3);
	for (size_t x = 0; x < monitors.size(); x++)
	{
		auto& monitor = monitors[x];
		monitor.orig_screen = static_cast<SDL_DisplayID>((x + 1) * 11);
		monitor.x = static_cast<INT32>(x * 1600);
		monitor.width = 1600;
		monitor.height = 900;
		monitor.is_primary = (x == 0); // The Mac's primary remains the left display.
		monitor.attributes.desktopScaleFactor = static_cast<UINT32>(100 + x * 50);
		monitor.attributes.deviceScaleFactor = 100;
	}
	const auto original = monitors;
	sdl.applyPrimaryMonitor(monitors);
	if (!expect(!monitors[0].is_primary && !monitors[1].is_primary && monitors[2].is_primary,
	            "first explicit selection overrides the Mac primary"))
		return false;
	if (!expect(monitors[2].x == original[2].x &&
	            monitors[2].attributes.desktopScaleFactor == 200,
	            "primary selection preserves monitor geometry and scale"))
		return false;
	if (!expect(freerdp_settings_set_monitor_def_array_sorted(settings, monitors.data(),
	                                                       monitors.size()),
	            "explicit primary layout is accepted by FreeRDP"))
		return false;
	auto sorted = static_cast<const rdpMonitor*>(
	    freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray));
	if (!expect(sorted && sorted[0].orig_screen == 33 && sorted[0].is_primary &&
	            sorted[0].x == 0 && sorted[0].y == 0 && sorted[1].orig_screen == 11 &&
	            sorted[1].x == -3200 && sorted[2].orig_screen == 22 && sorted[2].x == -1600,
	            "negotiated definitions retain physical identity after primary/position sorting"))
		return false;

	monitors.pop_back();
	sdl.applyPrimaryMonitor(monitors);
	if (!expect(!monitors[0].is_primary && monitors[1].is_primary,
	            "primary removal promotes the next surviving explicit selection"))
		return false;
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, nullptr, 0))
		return false;
	monitors = original;
	sdl.applyPrimaryMonitor(monitors);
	if (!expect(monitors[0].is_primary && !monitors[1].is_primary && !monitors[2].is_primary,
	            "automatic selection retains the Mac primary"))
		return false;
	for (auto& monitor : monitors)
		monitor.is_primary = false;
	sdl.applyPrimaryMonitor(monitors);
	if (!expect(monitors[0].is_primary, "missing primary falls back to one available display"))
		return false;
	for (auto& monitor : monitors)
		monitor.is_primary = true;
	sdl.applyPrimaryMonitor(monitors);
	if (!expect(std::count_if(monitors.cbegin(), monitors.cend(),
	                         [](const rdpMonitor& m) { return m.is_primary; }) == 1,
	            "layout always contains exactly one primary"))
		return false;
	monitors.clear();
	sdl.applyPrimaryMonitor(monitors);
	sdl.setMonitorIds(originalIds);
	return true;
}

static bool stableSelections()
{
	const std::vector<SdlMonitorSelection> wanted = {
		{ "4k-uuid", SdlMonitorScaleOverride{ 11, 175, 180 } }, { "fhd-uuid", std::nullopt }
	};
	auto mapped = sdl_resolve_monitor_selection(
	    wanted, { { "4K-UUID", 11 }, { "FHD-UUID", 22 }, { "laptop", 1 } }, 1);
	if (!expect(mapped.ids == std::vector<SDL_DisplayID>{ 11, 22 } &&
	                mapped.scales.at(11).desktopScaleFactor == 175,
	            "saved monitor order and explicit DPI are resolved case-insensitively"))
		return false;
	mapped = sdl_resolve_monitor_selection(wanted, { { "laptop", 1 } }, 1);
	if (!expect(mapped.ids == std::vector<SDL_DisplayID>{ 1 } && mapped.scales.empty(),
	            "all selected displays missing uses laptop without external DPI overrides"))
		return false;
	mapped = sdl_resolve_monitor_selection(wanted, { { "FHD-UUID", 44 }, { "laptop", 1 } }, 1);
	if (!expect(mapped.ids == std::vector<SDL_DisplayID>{ 44 } && mapped.scales.empty(),
	            "returning selected display replaces laptop fallback"))
		return false;
	mapped = sdl_resolve_monitor_selection(
	    wanted, { { "FHD-UUID", 44 }, { "laptop", 1 }, { "4K-UUID", 55 } }, 1);
	if (!expect(mapped.ids == std::vector<SDL_DisplayID>{ 55, 44 } &&
	                mapped.scales.count(11) == 0 && mapped.scales.at(55).displayId == 55 &&
	                mapped.scales.at(55).desktopScaleFactor == 175 &&
	                mapped.scales.at(55).deviceScaleFactor == 180,
	            "replugged SDL IDs restore original primary ordering and DPI by physical UUID"))
		return false;
	mapped = sdl_resolve_monitor_selection(
	    wanted, { { "4k-uuid", 55 }, { "4K-UUID", 66 }, { "laptop", 1 } }, 1);
	return expect(mapped.ids == std::vector<SDL_DisplayID>{ 1 } && mapped.scales.empty(),
	              "ambiguous identity cannot apply scaling to an unrelated display");
}

static bool run()
{
	std::unique_ptr<freerdp, decltype(&freerdp_free)> instance(freerdp_new(), freerdp_free);
	if (!expect(instance != nullptr, "instance allocation"))
		return false;
	instance->ContextSize = sizeof(sdl_rdp_context);
	if (!expect(freerdp_context_new(instance.get()), "context allocation"))
		return false;
	// The context must outlive the SDL wrapper below.
	struct ContextGuard
	{
		freerdp* instance;
		~ContextGuard() { freerdp_context_free(instance); }
	} contextGuard{ instance.get() };
	SdlContext sdl(instance->context);
	reinterpret_cast<sdl_rdp_context*>(instance->context)->sdl = &sdl;
	auto settings = instance->context->settings;
	if (!expect(sdl.detectDisplays(), "dummy display discovery"))
		return false;
	const auto displays = sdl.getDisplayIds();
	if (!expect(!displays.empty(), "dummy display available"))
		return false;

	for (const auto multimon : { FALSE, TRUE })
	{
		if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, multimon))
			return false;
		if (!expect(detect(sdl), "initial automatic monitor detection"))
			return false;
		if (!expect(freerdp_settings_get_uint32(settings, FreeRDP_NumMonitorIds) == 0,
		            "automatic detection must not populate the explicit-selection count"))
			return false;
		const auto selected = sdl.monitorIds();
		if (!expect(detect(sdl), "second automatic monitor detection") ||
		    !expect(sdl.monitorIds() == selected, "automatic selection preserved on retry"))
			return false;
	}

	if (!stableSelections() || !hotplug(sdl, displays.front()) || !primarySelection(sdl))
		return false;

	const UINT32 selected = displays.front();
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, &selected, 1))
		return false;
	if (!expect(detect(sdl) && detect(sdl), "explicit monitor selection survives retries") ||
	    !expect(sdl.monitorIds().size() == 1 && sdl.monitorIds().front() == selected,
	            "explicit selection retained"))
		return false;

	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, nullptr, 0) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_NumMonitorIds, 1))
		return false;
	if (!expect(!detect(sdl), "missing explicit ID array fails without aborting"))
		return false;
	return true;
}

int main()
{
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	sdl_dialogs_init();
	const bool success = run();
	sdl_dialogs_uninit();
	SDL_Quit();
	if (success)
		puts("PASS primary selection, monitor detection retries, stale events, migrated-window removal, and fallback layout");
	return success ? 0 : 1;
}
