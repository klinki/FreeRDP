/** Per-session scroll direction mapping. Copyright 2026 FreeRDP contributors.
 * Licensed under Apache-2.0.
 */
#pragma once

#include <cstdint>
#include <SDL3/SDL.h>

struct SdlWheelDelta
{
	int32_t x;
	int32_t y;
};

// Reverse the final direction, after applying the existing SDL/local preference.
// Preserve the RDP wheel step size and fractional input on both axes.
[[nodiscard]] inline SdlWheelDelta sdl_mouse_wheel_delta(const SDL_MouseWheelEvent& event,
                                                       bool useLocalDirection, bool reverse)
{
	const bool flipped = ((event.direction == SDL_MOUSEWHEEL_FLIPPED) && !useLocalDirection) != reverse;
	const float scale = (flipped ? -1.0f : 1.0f) * 120.0f;
	return { static_cast<int32_t>(event.x * scale), static_cast<int32_t>(event.y * scale) };
}
