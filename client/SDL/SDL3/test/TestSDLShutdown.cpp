/**
 * Regression coverage for UI shutdown racing a core reconnect abort reset.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

#include "sdl_context.hpp"
#include "sdl_types.hpp"

static bool expect(bool condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
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
	} guard{ instance.get() };
	SdlContext sdl(instance->context);
	reinterpret_cast<sdl_rdp_context*>(instance->context)->sdl = &sdl;

	// Deterministically reproduce the reset performed by freerdp_reconnect
	// after the main thread has signalled the old abort event.
	const auto abort = freerdp_abort_event(instance->context);
	std::promise<bool> resetResult;
	auto result = resetResult.get_future();
	std::thread reconnect([&] {
		const bool woken = WaitForSingleObject(abort, 5000) == WAIT_OBJECT_0;
		resetResult.set_value(woken && ResetEvent(abort));
	});
	freerdp_set_last_error(instance->context, FREERDP_ERROR_PRE_CONNECT_FAILED);
	const bool stopped = sdl.requestStop();
	reconnect.join();
	if (!expect(stopped && result.get(), "shutdown wakes worker before reconnect resets abort") ||
	    !expect(!freerdp_shall_disconnect_context(instance->context), "core abort has been reset") ||
	    !expect(sdl.shallAbort(true), "UI shutdown remains permanent after abort reset") ||
	    !expect(freerdp_get_last_error(instance->context) == FREERDP_ERROR_PRE_CONNECT_FAILED,
	            "shutdown preserves original diagnostic") ||
	    !expect(instance->RetryDialog(instance.get(), "connection", 0, nullptr) < 0,
	            "no reconnect attempt or dialog after UI shutdown"))
		return false;

	// Starting a worker while shutdown wins the race must not start a new
	// connection, even though freerdp_connect would reset the abort again.
	if (!expect(sdl.start() == 0 && sdl.join() == 0, "worker stopped before connection starts") ||
	    !expect(sdl.join() == 0, "repeated stop is harmless"))
		return false;
	return true;
}

int main()
{
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1;
	const bool success = run();
	SDL_Quit();
	if (success)
		puts("PASS sticky shutdown across reconnect reset, retry cancellation, and worker start");
	return success ? 0 : 1;
}
