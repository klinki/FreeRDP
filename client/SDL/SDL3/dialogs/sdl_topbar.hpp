/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL client connection bar
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

#include <memory>
#include <string>

#include <SDL3/SDL.h>

enum class SdlTopBarButton
{
	None,
	Compact,
	Pin,
	Minimize,
	Restore,
	Close
};

/* mstsc-style floating connection bar: a small centered pill, draggable by
 * its title area to anywhere on the window, width-resizable via the right
 * edge handle. Fixed height; only horizontal resize. All geometry in render
 * pixels; SdlTopBar is stateless, the caller owns the rect. */
struct SdlTopBarRect
{
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;
};

class SdlTopBar
{
  public:
	explicit SdlTopBar(SDL_Renderer* renderer, std::string title);
	SdlTopBar(const SdlTopBar& other) = delete;
	SdlTopBar(SdlTopBar&& other) noexcept;
	~SdlTopBar();

	SdlTopBar& operator=(const SdlTopBar& other) = delete;
	SdlTopBar& operator=(SdlTopBar&& other) noexcept;

	/* Default rect: centered horizontally at the top, default width. */
	[[nodiscard]] static SdlTopBarRect defaultRect(const SDL_Rect& viewport, bool compact);
	[[nodiscard]] static float fixedHeight(const SDL_Rect& viewport, bool compact);
	[[nodiscard]] static float minWidth(const SDL_Rect& viewport);
	[[nodiscard]] static float defaultWidth(const SDL_Rect& viewport);
	[[nodiscard]] static SdlTopBarRect clampToViewport(SdlTopBarRect bar,
	                                                   const SDL_Rect& viewport, bool compact);

	[[nodiscard]] bool draw(const SdlTopBarRect& bar, const SDL_Rect& viewport, bool pinned,
	                        const SDL_FPoint& pointer) const;
	[[nodiscard]] bool contains(const SdlTopBarRect& bar, const SDL_FPoint& pointer) const;
	[[nodiscard]] bool nearTop(const SDL_Rect& viewport, const SDL_FPoint& pointer,
	                           bool compact) const;
	[[nodiscard]] bool hitResize(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                             const SDL_FPoint& pointer) const;
	[[nodiscard]] bool hitMove(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                           const SDL_FPoint& pointer) const;
	[[nodiscard]] SdlTopBarButton hitTest(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                                      const SDL_FPoint& pointer) const;

  private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
