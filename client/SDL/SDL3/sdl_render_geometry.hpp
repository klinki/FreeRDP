/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client render geometry helpers
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
#include <cstdint>
#include <limits>
#include <vector>

#include <SDL3/SDL.h>

namespace sdl::render
{
namespace detail
{
struct RectBounds
{
	Sint64 left = 0;
	Sint64 top = 0;
	Sint64 right = 0;
	Sint64 bottom = 0;
};

[[nodiscard]] inline RectBounds bounds(const SDL_Rect& rect)
{
	return { static_cast<Sint64>(rect.x), static_cast<Sint64>(rect.y),
	         static_cast<Sint64>(rect.x) + rect.w, static_cast<Sint64>(rect.y) + rect.h };
}

[[nodiscard]] inline bool valid(const SDL_Rect& rect)
{
	return rect.w > 0 && rect.h > 0;
}

[[nodiscard]] inline RectBounds visibleSourceBounds(const SDL_Point& offset,
	                                                  const SDL_Rect& windowViewport)
{
	return { static_cast<Sint64>(windowViewport.x) - static_cast<Sint64>(offset.x),
	         static_cast<Sint64>(windowViewport.y) - static_cast<Sint64>(offset.y),
	         static_cast<Sint64>(windowViewport.x) - static_cast<Sint64>(offset.x) +
	             windowViewport.w,
	         static_cast<Sint64>(windowViewport.y) - static_cast<Sint64>(offset.y) +
	             windowViewport.h };
}
} // namespace detail

/* Return the half-open intersection of two integer rectangles.  A rectangle
 * with a non-positive extent represents an empty intersection.  Use 64-bit
 * endpoints so clipping remains well-defined for negative desktop origins and
 * for values close to the SDL integer limits. */
[[nodiscard]] inline SDL_Rect intersection(const SDL_Rect& first, const SDL_Rect& second)
{
	if (!detail::valid(first) || !detail::valid(second))
		return {};

	const auto a = detail::bounds(first);
	const auto b = detail::bounds(second);
	const Sint64 left = std::max(a.left, b.left);
	const Sint64 top = std::max(a.top, b.top);
	const Sint64 right = std::min(a.right, b.right);
	const Sint64 bottom = std::min(a.bottom, b.bottom);

	if ((right <= left) || (bottom <= top))
		return {};

	return { static_cast<Sint32>(left), static_cast<Sint32>(top),
	         static_cast<Sint32>(right - left), static_cast<Sint32>(bottom - top) };
}

/* The source-space rectangle that maps to the visible part of a window when
 * a source point is translated by offset to become a destination point. */
[[nodiscard]] inline SDL_Rect visibleSourceRect(const SDL_Point& offset,
                                                const SDL_Rect& windowViewport)
{
	if (!detail::valid(windowViewport))
		return {};

	const auto visible = detail::visibleSourceBounds(offset, windowViewport);
	/* SDL_Rect cannot represent a source interval outside the Sint32 domain.
	 * Return the representable portion; clipSourceRect below keeps the full
	 * 64-bit interval and is therefore the operation to use for rendering. */
	const Sint64 minValue = std::numeric_limits<Sint32>::min();
	const Sint64 maxValue = static_cast<Sint64>(std::numeric_limits<Sint32>::max()) + 1;
	const auto left = std::max(visible.left, minValue);
	const auto top = std::max(visible.top, minValue);
	const auto right = std::min(visible.right, maxValue);
	const auto bottom = std::min(visible.bottom, maxValue);
	if ((right <= left) || (bottom <= top))
		return {};

	return { static_cast<Sint32>(left), static_cast<Sint32>(top),
	         static_cast<Sint32>(std::min<Sint64>(right - left, std::numeric_limits<Sint32>::max())),
	         static_cast<Sint32>(std::min<Sint64>(bottom - top, std::numeric_limits<Sint32>::max())) };
}

/* Clip one dirty source rectangle to both the framebuffer and the window's
 * visible source area.  The returned rectangle is still in source
 * coordinates; callers can form the destination as offset + source. */
[[nodiscard]] inline SDL_Rect clipSourceRect(const SDL_Rect& sourceRect,
                                             const SDL_Rect& sourceBounds,
                                             const SDL_Point& offset,
                                             const SDL_Rect& windowViewport)
{
	if (!detail::valid(sourceRect) || !detail::valid(sourceBounds) || !detail::valid(windowViewport))
		return {};

	const auto source = detail::bounds(sourceRect);
	const auto bounds = detail::bounds(sourceBounds);
	const auto visible = detail::visibleSourceBounds(offset, windowViewport);
	const Sint64 left = std::max({ source.left, bounds.left, visible.left });
	const Sint64 top = std::max({ source.top, bounds.top, visible.top });
	const Sint64 right = std::min({ source.right, bounds.right, visible.right });
	const Sint64 bottom = std::min({ source.bottom, bounds.bottom, visible.bottom });
	if ((right <= left) || (bottom <= top))
		return {};

	return { static_cast<Sint32>(left), static_cast<Sint32>(top),
	         static_cast<Sint32>(right - left), static_cast<Sint32>(bottom - top) };
}

/* Clip explicit dirty regions without assigning a meaning to an empty input.
 * In particular, an empty result means that none of the supplied regions
 * reaches this window; full repaint decisions belong to the caller. */
[[nodiscard]] inline std::vector<SDL_Rect>
clipSourceRects(const std::vector<SDL_Rect>& sourceRects, const SDL_Rect& sourceBounds,
                const SDL_Point& offset, const SDL_Rect& windowViewport)
{
	std::vector<SDL_Rect> clipped;
	clipped.reserve(sourceRects.size());
	for (const auto& sourceRect : sourceRects)
	{
		const auto rect = clipSourceRect(sourceRect, sourceBounds, offset, windowViewport);
		if ((rect.w > 0) && (rect.h > 0))
			clipped.push_back(rect);
	}
	return clipped;
}
} // namespace sdl::render
