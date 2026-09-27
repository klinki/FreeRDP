/**
 * Regression coverage for repeated SDL keyboard setup during sign-in retries.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <memory>

#include <freerdp/locale/locale.h>

#include "sdl_context.hpp"
#include "sdl_types.hpp"

static bool expect(bool condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

static bool run(bool explicitLayout)
{
	std::unique_ptr<freerdp, decltype(&freerdp_free)> instance(freerdp_new(), freerdp_free);
	if (!expect(instance != nullptr, "instance allocation"))
		return false;
	instance->ContextSize = sizeof(sdl_rdp_context);
	if (!expect(freerdp_context_new(instance.get()), "context allocation"))
		return false;
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

	const UINT32 requested = explicitLayout ? GERMAN_STANDARD : 0;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, requested) ||
	    !freerdp_settings_set_string(settings, FreeRDP_KeyboardRemappingList, "0x1e=0x30"))
		return false;

	if (!explicitLayout)
	{
		// A failed first initialization must remain retryable after correcting settings.
		if (!freerdp_settings_set_string(settings, FreeRDP_KeyboardRemappingList, "invalid") ||
		    !expect(!instance->PreConnect(instance.get()), "invalid remapping fails gracefully") ||
		    !freerdp_settings_set_string(settings, FreeRDP_KeyboardRemappingList, "0x1e=0x30"))
			return false;
	}

	if (!expect(instance->PreConnect(instance.get()), "first connection setup"))
		return false;
	const auto layout = freerdp_settings_get_uint32(settings, FreeRDP_KeyboardLayout);
	if (!expect(layout != 0, "automatic keyboard layout resolved") ||
	    !expect(!explicitLayout || layout == requested, "explicit keyboard layout retained"))
		return false;

	for (int attempt = 0; attempt < 10; attempt++)
	{
		// FreeRDP disconnects a failed attempt before running PreConnect again.
		instance->PostDisconnect(instance.get());
		if (!expect(instance->PreConnect(instance.get()), "connection setup survives retry") ||
		    !expect(freerdp_settings_get_uint32(settings, FreeRDP_KeyboardLayout) == layout,
		            "keyboard layout retained on retry"))
			return false;
	}
	instance->PostFinalDisconnect(instance.get());
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
	const bool success = run(false) && run(true);
	SDL_Quit();
	if (success)
		puts("PASS repeated connection setup, automatic/explicit keyboard layout, invalid remapping recovery");
	return success ? 0 : 1;
}
