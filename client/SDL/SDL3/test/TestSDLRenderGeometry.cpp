/**
 * FreeRDP: SDL3 render geometry tests
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

#include "../sdl_render_geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
using I64 = std::int64_t;

struct PixelBuffer
{
	SDL_Rect bounds{};
	std::vector<std::uint32_t> pixels;

	PixelBuffer(SDL_Rect rect, std::uint32_t fill) : bounds(rect), pixels(
	    static_cast<size_t>(std::max(0, rect.w)) * static_cast<size_t>(std::max(0, rect.h)), fill)
	{
	}

	bool contains(I64 x, I64 y) const
	{
		return x >= bounds.x && y >= bounds.y && x < static_cast<I64>(bounds.x) + bounds.w &&
		       y < static_cast<I64>(bounds.y) + bounds.h;
	}

	std::uint32_t& at(I64 x, I64 y)
	{
		return pixels[static_cast<size_t>(y - bounds.y) * static_cast<size_t>(bounds.w) +
		              static_cast<size_t>(x - bounds.x)];
	}

	const std::uint32_t& at(I64 x, I64 y) const
	{
		return pixels[static_cast<size_t>(y - bounds.y) * static_cast<size_t>(bounds.w) +
		              static_cast<size_t>(x - bounds.x)];
	}
};

struct SourceImage
{
	int width = 0;
	int height = 0;
	std::vector<std::uint32_t> pixels;

	SourceImage(int w, int h, std::uint32_t salt) : width(w), height(h), pixels(
	    static_cast<size_t>(w) * static_cast<size_t>(h))
	{
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				const auto value = static_cast<std::uint32_t>(x * 0x1021u + y * 0x0101u) ^ salt;
				pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
				    0xff000000u | ((value * 13u) & 0x00ff0000u) | ((value * 37u) & 0x0000ff00u) |
				    ((value * 71u) & 0x000000ffu);
			}
		}
	}

	std::uint32_t at(I64 x, I64 y) const
	{
		return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
	}

	void set(I64 x, I64 y, std::uint32_t value)
	{
		pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = value;
	}
};

bool expect(bool condition, const std::string& message)
{
	if (!condition)
		std::cerr << message << '\n';
	return condition;
}

bool same(const PixelBuffer& first, const PixelBuffer& second, const std::string& name)
{
	if (first.bounds.x != second.bounds.x || first.bounds.y != second.bounds.y ||
	    first.bounds.w != second.bounds.w || first.bounds.h != second.bounds.h)
		return expect(false, name + ": buffer bounds differ");
	for (size_t i = 0; i < first.pixels.size(); i++)
	{
		if (first.pixels[i] != second.pixels[i])
		{
			const auto x = static_cast<I64>(first.bounds.x) +
			               static_cast<I64>(i % static_cast<size_t>(first.bounds.w));
			const auto y = static_cast<I64>(first.bounds.y) +
			               static_cast<I64>(i / static_cast<size_t>(first.bounds.w));
			return expect(false, name + ": first differing pixel at (" + std::to_string(x) + "," +
			                              std::to_string(y) + ")");
		}
	}
	return true;
}

bool pointIn(const SDL_Rect& rect, I64 x, I64 y)
{
	if (rect.w <= 0 || rect.h <= 0)
		return false;
	return x >= rect.x && y >= rect.y && x < static_cast<I64>(rect.x) + rect.w &&
	       y < static_cast<I64>(rect.y) + rect.h;
}

/* Independent per-pixel oracle. It deliberately does not use the production
 * rectangle intersection helper, so a shared arithmetic mistake is visible. */
void paintReference(PixelBuffer& target, const SourceImage& source, const std::vector<SDL_Rect>& dirty,
                    const SDL_Rect& sourceBounds, const SDL_Point& offset,
                    const SDL_Rect& viewport)
{
	for (const auto& rect : dirty)
	{
		const I64 right = static_cast<I64>(rect.x) + rect.w;
		const I64 bottom = static_cast<I64>(rect.y) + rect.h;
		for (I64 sy = rect.y; sy < bottom; sy++)
		{
			for (I64 sx = rect.x; sx < right; sx++)
			{
				const I64 dx = static_cast<I64>(offset.x) + sx;
				const I64 dy = static_cast<I64>(offset.y) + sy;
				if (!pointIn(sourceBounds, sx, sy) || !pointIn(viewport, dx, dy) ||
				    !target.contains(dx, dy))
					continue;
				target.at(dx, dy) = source.at(sx, sy);
			}
		}
	}
}

void paintClipped(PixelBuffer& target, const SourceImage& source, const std::vector<SDL_Rect>& dirty,
                  const SDL_Rect& sourceBounds, const SDL_Point& offset,
                  const SDL_Rect& viewport)
{
	const auto clipped = sdl::render::clipSourceRects(dirty, sourceBounds, offset, viewport);
	for (const auto& rect : clipped)
	{
		for (I64 sy = rect.y; sy < static_cast<I64>(rect.y) + rect.h; sy++)
		{
			for (I64 sx = rect.x; sx < static_cast<I64>(rect.x) + rect.w; sx++)
			{
				const I64 dx = static_cast<I64>(offset.x) + sx;
				const I64 dy = static_cast<I64>(offset.y) + sy;
				if (!target.contains(dx, dy))
					continue;
				target.at(dx, dy) = source.at(sx, sy);
			}
		}
	}
}

bool geometryMatrix()
{
	const SourceImage source(17, 13, 0x37a1c5e9u);
	const SDL_Rect sourceBounds{ 1, 1, 15, 11 };
	const SDL_Rect targetBounds{ -20, -10, 100, 64 };
	const std::vector<SDL_Rect> dirty = {
		{ -9, -7, 12, 10 }, { 7, 4, 13, 12 }, { 15, 0, 10, 7 },
		{ -20, -20, 5, 5 }, { 0, 0, 1, 1 }, { 16, 12, 4, 4 },
		{ 6, 5, 5, 3 }, { 4, 4, -2, 6 }, { 3, 8, 5, 0 },
	};
	const std::vector<SDL_Rect> monitors = {
		{ -15, -5, 31, 23 }, // negative origin and odd dimensions
		{ 16, 1, 35, 27 },   // positive origin and odd dimensions
		{ 51, -8, 21, 29 },  // edge crossing at the far side
	};
	const std::vector<SDL_Point> offsets = { { -18, -7 }, { 18, 3 }, { 38, -9 } };

	if (!expect(monitors.size() == offsets.size(), "test matrix is malformed"))
		return false;
	const auto visible = sdl::render::visibleSourceRect(offsets.front(), monitors.front());
	if (!expect(visible.x == 3 && visible.y == 2 && visible.w == 31 && visible.h == 23,
	            "nonzero viewport origin was dropped from visible source geometry"))
		return false;

	for (size_t i = 0; i < monitors.size(); i++)
	{
		PixelBuffer reference(targetBounds, 0xdeadbeefu);
		PixelBuffer clipped(targetBounds, 0xdeadbeefu);
		paintReference(reference, source, dirty, sourceBounds, offsets[i], monitors[i]);
		paintClipped(clipped, source, dirty, sourceBounds, offsets[i], monitors[i]);
		if (!same(reference, clipped, "synthetic monitor " + std::to_string(i)))
			return false;

		// Explicit dirty regions that miss a monitor are a no-op. The caller's
		// empty-list full repaint decision is separate and is tested below.
		const std::vector<SDL_Rect> miss = { { -100, -100, 2, 2 } };
		if (!expect(sdl::render::clipSourceRects(miss, sourceBounds, offsets[i], monitors[i]).empty(),
		            "off-window dirty region was not clipped to empty"))
			return false;
		PixelBuffer missTarget(targetBounds, 0xdeadbeefu);
		paintClipped(missTarget, source, miss, sourceBounds, offsets[i], monitors[i]);
		if (!expect(missTarget.pixels == PixelBuffer(targetBounds, 0xdeadbeefu).pixels,
		            "clipped-empty batch changed pixels"))
			return false;
	}
	return true;
}

bool emptyAndFullSemantics()
{
	const SourceImage source(17, 13, 0x12345678u);
	const SDL_Rect sourceBounds{ 0, 0, source.width, source.height };
	const SDL_Rect targetBounds{ -4, -3, 31, 25 };
	const SDL_Rect viewport{ -2, 1, 19, 17 };
	const SDL_Point offset{ -5, -4 };

	if (!expect(sdl::render::clipSourceRects({}, sourceBounds, offset, viewport).empty(),
	            "empty dirty list must stay a no-op at the geometry seam"))
		return false;

	PixelBuffer reference(targetBounds, 0xdeadbeefu);
	PixelBuffer clipped(targetBounds, 0xdeadbeefu);
	const std::vector<SDL_Rect> full = { sourceBounds };
	paintReference(reference, source, full, sourceBounds, offset, viewport);
	paintClipped(clipped, source, full, sourceBounds, offset, viewport);
	return same(reference, clipped, "explicit full repaint") &&
	       expect(clipped.pixels != PixelBuffer(targetBounds, 0xdeadbeefu).pixels,
	              "explicit full repaint was treated as a no-op");
}

bool limitValues()
{
	const auto min = std::numeric_limits<Sint32>::min();
	const auto max = std::numeric_limits<Sint32>::max();
	const SDL_Rect finiteSource{ 0, 0, 8, 8 };
	const SDL_Rect finiteViewport{ -3, -2, 11, 9 };

	// The visible source starts beyond INT_MAX when a window is translated by
	// INT_MIN. It must not wrap back into the finite source image.
	const auto minOffset = sdl::render::clipSourceRect(finiteSource, finiteSource, { min, min },
	                                                   finiteViewport);
	if (!expect(minOffset.w == 0, "INT_MIN offset wrapped into a visible source rectangle"))
		return false;
	if (!expect(sdl::render::clipSourceRect({ max - 3, max - 3, 3, 3 },
	                                        { max - 3, max - 3, 3, 3 }, { max, max },
	                                        finiteViewport).w == 0,
	            "INT_MAX offset produced a false visible source rectangle"))
		return false;

	// Intersections themselves must remain defined when endpoints are near the
	// SDL integer limits. The valid overlap is only the final two pixels.
	const auto overlap = sdl::render::intersection({ max - 4, max - 4, 8, 8 },
	                                                { max - 2, max - 2, 2, 2 });
	return expect(overlap.x == max - 2 && overlap.y == max - 2 && overlap.w == 2 &&
	                  overlap.h == 2,
	              "near-INT_MAX intersection lost its valid overlap");
}

bool batchedFramePreservation()
{
	const SourceImage first(17, 13, 0x0f0f0f0fu);
	const SourceImage second(17, 13, 0xf0f0f0f0u);
	const SDL_Rect sourceBounds{ 0, 0, first.width, first.height };
	const SDL_Rect targetBounds{ -12, -8, 55, 39 };
	const SDL_Rect viewport{ -6, -3, 31, 25 };
	const SDL_Point offset{ -9, -6 };
	const std::vector<SDL_Rect> updates = {
		{ -4, -2, 8, 7 }, { 2, 3, 5, 9 }, { 11, 6, 9, 4 }, { 14, -3, 5, 12 },
		{ -20, -20, 4, 4 }, { 0, 0, 1, 1 },
	};

	PixelBuffer expected(targetBounds, 0xdeadbeefu);
	PixelBuffer actual(targetBounds, 0xdeadbeefu);
	const std::vector<SDL_Rect> full = { sourceBounds };
	paintReference(expected, first, full, sourceBounds, offset, viewport);
	paintClipped(actual, first, full, sourceBounds, offset, viewport);
	if (!same(expected, actual, "initial frame"))
		return false;

	// Upload a new source tile before drawing each clipped destination. The
	// staged image models the persistent SDL texture and catches a draw-before-
	// upload ordering bug: the visible tile must contain the second frame while
	// untouched pixels retain the first frame.
	paintReference(expected, second, updates, sourceBounds, offset, viewport);
	SourceImage staged = first;
	for (const auto& update : updates)
	{
		const auto clipped = sdl::render::clipSourceRect(update, sourceBounds, offset, viewport);
		if (clipped.w <= 0 || clipped.h <= 0)
			continue;
		for (I64 y = clipped.y; y < static_cast<I64>(clipped.y) + clipped.h; y++)
		{
			for (I64 x = clipped.x; x < static_cast<I64>(clipped.x) + clipped.w; x++)
				staged.set(x, y, second.at(x, y));
		}
		paintClipped(actual, staged, { clipped }, sourceBounds, offset, viewport);
	}
	return same(expected, actual, "batched upload/draw frame") &&
	       expect(actual.pixels != PixelBuffer(targetBounds, 0xdeadbeefu).pixels,
	              "batched update did not paint any visible pixel");
}
} // namespace

int main()
{
	return geometryMatrix() && emptyAndFullSemantics() && limitValues() && batchedFramePreservation()
	           ? 0
	           : 1;
}
