/** Windowed geometry restoration without an RDP server or real credentials. */
#include "../sdl_window_geometry.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* message)
{
	if (!value) throw std::runtime_error(message);
}

int main()
{
	try
	{
		require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
		auto window = SDL_CreateWindow("Sheltie window size test", 640, 480, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		require(window != nullptr, SDL_GetError());
		SdlWindowGeometry geometry;
		geometry.initialize(window, {800, 600});
		require(SDL_ShowWindow(window) && SDL_SyncWindow(window), SDL_GetError());
		auto size = geometry.observe(window, SDL_EVENT_WINDOW_SHOWN);
		require(size && size->x == 800 && size->y == 600, "restored logical size differs from saved size");
		int pixelWidth = 0, pixelHeight = 0;
		std::ignore = SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
		printf("Restored content: %dx%d logical, %dx%d pixels\n", size->x, size->y, pixelWidth, pixelHeight);

		require(SDL_SetWindowSize(window, 900, 650) && SDL_SyncWindow(window), SDL_GetError());
		size = geometry.observe(window, SDL_EVENT_WINDOW_RESIZED);
		require(size && size->x == 900 && size->y == 650, "user resize not remembered");
		// Simulated transition events exercise ordering without switching the user's Space.
		require(!geometry.observe(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN), "fullscreen reported a windowed size");
		require(SDL_SetWindowSize(window, 1000, 700) && SDL_SyncWindow(window), SDL_GetError());
		require(!geometry.observe(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED), "fullscreen dimensions replaced normal size");
		size = geometry.observe(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN);
		require(size && size->x == 900 && size->y == 650, "fullscreen transition lost last windowed size");
		require(!geometry.observe(window, SDL_EVENT_WINDOW_MAXIMIZED), "maximized size was reported");
		require(!geometry.observe(window, SDL_EVENT_WINDOW_RESIZED), "resize during maximize was reported");
		require(geometry.observe(window, SDL_EVENT_WINDOW_RESTORED).has_value(), "normal window was not tracked after restore");
		require(!geometry.observe(window, SDL_EVENT_WINDOW_MINIMIZED), "minimized size was reported");
		const auto clamped = SdlWindowGeometry::fit({2000, 1600}, {0, 0, 1200, 800}, 30, 2, 2, 2);
		require(clamped.x == 1196 && clamped.y == 768, "saved size did not fit usable display including borders");
		geometry.initialize(window, {16000, 16000});
		SDL_Rect usable{};
		require(SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable), SDL_GetError());
		const auto fitted = geometry.normalSize();
		require(fitted.x <= usable.w && fitted.y <= usable.h, "restore overflowed current display");
		SDL_DestroyWindow(window);
		SDL_Quit();
		puts("Window geometry restore, resize, fullscreen, maximize, minimize, and fit tests passed");
		return 0;
	}
	catch (const std::exception& error)
	{
		fprintf(stderr, "%s\n", error.what());
		SDL_Quit();
		return 1;
	}
}
