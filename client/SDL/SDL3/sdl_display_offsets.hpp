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

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

/* Pixel origins of the local displays, derived from their logical (point) bounds and their pixel
 * sizes. Each entry maps a display to its logical bounds and its pixel rectangle; only the pixel
 * origin is computed. */
namespace sdl_display_offsets
{
	using Offsets = std::map<SDL_DisplayID, std::pair<SDL_Rect, SDL_Rect>>;

	/* Two displays are neighbours when they share an edge segment of positive length; edges
	 * that are merely in line, e.g. a display diagonally below another, do not count. */
	inline bool spans(int a, int alen, int b, int blen)
	{
		return (a < b + blen) && (b < a + alen);
	}

	inline bool touchX(const SDL_Rect& a, const SDL_Rect& b)
	{
		return ((a.x + a.w == b.x) || (b.x + b.w == a.x)) && spans(a.y, a.h, b.y, b.h);
	}

	inline bool touchY(const SDL_Rect& a, const SDL_Rect& b)
	{
		return ((a.y + a.h == b.y) || (b.y + b.h == a.y)) && spans(a.x, a.w, b.x, b.w);
	}

	/* Distance in points, converted to pixels of the display it lies on. */
	inline int scaled(int distance, int logical, int pixel)
	{
		if (logical <= 0)
			return distance;
		return static_cast<int>(std::lround(static_cast<double>(distance) * pixel / logical));
	}

	/* Pixel position along the axis that does not touch. Without this, a monitor that is
	 * adjacent on one axis but shifted on the other (e.g. a wider monitor above another, or a
	 * rotated monitor that is not top aligned) keeps offset 0 on that axis, the layout sent to
	 * the server no longer matches the local one and pointer input lands offset by that
	 * difference.
	 *
	 * The offset between the two starts lies on whichever display starts first, so it is scaled
	 * by that display; with the reference's scale only, a 1x display hanging off a 2x one could
	 * lose contact with it. fromEnd keeps the offset between the two ends instead. */
	inline int crossPosition(int ePos, int eLogical, int ePixel, int fPos, int fLogical, int fPixel,
	                         int fPixelPos, bool fromEnd)
	{
		if (!fromEnd)
		{
			const int d = ePos - fPos;
			return fPixelPos +
			       ((d >= 0) ? scaled(d, fLogical, fPixel) : scaled(d, eLogical, ePixel));
		}
		const int d = (ePos + eLogical) - (fPos + fLogical);
		const int end = fPixelPos + fPixel +
		                ((d <= 0) ? scaled(d, fLogical, fPixel) : scaled(d, eLogical, ePixel));
		return end - ePixel;
	}

	inline bool overlapsPlaced(const Offsets& offsets, SDL_DisplayID id, const SDL_Rect& pixel,
	                           const std::vector<SDL_DisplayID>& placed)
	{
		for (const auto other : placed)
		{
			if (other == id)
				continue;
			const auto& o = offsets.at(other).second;
			if (spans(pixel.x, pixel.w, o.x, o.w) && spans(pixel.y, pixel.h, o.y, o.h))
				return true;
		}
		return false;
	}

	/* Places every display that touches id and is not placed yet, and returns them. A display
	 * keeps the position it got from the first display that placed it. */
	inline std::vector<SDL_DisplayID> neighbours(Offsets& offsets, SDL_DisplayID id,
	                                             std::vector<SDL_DisplayID>& placed)
	{
		const auto first = offsets.at(id);
		std::vector<SDL_DisplayID> result;

		for (auto& entry : offsets)
		{
			if (std::find(placed.begin(), placed.end(), entry.first) != placed.end())
				continue;

			const auto& e = entry.second.first;
			const auto& f = first.first;
			auto& pixel = entry.second.second;
			const bool ax = touchX(e, f);
			if (!ax && !touchY(e, f))
				continue;

			const auto place = [&](bool fromEnd)
			{
				if (ax)
				{
					pixel.x =
					    (e.x < f.x) ? first.second.x - pixel.w : first.second.x + first.second.w;
					pixel.y = crossPosition(e.y, e.h, pixel.h, f.y, f.h, first.second.h,
					                        first.second.y, fromEnd);
				}
				else
				{
					pixel.y =
					    (e.y < f.y) ? first.second.y - pixel.h : first.second.y + first.second.h;
					pixel.x = crossPosition(e.x, e.w, pixel.w, f.x, f.w, first.second.w,
					                        first.second.x, fromEnd);
				}
				return !overlapsPlaced(offsets, entry.first, pixel, placed);
			};

			/* Mixed scales can make the point layout impossible to keep exactly: a 2x display
			 * below a 1x one is wider in pixels and reaches under the display next to it. Keep
			 * the far edges in line instead when the near ones would overlap a placed display.
			 * When neither fits, keep the near edges; the layout check reports the overlap. */
			if (!place(false) && !place(true))
				std::ignore = place(false);
			placed.push_back(entry.first);
			result.push_back(entry.first);
		}
		return result;
	}

	/* Places the primary display at its origin, then walks outwards: each display is placed
	 * next to the first placed display it touches, in pixels of both. */
	inline void update(Offsets& offsets, SDL_DisplayID primary)
	{
		if (offsets.empty())
			return;
		if (offsets.count(primary) == 0)
			primary = offsets.begin()->first;
		/* Align display offsets to the primary display origin. */
		auto it = offsets.find(primary);
		it->second.second.x = it->second.first.x;
		it->second.second.y = it->second.first.y;
		std::vector<SDL_DisplayID> placed{ primary };

		auto next = neighbours(offsets, primary, placed);
		while (!next.empty())
		{
			const auto neighbor = next.front();
			next.erase(next.begin());
			auto more = neighbours(offsets, neighbor, placed);
			next.insert(next.end(), more.begin(), more.end());
		}
	}
} // namespace sdl_display_offsets
