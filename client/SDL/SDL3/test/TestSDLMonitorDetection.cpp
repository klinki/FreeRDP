/**
 * Regression coverage for repeated SDL monitor detection during sign-in retries.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <memory>

#include "sdl_context.hpp"
#include "sdl_monitor.hpp"
#include "sdl_types.hpp"

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
	const bool success = run();
	SDL_Quit();
	if (success)
		puts("PASS repeated automatic/explicit monitor detection and malformed selection");
	return success ? 0 : 1;
}
