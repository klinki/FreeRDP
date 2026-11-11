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
#include "sdl_fullscreen_inset.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#import <AppKit/AppKit.h>

int sdl_fullscreen_top_inset(SDL_DisplayID id)
{
	const char* driver = SDL_GetCurrentVideoDriver();
	if (!driver || (std::strcmp(driver, "cocoa") != 0))
		return 0;

	SDL_Rect bounds = {};
	if (!SDL_GetDisplayBounds(id, &bounds))
		return 0;

	@autoreleasepool
	{
		NSArray<NSScreen*>* screens = [NSScreen screens];
		if (screens.count == 0)
			return 0;
		/* SDL display bounds are in points, with the origin at the top left of the main screen;
		 * AppKit frames have it at the bottom left. */
		const CGFloat mainHeight = NSHeight(screens[0].frame);
		for (NSScreen* screen in screens)
		{
			const NSRect frame = screen.frame;
			if ((std::lround(NSMinX(frame)) != bounds.x) ||
			    (std::lround(mainHeight - NSMaxY(frame)) != bounds.y) ||
			    (std::lround(NSWidth(frame)) != bounds.w) ||
			    (std::lround(NSHeight(frame)) != bounds.h))
				continue;

			if (@available(macOS 12.0, *))
			{
				/* Only notched displays keep a band above fullscreen content. The band is the menu
				 * bar area, which is one point taller than the camera housing's safe area. With
				 * the menu bar hidden, the safe area is the best estimate. */
				const CGFloat safe = screen.safeAreaInsets.top;
				if (safe <= 0)
					return 0;
				const CGFloat band = std::max(NSMaxY(frame) - NSMaxY(screen.visibleFrame), safe);
				return static_cast<int>(std::lround(band * screen.backingScaleFactor));
			}
			return 0;
		}
	}
	return 0;
}
