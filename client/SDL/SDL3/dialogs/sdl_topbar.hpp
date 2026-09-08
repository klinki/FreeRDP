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
	Pin,
	Minimize,
	Restore,
	Close
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

	[[nodiscard]] bool draw(const SDL_Rect& viewport, bool pinned, const SDL_FPoint& pointer) const;
	[[nodiscard]] bool contains(const SDL_Rect& viewport, const SDL_FPoint& pointer) const;
	[[nodiscard]] bool nearTop(const SDL_Rect& viewport, const SDL_FPoint& pointer) const;
	[[nodiscard]] SdlTopBarButton hitTest(const SDL_Rect& viewport,
	                                      const SDL_FPoint& pointer) const;

  private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
