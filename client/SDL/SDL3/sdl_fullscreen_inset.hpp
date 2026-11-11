/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client
 *
 * Copyright 2026 FreeRDP contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <SDL3/SDL.h>

/**
 * The pixel rows the system keeps above a fullscreen window's content on a display, 0 when it
 * keeps none. On a notched Mac, fullscreen content starts below the band that holds the camera
 * housing and the menu bar, so the window is that much shorter than the display.
 *
 * Call it on the main thread, from where displays are detected.
 */
#if defined(__APPLE__)
[[nodiscard]] int sdl_fullscreen_top_inset(SDL_DisplayID id);
#else
[[nodiscard]] inline int sdl_fullscreen_top_inset([[maybe_unused]] SDL_DisplayID id)
{
	return 0;
}
#endif
