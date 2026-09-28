/**
 * Regression coverage for monitor detection retries and physical display removal.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <memory>

#include "sdl_context.hpp"
#include "sdl_monitor.hpp"
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
	SDL_Event removal{};
	removal.display.type = SDL_EVENT_DISPLAY_REMOVED;
	removal.display.displayID = removed;
	if (!expect(sdl.handleEvent(removal), "remove migrated windows through display channel"))
		return false;
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

	if (!hotplug(sdl, displays.front()))
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
		puts("PASS monitor detection retries, stale events, migrated-window removal, and fallback layout");
	return success ? 0 : 1;
}
