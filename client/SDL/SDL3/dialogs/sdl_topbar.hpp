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
#include "../sdl_refresh.hpp"

enum class SdlTopBarButton
{
	None,
	Performance,
	Refresh,
	RefreshLocal,
	RefreshTextures,
	RefreshServer,
	RefreshUpdates,
	RefreshCombined,
	Compact,
	Pin,
	Minimize,
	Restore,
	Close
};

/* mstsc-style floating connection bar: a small centered pill, draggable by
 * its title area to anywhere on the window, width-resizable via the right
 * edge handle. Fixed height; only horizontal resize. All geometry in render
 * pixels; the caller owns the bar rect, and SdlTopBar owns its hover panel. */
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
	void setPerformanceAvailable(bool value) { _performanceAvailable=value; }
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
	[[nodiscard]] bool contains(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                            const SDL_FPoint& pointer) const;
	[[nodiscard]] bool updateHover(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                               const SDL_FPoint& pointer, uint64_t now);
	[[nodiscard]] bool serviceHover(uint64_t now);
	[[nodiscard]] bool refreshMenuOpen() const { return _refreshOpen; }
	void closeRefreshMenu() { _refreshOpen = false; _refreshCloseAt = 0; }
	void setRefreshCapabilities(bool server, bool updates) { _serverRefresh = server; _outputReset = updates; }
	[[nodiscard]] bool refreshEnabled(SdlTopBarButton button) const;
	void noteRefresh(SdlTopBarButton button) { _lastRefresh = button; }
	[[nodiscard]] static bool refreshMode(SdlTopBarButton button, SdlRefreshMode& mode);
	[[nodiscard]] static SdlTopBarButton refreshButton(SdlRefreshMode mode);
	[[nodiscard]] SDL_FRect refreshMenuRect(const SdlTopBarRect& bar, const SDL_Rect& viewport) const;
	[[nodiscard]] bool nearTop(const SDL_Rect& viewport, const SDL_FPoint& pointer,
	                           bool compact) const;
	[[nodiscard]] bool hitResize(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                             const SDL_FPoint& pointer) const;
	[[nodiscard]] bool hitMove(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                           const SDL_FPoint& pointer) const;
	[[nodiscard]] SdlTopBarButton hitTest(const SdlTopBarRect& bar, const SDL_Rect& viewport,
	                                      const SDL_FPoint& pointer) const;

  private:
	bool _performanceAvailable=false;
	bool _refreshOpen = false, _serverRefresh = true, _outputReset = true;
	uint64_t _refreshCloseAt = 0;
	SdlTopBarButton _lastRefresh = SdlTopBarButton::None;
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
