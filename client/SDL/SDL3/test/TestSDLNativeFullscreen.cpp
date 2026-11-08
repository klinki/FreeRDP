/**
 * Regression coverage for issue sheltie-app/sheltie-macos#26 with real SDL
 * windows: leaving fullscreen with the session toggle and re-entering through
 * a control the engine did not request must restore the connection bar, keep
 * the pin state and leave the next toggle in the right direction. Covers one
 * window and two windows with only one of them re-entered.
 *
 * The "native" re-entry calls SDL_SetWindowFullscreen directly, bypassing
 * SdlContext, so the engine only learns of it from the window event, as it
 * does for the macOS green button. It is not a click on the green button.
 *
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <functional>
#include <memory>

#include "sdl_context.hpp"
#include "sdl_types.hpp"
#include "sdl_utils.hpp"

static bool expect(bool condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

/* Run the parts of the client main loop this test needs until `done`:
 * queued fullscreen requests and native fullscreen events. */
static bool pump(SdlContext& sdl, const std::function<bool()>& done, Uint64 timeoutMs = 10000)
{
	const auto deadline = SDL_GetTicks() + timeoutMs;
	while (SDL_GetTicks() < deadline)
	{
		SDL_Event ev{};
		while (SDL_PollEvent(&ev))
		{
			if (ev.type == SDL_EVENT_USER_WINDOW_FULLSCREEN)
			{
				const auto id =
				    static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(ev.user.data1));
				const bool enter = ev.user.code != 0;
				const bool force = ev.user.data2 != nullptr;
				if (id == 0 ? !sdl.setFullscreen(enter, force)
				            : !sdl.applyFullscreen(id, enter, force))
					return false;
				continue;
			}
			/* Other window events reach input handling that needs a
			 * connected client. */
			if ((ev.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
			     ev.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) &&
			    !sdl.handleEvent(ev))
				return false;
		}
		if (done())
			return true;
		SDL_Delay(10);
	}
	return done();
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
	if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "native-fullscreen.invalid") ||
	    !freerdp_settings_set_bool(settings, FreeRDP_Fullscreen, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE) || !sdl.detectDisplays())
		return false;
	const auto displays = sdl.getDisplayIds();
	if (displays.empty())
		return false;
	const auto display = displays.front();
	const auto& fs = sdl.fullscreenState();

	/* addDisplayWindow creates and shows a fullscreen window, like a display
	 * joining a session. Its enter event is the first native observation. */
	if (!sdl.addDisplayWindow(display))
		return false;
	auto window = sdl.getFirstWindow();
	if (!window || !window->window())
		return false;
	const auto id = window->id();
	if (!expect(fs.barOwner == id, "first window owns the bar") ||
	    !expect(pump(sdl, [&] { return fs.windowFullscreen(id); }),
	            "window reports fullscreen from its enter event") ||
	    !expect(fs.barShown(id, false), "bar shown in fullscreen") ||
	    !expect(fs.barPinned, "bar pinned by default"))
		return false;

	/* FreeRDP's toggle (Right Shift+Return or the bar's restore button). */
	if (!expect(sdl.toggleFullscreen(id), "session toggle queued") ||
	    !expect(pump(sdl, [&] { return !fs.windowFullscreen(id) && !fs.anyPending(SDL_GetTicks()); }),
	            "toggle leaves fullscreen") ||
	    !expect(!fs.barShown(id, false), "bar hidden after leaving") ||
	    !expect(fs.barPinned, "leaving keeps the pin"))
		return false;

	/* Re-entry the engine did not request. */
	if (!expect(SDL_SetWindowFullscreen(window->window(), true), "native enter accepted") ||
	    !expect(pump(sdl, [&] { return fs.windowFullscreen(id); }), "native enter observed") ||
	    !expect(fs.barShown(id, false), "bar restored after native re-entry") ||
	    !expect(fs.barPinned, "native re-entry keeps the pin") ||
	    !expect(sdl.fullscreen(), "session reports fullscreen after native re-entry"))
		return false;

	/* The same with an unpinned bar. */
	if (!sdl.toggleFullscreen(id) ||
	    !pump(sdl, [&] { return !fs.windowFullscreen(id) && !fs.anyPending(SDL_GetTicks()); }))
		return false;
	auto& mutableState = const_cast<SdlFullscreenState&>(fs);
	mutableState.barPinned = false;
	if (!expect(SDL_SetWindowFullscreen(window->window(), true), "native enter accepted") ||
	    !expect(pump(sdl, [&] { return fs.windowFullscreen(id); }), "native enter observed") ||
	    !expect(fs.barShown(id, false), "unpinned bar shown after native re-entry") ||
	    !expect(!fs.barPinned, "unpinned state kept"))
		return false;
	mutableState.barPinned = true;

	/* Two windows, only the second leaves natively: the bar stays with its
	 * fullscreen owner and a toggle from the windowed one re-enters all. */
	/* Prefer a second physical display. Two fullscreen windows cannot share
	 * one real display (macOS takes the first out of fullscreen), so with a
	 * single real display the mixed-monitor part is skipped. The dummy driver
	 * has one display; tag the first window with another one there. */
	SDL_DisplayID ownerDisplay = display;
	SDL_DisplayID secondDisplay = 0;
	if (displays.size() > 1)
		secondDisplay = displays[1];
	else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0)
	{
		ownerDisplay = 0xfffffff0;
		auto monitor = window->monitor(false);
		monitor.orig_screen = ownerDisplay;
		window->setMonitor(monitor);
		secondDisplay = display;
	}
	else
	{
		printf("SKIP mixed monitors: needs a second display\n");
		return sdl.toggleFullscreen(id) &&
		       expect(pump(sdl, [&] { return !fs.anyFullscreen() && !fs.anyPending(SDL_GetTicks()); }),
		              "final toggle leaves every window");
	}
	printf("Second display: %s\n", SDL_GetDisplayName(secondDisplay));
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE) ||
	    !sdl.addDisplayWindow(secondDisplay))
		return false;
	SDL_WindowID second = 0;
	int count = 0;
	auto windows = SDL_GetWindows(&count);
	for (int x = 0; x < count; x++)
	{
		const auto other = SDL_GetWindowID(windows[x]);
		if (other != id && sdl.getWindowForId(other))
			second = other;
	}
	SDL_free(windows);
	if (!expect(second != 0, "second window created"))
		return false;
	if (!pump(sdl, [&] { return fs.windowFullscreen(second); }, 3000))
	{
		const auto w = sdl.getWindowForId(second)->window();
		fprintf(stderr, "DIAG second flags=0x%llx display=%u (want %u) hidden=%d\n",
		        static_cast<unsigned long long>(SDL_GetWindowFlags(w)), SDL_GetDisplayForWindow(w),
		        secondDisplay, (SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN) != 0);
		return false;
	}
	auto secondWindow = sdl.getWindowForId(second);
	if (!expect(SDL_SetWindowFullscreen(secondWindow->window(), false), "native leave accepted") ||
	    !expect(pump(sdl, [&] { return !fs.windowFullscreen(second); }), "native leave observed") ||
	    !expect(fs.barOwner == id && fs.barShown(id, true), "bar stays on its fullscreen owner") ||
	    !expect(!sdl.fullscreen(), "mixed session is not fullscreen") ||
	    !expect(sdl.toggleFullscreen(second), "toggle from the windowed monitor queued") ||
	    !expect(pump(sdl, [&] { return fs.allFullscreen() && !fs.anyPending(SDL_GetTicks()); }),
	            "toggle from the windowed monitor re-enters the whole session") ||
	    !expect(fs.barShown(id, true), "bar still shown"))
		return false;

	/* The bar's window disappears with its display: the bar moves to the
	 * remaining window and follows that window's actual state. */
	if (!expect(sdl.removeDisplayWindow(ownerDisplay) && !sdl.getWindowForId(id),
	            "owner window removed with its display") ||
	    !expect(fs.barOwner == second, "bar moves to the remaining window") ||
	    !expect(fs.barShown(second, true), "bar shown on the fullscreen remaining window"))
		return false;
	const auto remaining = second;

	/* Leave everything so the test exits windowed. */
	if (!sdl.toggleFullscreen(remaining) ||
	    !expect(pump(sdl, [&] { return !fs.anyFullscreen() && !fs.anyPending(SDL_GetTicks()); }),
	            "final toggle leaves every window"))
		return false;
	return true;
}

/* Without arguments the dummy video driver is used, which completes each
 * transition synchronously. Pass any argument to use the real display, where
 * transitions are native and asynchronous (macOS Spaces). */
int main(int argc, char**)
{
	if (argc == 1)
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1;
	printf("Video driver: %s\n", SDL_GetCurrentVideoDriver());
	const bool ok = run();
	SDL_Quit();
	if (ok)
		printf("TestSDLNativeFullscreen: passed\n");
	return ok ? 0 : 1;
}
