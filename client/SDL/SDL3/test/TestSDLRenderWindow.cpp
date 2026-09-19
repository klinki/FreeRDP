/**
 * FreeRDP: SDL3 render window regression tests
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

#include "../sdl_window.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3_ttf/SDL_ttf.h>

namespace
{
struct Pixel
{
	Uint8 r = 0;
	Uint8 g = 0;
	Uint8 b = 0;
	Uint8 a = 0xff;

	bool operator==(const Pixel& other) const
	{
		return r == other.r && g == other.g && b == other.b && a == other.a;
	}
};

struct Frame
{
	int width = 0;
	int height = 0;
	std::vector<Pixel> pixels;

	Frame(int w, int h, Uint32 salt) : width(w), height(h),
	                                    pixels(static_cast<size_t>(w) * static_cast<size_t>(h))
	{
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				const auto value = static_cast<Uint32>(x * 31u + y * 47u) ^ salt;
				at(x, y) = { static_cast<Uint8>(0x18u + (value & 0xafu)),
				             static_cast<Uint8>(0x21u + ((value >> 7) & 0xafu)),
				             static_cast<Uint8>(0x35u + ((value >> 13) & 0xafu)), 0xff };
			}
		}
	}

	Pixel& at(int x, int y)
	{
		return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
	}

	const Pixel& at(int x, int y) const
	{
		return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
	}
};

struct Canvas
{
	int width = 0;
	int height = 0;
	std::vector<Pixel> pixels;

	Canvas(int w, int h, Pixel fill) : width(w), height(h),
	                                    pixels(static_cast<size_t>(w) * static_cast<size_t>(h), fill)
	{
	}

	bool contains(int x, int y) const
	{
		return x >= 0 && y >= 0 && x < width && y < height;
	}

	Pixel& at(int x, int y)
	{
		return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
	}
};

bool expect(bool condition, const std::string& message)
{
	if (!condition)
		std::cerr << message << '\n';
	return condition;
}

bool inRect(const SDL_Rect& rect, int x, int y)
{
	return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y && x < rect.x + rect.w &&
	       y < rect.y + rect.h;
}

void paintReference(Canvas& target, const Frame& source, std::vector<SDL_Rect> dirty,
                    SDL_Point offset)
{
	if (dirty.empty())
		dirty.push_back({ 0, 0, source.width, source.height });
	const SDL_Rect sourceBounds{ 0, 0, source.width, source.height };
	const SDL_Rect viewport{ 0, 0, target.width, target.height };
	for (const auto& rect : dirty)
	{
		for (int y = rect.y; y < rect.y + rect.h; y++)
		{
			for (int x = rect.x; x < rect.x + rect.w; x++)
			{
				const int dx = offset.x + x;
				const int dy = offset.y + y;
				if (inRect(sourceBounds, x, y) && inRect(viewport, dx, dy) && target.contains(dx, dy))
					target.at(dx, dy) = source.at(x, y);
			}
		}
	}
}

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;

SurfacePtr makeSurface(const Frame& frame)
{
	SurfacePtr surface(SDL_CreateSurface(frame.width, frame.height, SDL_PIXELFORMAT_RGBA32),
	                   SDL_DestroySurface);
	if (!surface)
		return { nullptr, SDL_DestroySurface };
	SDL_SetSurfaceBlendMode(surface.get(), SDL_BLENDMODE_NONE);
	for (int y = 0; y < frame.height; y++)
	{
		for (int x = 0; x < frame.width; x++)
		{
			const auto& pixel = frame.at(x, y);
			if (!SDL_WriteSurfacePixel(surface.get(), x, y, pixel.r, pixel.g, pixel.b, pixel.a))
				return { nullptr, SDL_DestroySurface };
		}
	}
	return surface;
}

bool same(const Canvas& expected, SDL_Surface* actual, const std::string& name)
{
	if (!actual || actual->w != expected.width || actual->h != expected.height)
		return expect(false, name + ": readback dimensions differ");
	for (int y = 0; y < expected.height; y++)
	{
		for (int x = 0; x < expected.width; x++)
		{
			Uint8 r = 0;
			Uint8 g = 0;
			Uint8 b = 0;
			Uint8 a = 0;
			if (!SDL_ReadSurfacePixel(actual, x, y, &r, &g, &b, &a))
				return expect(false, name + ": unable to decode readback pixel");
			const Pixel got{ r, g, b, a };
			if (!(got == expected.pixels[static_cast<size_t>(y) * static_cast<size_t>(expected.width) +
			                              static_cast<size_t>(x)]))
				return expect(false, name + ": first differing pixel at (" + std::to_string(x) + "," +
			                              std::to_string(y) + ")");
		}
	}
	return true;
}

bool readTarget(SdlWindow& window, const Canvas& expected, const std::string& name)
{
	SurfacePtr actual(SDL_RenderReadPixels(window.renderer(), nullptr), SDL_DestroySurface);
	return same(expected, actual.get(), name);
}

bool present(SdlWindow& window, const std::string& name)
{
	return expect(window.updateSurface(), name + ": SDL_RenderPresent failed");
}

class TestWindow final : public SdlWindow
{
  public:
	TestWindow(SDL_DisplayID display, SDL_Rect rect)
	    : SdlWindow(display, "FreeRDP render regression", rect, SDL_WINDOW_HIDDEN)
	{
	}
};

bool rendererLifecycle()
{
	const SDL_DisplayID display = SDL_GetPrimaryDisplay();
	if (!expect(display != 0, "SDL dummy video driver has no primary display"))
		return false;

	TestWindow window(display, { 0, 0, 13, 9 });
	if (!expect(window.renderer() != nullptr && window.window() != nullptr,
	            "hidden test window or renderer could not be created"))
		return false;

	const Frame first(17, 13, 0x19u);
	const Frame second(17, 13, 0xe7u);
	const auto firstSurface = makeSurface(first);
	const auto secondSurface = makeSurface(second);
	if (!expect(firstSurface != nullptr && secondSurface != nullptr, "source surface creation failed"))
		return false;

	const Pixel clear{ 7, 11, 13, 255 };
	if (!expect(window.fill(clear.r, clear.g, clear.b, clear.a), "initial target clear failed"))
		return false;

	const SDL_Point offset{ -5, -3 };
	Canvas expected(13, 9, clear);
	paintReference(expected, first, {}, offset);
	const std::vector<SDL_Rect> initialDirty = { { -2, -2, 1, 1 } };
	if (!expect(window.drawRects(firstSurface.get(), offset, initialDirty),
	            "initial forced full draw failed") ||
	    !readTarget(window, expected, "initial partial full draw") || !present(window, "initial present"))
		return false;

	// A second synthetic monitor has a different local viewport. The same
	// source update crosses its right edge, exercising the per-window clipping
	// that a multi-monitor draw fan-out needs.
	TestWindow neighbor(display, { 0, 0, 11, 9 });
	if (!expect(neighbor.renderer() != nullptr, "neighbor monitor renderer could not be created") ||
	    !expect(neighbor.fill(clear.r, clear.g, clear.b, clear.a),
	            "neighbor monitor target clear failed"))
		return false;
	const SDL_Point neighborOffset{ 8, -3 };
	const std::vector<SDL_Rect> neighborDirty = { { 0, 0, first.width, first.height } };
	Canvas neighborExpected(11, 9, clear);
	paintReference(neighborExpected, first, neighborDirty, neighborOffset);
	if (!expect(neighbor.drawRects(firstSurface.get(), neighborOffset, neighborDirty),
	            "neighbor monitor crossing draw failed") ||
	    !readTarget(neighbor, neighborExpected, "neighbor monitor pixel mismatch") ||
	    !present(neighbor, "neighbor monitor present"))
		return false;

	// An update wholly outside the window must not upload invalid source memory
	// or disturb the persistent target.
	const std::vector<SDL_Rect> offscreen = { { -40, -30, 4, 4 }, { 40, 30, 4, 4 } };
	if (!expect(window.drawRects(secondSurface.get(), offset, offscreen), "offscreen draw failed") ||
	    !readTarget(window, expected, "offscreen update changed target"))
		return false;

	// Partial regions crossing the left screen edge and overlapping a second
	// region must retain exact source pixels after upload and draw.
	const std::vector<SDL_Rect> dirty = { { -4, -2, 9, 8 }, { 3, 2, 12, 5 }, { 10, 7, 8, 8 } };
	paintReference(expected, second, dirty, offset);
	if (!expect(window.drawRects(secondSurface.get(), offset, dirty), "partial update failed") ||
	    !readTarget(window, expected, "partial update pixel mismatch"))
		return false;

	// The caller's empty list means full source repaint. This also verifies
	// that a previous partial frame cannot survive a full update.
	paintReference(expected, second, {}, offset);
	if (!expect(window.drawRects(secondSurface.get(), offset), "full repaint failed") ||
	    !readTarget(window, expected, "full repaint pixel mismatch") || !present(window, "full present"))
		return false;

	if (!expect(window.resize({ 15, 11 }), "window resize failed") ||
	    !expect(SDL_SyncWindow(window.window()), "window resize synchronization failed"))
		return false;
	if (!expect(window.fill(clear.r, clear.g, clear.b, clear.a), "resized target clear failed"))
		return false;
	Canvas resized(15, 11, clear);
	const SDL_Point resizedOffset{ 1, 1 };
	paintReference(resized, second, {}, resizedOffset);
	const std::vector<SDL_Rect> resizedDirty = { { 1, 1, 1, 1 } };
	if (!expect(window.drawRects(secondSurface.get(), resizedOffset, resizedDirty),
	            "resized forced full draw failed") ||
	    !readTarget(window, resized, "resized target pixel mismatch"))
		return false;

	// Recreating the source framebuffer must recreate the streaming texture and
	// must not retain the old 17x13 dimensions.
	const Frame smaller(9, 7, 0x5au);
	const auto smallerSurface = makeSurface(smaller);
	if (!expect(smallerSurface != nullptr, "smaller source surface creation failed"))
		return false;
	if (!expect(window.fill(clear.r, clear.g, clear.b, clear.a), "source-size target clear failed"))
		return false;
	Canvas smallerExpected(15, 11, clear);
	paintReference(smallerExpected, smaller, {}, { 2, 2 });
	const std::vector<SDL_Rect> smallerDirty = { { 2, 2, 1, 1 } };
	if (!expect(window.drawRects(smallerSurface.get(), { 2, 2 }, smallerDirty),
	            "source-size forced full draw failed") ||
	    !readTarget(window, smallerExpected, "source-size target pixel mismatch"))
		return false;

	// Exercise the scaled upload/draw path as a smoke check and verify that it
	// leaves pixels outside the scaled full-source destination untouched.
	if (!expect(window.fill(clear.r, clear.g, clear.b, clear.a), "scaled target clear failed"))
		return false;
	const SDL_FPoint scale{ 1.5f, 0.75f };
	const std::vector<SDL_Rect> scaledOffscreen = { { -30, -20, 4, 4 }, { 30, 20, 4, 4 } };
	Canvas scaledUntouched(15, 11, clear);
	if (!expect(window.drawScaledRects(smallerSurface.get(), scale, scaledOffscreen),
	            "scaled offscreen draw failed") ||
	    !readTarget(window, scaledUntouched, "scaled offscreen update changed target"))
		return false;
	if (!expect(window.fill(clear.r, clear.g, clear.b, clear.a), "scaled target refill failed"))
		return false;
	if (!expect(window.drawScaledRects(smallerSurface.get(), scale), "scaled full draw failed"))
		return false;
	SurfacePtr scaled(SDL_RenderReadPixels(window.renderer(), nullptr), SDL_DestroySurface);
	if (!expect(scaled != nullptr, "scaled target readback failed"))
		return false;
	const int scaledWidth = 14; // ceil(9 * 1.5)
	const int scaledHeight = 6; // ceil(7 * .75)
	bool changed = false;
	for (int y = 0; y < scaled->h; y++)
	{
		for (int x = 0; x < scaled->w; x++)
		{
			Uint8 r = 0;
			Uint8 g = 0;
			Uint8 b = 0;
			Uint8 a = 0;
			if (!SDL_ReadSurfacePixel(scaled.get(), x, y, &r, &g, &b, &a))
				return expect(false, "scaled target pixel unreadable");
			const Pixel pixel{ r, g, b, a };
			const bool inside = x < scaledWidth && y < scaledHeight;
			if (inside)
				changed = changed || !(pixel == clear);
			else if (!(pixel == clear))
				return expect(false, "scaled draw painted outside its destination");
		}
	}
	return expect(changed, "scaled draw did not paint its destination");
}
} // namespace

int main()
{
	if (!SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy") ||
	    !SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software") || !SDL_Init(SDL_INIT_VIDEO) || !TTF_Init())
	{
		std::cerr << "SDL init failed: " << SDL_GetError() << '\n';
		return 1;
	}
	const bool ok = rendererLifecycle();
	TTF_Quit();
	SDL_Quit();
	return ok ? 0 : 1;
}
