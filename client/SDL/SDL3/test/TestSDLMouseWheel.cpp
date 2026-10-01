/** Scroll direction regression tests. Copyright 2026 FreeRDP contributors.
 * Licensed under Apache-2.0.
 */
#include "../sdl_mouse_wheel.hpp"
#include <cstdio>
#include <initializer_list>

int main()
{
	// Physical wheels, trackpad/fractional input, simultaneous axes, and no movement.
	for (const auto input : { SDL_FPoint{ 0, 1 }, SDL_FPoint{ 1, 0 }, SDL_FPoint{ -2, 3 },
	                          SDL_FPoint{ 0.25f, -0.5f }, SDL_FPoint{ 0, 0 } })
	{
		for (const auto direction : { SDL_MOUSEWHEEL_NORMAL, SDL_MOUSEWHEEL_FLIPPED })
		{
			for (const bool local : { false, true })
			{
				SDL_MouseWheelEvent event{};
				event.x = input.x;
				event.y = input.y;
				event.direction = direction;
				const auto normal = sdl_mouse_wheel_delta(event, local, false);
				const auto reversed = sdl_mouse_wheel_delta(event, local, true);
				const auto restored = sdl_mouse_wheel_delta(event, local, false);
				const int expectedSign = direction == SDL_MOUSEWHEEL_FLIPPED && !local ? -1 : 1;
				if (normal.x != static_cast<int32_t>(input.x * 120 * expectedSign) ||
				    normal.y != static_cast<int32_t>(input.y * 120 * expectedSign) ||
				    reversed.x != -normal.x || reversed.y != -normal.y ||
				    restored.x != normal.x || restored.y != normal.y)
				{
					fprintf(stderr, "Mouse wheel direction or magnitude changed incorrectly\n");
					return 1;
				}
			}
		}
	}
	puts("PASS normal/reversed scrolling, both axes, natural scrolling preferences, fractional input");
	return 0;
}
