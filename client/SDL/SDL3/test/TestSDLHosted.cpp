/** Hosted rendering mode switch: visibility and geometry, without an RDP server. */
#include "../sdl_hosted.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}

static bool hidden(SDL_Window* window)
{
	return (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0;
}

int main()
{
	try
	{
		require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
		const auto flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		auto window = SDL_CreateWindow("Sheltie hosted test", 640, 480, flags);
		require(window != nullptr, SDL_GetError());
		const auto id = SDL_GetWindowID(window);

		/* Off: nothing changes, and layouts are refused. */
		require(!SdlHosted::enabled(), "hosted mode is on by default");
		require(!SdlHosted::setLayout(id, { 1600, 1000, 2.0f }), "layout accepted while off");
		int w = 0, h = 0, sw = 0, sh = 0;
		require(SdlHosted::pixelSize(window, &w, &h) && SDL_GetWindowSizeInPixels(window, &sw, &sh),
		        SDL_GetError());
		require(w == sw && h == sh, "pixel size differs from SDL's while off");
		require(SdlHosted::pixelDensity(window) == SDL_GetWindowPixelDensity(window),
		        "density differs from SDL's while off");
		require(SdlHosted::show(window) && !hidden(window), "show did not show while off");
		require(SDL_HideWindow(window) && hidden(window), SDL_GetError());

		/* On: no request may put the window on screen. */
		SdlHosted::enable(true);
		require(SdlHosted::show(window) && hidden(window), "show revealed a hosted window");
		require(SdlHosted::raise(window) && hidden(window), "raise revealed a hosted window");
		require(SdlHosted::restore(window) && hidden(window), "restore revealed a hosted window");
		require(SdlHosted::minimize(window) && hidden(window), "minimize changed a hosted window");
		require(SdlHosted::fullscreen(window, true), "fullscreen request failed");
		require((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0 && hidden(window),
		        "a hosted window went fullscreen");
		require(SdlHosted::grabKeyboard(window, true) && !SDL_GetWindowKeyboardGrab(window),
		        "a hosted window grabbed the keyboard");
		require(SdlHosted::grabMouse(window, true) && !SDL_GetWindowMouseGrab(window),
		        "a hosted window grabbed the mouse");

		/* Without a layout the window's own geometry is used. */
		require(!SdlHosted::layout(id), "layout present before one was set");
		require(SdlHosted::pixelSize(window, &w, &h) && w == sw && h == sh,
		        "pixel size differs from SDL's without a layout");

		/* With a layout, size and scale come from the launcher, whatever the window says. */
		require(SdlHosted::setLayout(id, { 1600, 1000, 2.0f }), "valid layout refused");
		require(SdlHosted::pixelSize(window, &w, &h) && w == 1600 && h == 1000,
		        "pixel size is not the layout's");
		require(SdlHosted::pixelDensity(window) == 2.0f, "density is not the layout's");
		require(SdlHosted::displayScale(window) == 2.0f, "display scale is not the layout's");
		require(SdlHosted::setLayout(id, { 1280, 800, 1.0f }), "second layout refused");
		require(SdlHosted::pixelSize(window, &w, &h) && w == 1280 && h == 800 &&
		            SdlHosted::pixelDensity(window) == 1.0f,
		        "second layout not applied");

		/* Invalid layouts are refused and leave the last one in place. */
		require(!SdlHosted::setLayout(id, { 0, 800, 1.0f }), "zero width accepted");
		require(!SdlHosted::setLayout(id, { 1280, 20000, 1.0f }), "oversized height accepted");
		require(!SdlHosted::setLayout(id, { 1280, 800, 0.0f }), "zero scale accepted");
		require(!SdlHosted::setLayout(0, { 1280, 800, 1.0f }), "window 0 accepted");
		require(SdlHosted::pixelSize(window, &w, &h) && w == 1280 && h == 800,
		        "a refused layout changed the size");

		/* Other windows are unaffected; a forgotten window falls back to its own geometry. */
		auto other = SDL_CreateWindow("Sheltie hosted test 2", 320, 240, flags);
		require(other != nullptr, SDL_GetError());
		int ow = 0, oh = 0, osw = 0, osh = 0;
		require(SdlHosted::pixelSize(other, &ow, &oh) &&
		            SDL_GetWindowSizeInPixels(other, &osw, &osh) && ow == osw && oh == osh,
		        "a layout leaked to another window");
		SdlHosted::forget(id);
		require(!SdlHosted::layout(id), "layout survived forget");
		require(SdlHosted::pixelSize(window, &w, &h) && w == sw && h == sh,
		        "forgotten window kept the layout size");

		/* Off again: layouts are not consulted. */
		require(SdlHosted::setLayout(id, { 1600, 1000, 2.0f }), "layout refused");
		SdlHosted::enable(false);
		require(SdlHosted::pixelSize(window, &w, &h) && w == sw && h == sh,
		        "layout used while off");

		SDL_DestroyWindow(other);
		SDL_DestroyWindow(window);
		SDL_Quit();
		printf("Hosted mode switch: ok\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		fprintf(stderr, "TestSDLHosted failed: %s\n", error.what());
		return 1;
	}
}
