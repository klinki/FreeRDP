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

#include "sdl_topbar.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <SDL3_ttf/SDL_ttf.h>

#include <freerdp/log.h>

#include "res/sdl3_resource_manager.hpp"

namespace
{
constexpr SDL_Color barColor = { 0x1f, 0x3b, 0x63, 0xf5 };
constexpr SDL_Color barBorderColor = { 0x58, 0x86, 0xb8, 0xff };
constexpr SDL_Color buttonColor = { 0x2c, 0x4e, 0x7a, 0xff };
constexpr SDL_Color pinnedButtonColor = { 0x4b, 0x78, 0xa8, 0xff };
constexpr SDL_Color buttonHoverColor = { 0x42, 0x70, 0xa5, 0xff };
constexpr SDL_Color iconColor = { 0xf2, 0xf6, 0xfb, 0xff };
constexpr SDL_Color titleColor = { 0xf2, 0xf6, 0xfb, 0xff };

struct Layout
{
	float height = 0.0f;
	float margin = 0.0f;
	float gap = 0.0f;
	float button = 0.0f;
	float buttonY = 0.0f;
	float pinX = 0.0f;
	float minimizeX = 0.0f;
	float restoreX = 0.0f;
	float closeX = 0.0f;
};

Layout layoutFor(const SDL_Rect& viewport)
{
	const auto scale = std::clamp(static_cast<float>(viewport.w) / 1920.0f, 1.0f, 2.0f);
	Layout layout;
	layout.height = 48.0f * scale;
	layout.margin = 8.0f * scale;
	layout.gap = 4.0f * scale;
	layout.button = 34.0f * scale;
	layout.buttonY = (layout.height - layout.button) / 2.0f;
	layout.closeX = static_cast<float>(viewport.w) - layout.margin - layout.button;
	layout.restoreX = layout.closeX - layout.gap - layout.button;
	layout.minimizeX = layout.restoreX - layout.gap - layout.button;
	layout.pinX = layout.minimizeX - layout.gap - layout.button;
	return layout;
}

SDL_FRect buttonRect(const Layout& layout, SdlTopBarButton button)
{
	float x = 0.0f;
	switch (button)
	{
		case SdlTopBarButton::Pin:
			x = layout.pinX;
			break;
		case SdlTopBarButton::Minimize:
			x = layout.minimizeX;
			break;
		case SdlTopBarButton::Restore:
			x = layout.restoreX;
			break;
		case SdlTopBarButton::Close:
			x = layout.closeX;
			break;
		case SdlTopBarButton::None:
			return {};
	}
	return { x, layout.buttonY, layout.button, layout.button };
}

bool pointIn(const SDL_FRect& rect, const SDL_FPoint& point)
{
	return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y &&
	       point.y < rect.y + rect.h;
}

bool setColor(SDL_Renderer* renderer, SDL_Color color)
{
	return SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

bool fillRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color)
{
	return setColor(renderer, color) && SDL_RenderFillRect(renderer, &rect);
}

bool drawLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2)
{
	return SDL_RenderLine(renderer, x1, y1, x2, y2);
}

void drawPin(SDL_Renderer* renderer, const SDL_FRect& rect)
{
	const auto cx = rect.x + rect.w / 2.0f;
	const auto top = rect.y + rect.h * 0.22f;
	const auto bottom = rect.y + rect.h * 0.70f;
	std::ignore = drawLine(renderer, cx - rect.w * 0.18f, top, cx + rect.w * 0.18f, top);
	std::ignore = drawLine(renderer, cx - rect.w * 0.18f, top, cx - rect.w * 0.11f, bottom);
	std::ignore = drawLine(renderer, cx + rect.w * 0.18f, top, cx + rect.w * 0.11f, bottom);
	std::ignore = drawLine(renderer, cx - rect.w * 0.20f, bottom, cx + rect.w * 0.20f, bottom);
	std::ignore = drawLine(renderer, cx, bottom, cx, rect.y + rect.h * 0.88f);
}

void drawMinimize(SDL_Renderer* renderer, const SDL_FRect& rect)
{
	const auto y = rect.y + rect.h * 0.70f;
	std::ignore = drawLine(renderer, rect.x + rect.w * 0.25f, y, rect.x + rect.w * 0.75f, y);
}

void drawRestore(SDL_Renderer* renderer, const SDL_FRect& rect)
{
	const SDL_FRect back = { rect.x + rect.w * 0.28f, rect.y + rect.h * 0.24f, rect.w * 0.42f,
	                        rect.h * 0.42f };
	const SDL_FRect front = { rect.x + rect.w * 0.40f, rect.y + rect.h * 0.36f, rect.w * 0.42f,
	                         rect.h * 0.42f };
	std::ignore = SDL_RenderRect(renderer, &back);
	std::ignore = SDL_RenderRect(renderer, &front);
}

void drawClose(SDL_Renderer* renderer, const SDL_FRect& rect)
{
	const auto left = rect.x + rect.w * 0.28f;
	const auto right = rect.x + rect.w * 0.72f;
	const auto top = rect.y + rect.h * 0.28f;
	const auto bottom = rect.y + rect.h * 0.72f;
	std::ignore = drawLine(renderer, left, top, right, bottom);
	std::ignore = drawLine(renderer, right, top, left, bottom);
}
} // namespace

struct SdlTopBar::Impl
{
	SDL_Renderer* renderer = nullptr;
	std::string title;
	std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> font{ nullptr, TTF_CloseFont };
	std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> titleTexture{ nullptr,
	                                                                           SDL_DestroyTexture };
	int titleWidth = 0;
	int titleHeight = 0;
};

SdlTopBar::SdlTopBar(SDL_Renderer* renderer, std::string title)
    : _impl(std::make_unique<Impl>())
{
	_impl->renderer = renderer;
	_impl->title = std::move(title);

	if (!_impl->renderer)
		return;

	auto ops = SDL3ResourceManager::get(SDLResourceManager::typeFonts(),
	                                    "OpenSans-VariableFont_wdth,wght.ttf");
	if (!ops)
	{
		WLog_Print(WLog_Get(CLIENT_TAG("SDL.topbar")), WLOG_WARN,
		           "Unable to load the connection bar font");
		return;
	}

	_impl->font = { TTF_OpenFontIO(ops, true, 20), TTF_CloseFont };
	if (!_impl->font)
	{
		WLog_Print(WLog_Get(CLIENT_TAG("SDL.topbar")), WLOG_WARN,
		           "Unable to create the connection bar font: %s", SDL_GetError());
		return;
	}

	auto surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
	    TTF_RenderText_Blended(_impl->font.get(), _impl->title.c_str(), 0, titleColor),
	    SDL_DestroySurface);
	if (!surface)
	{
		WLog_Print(WLog_Get(CLIENT_TAG("SDL.topbar")), WLOG_WARN,
		           "Unable to render the connection bar title: %s", SDL_GetError());
		return;
	}

	_impl->titleWidth = surface->w;
	_impl->titleHeight = surface->h;
	_impl->titleTexture = { SDL_CreateTextureFromSurface(_impl->renderer, surface.get()),
	                        SDL_DestroyTexture };
}

SdlTopBar::SdlTopBar(SdlTopBar&& other) noexcept = default;

SdlTopBar::~SdlTopBar() = default;

SdlTopBar& SdlTopBar::operator=(SdlTopBar&& other) noexcept = default;

bool SdlTopBar::contains(const SDL_Rect& viewport, const SDL_FPoint& pointer) const
{
	if (!_impl)
		return false;
	const auto layout = layoutFor(viewport);
	return pointer.y >= 0.0f && pointer.y < layout.height && pointer.x >= 0.0f &&
	       pointer.x < static_cast<float>(viewport.w);
}

bool SdlTopBar::nearTop(const SDL_Rect& viewport, const SDL_FPoint& pointer) const
{
	if (!_impl)
		return false;
	const auto layout = layoutFor(viewport);
	return pointer.y >= 0.0f && pointer.y < std::max(8.0f, layout.height * 0.16f) && pointer.x >= 0.0f &&
	       pointer.x < static_cast<float>(viewport.w);
}

SdlTopBarButton SdlTopBar::hitTest(const SDL_Rect& viewport, const SDL_FPoint& pointer) const
{
	if (!contains(viewport, pointer))
		return SdlTopBarButton::None;

	const auto layout = layoutFor(viewport);
	for (const auto button : { SdlTopBarButton::Pin, SdlTopBarButton::Minimize,
	                           SdlTopBarButton::Restore, SdlTopBarButton::Close })
	{
		if (pointIn(buttonRect(layout, button), pointer))
			return button;
	}
	return SdlTopBarButton::None;
}

bool SdlTopBar::draw(const SDL_Rect& viewport, bool pinned, const SDL_FPoint& pointer) const
{
	if (!_impl || !_impl->renderer || viewport.w <= 0 || viewport.h <= 0)
		return false;

	const auto layout = layoutFor(viewport);
	if (!SDL_SetRenderDrawBlendMode(_impl->renderer, SDL_BLENDMODE_BLEND))
		return false;
	if (!fillRect(_impl->renderer, { 0.0f, 0.0f, static_cast<float>(viewport.w), layout.height },
	              barColor))
		return false;

	if (!setColor(_impl->renderer, barBorderColor) ||
	    !drawLine(_impl->renderer, 0.0f, layout.height - 1.0f, static_cast<float>(viewport.w),
              layout.height - 1.0f))
		return false;

	const auto drawButton = [&](SdlTopBarButton button) -> bool
	{
		const auto rect = buttonRect(layout, button);
		const auto color = pointIn(rect, pointer)
		                       ? buttonHoverColor
		                       : (button == SdlTopBarButton::Pin && pinned ? pinnedButtonColor
		                                                                    : buttonColor);
		if (!fillRect(_impl->renderer, rect, color))
			return false;
		if (!setColor(_impl->renderer, iconColor))
			return false;
		switch (button)
		{
			case SdlTopBarButton::Pin:
				drawPin(_impl->renderer, rect);
				break;
			case SdlTopBarButton::Minimize:
				drawMinimize(_impl->renderer, rect);
				break;
			case SdlTopBarButton::Restore:
				drawRestore(_impl->renderer, rect);
				break;
			case SdlTopBarButton::Close:
				drawClose(_impl->renderer, rect);
				break;
			case SdlTopBarButton::None:
				break;
		}
		return true;
	};

	if (!drawButton(SdlTopBarButton::Pin) || !drawButton(SdlTopBarButton::Minimize) ||
	    !drawButton(SdlTopBarButton::Restore) || !drawButton(SdlTopBarButton::Close))
		return false;

	if (_impl->titleTexture && _impl->titleWidth > 0 && _impl->titleHeight > 0)
	{
		const auto maxWidth = std::max(0.0f, layout.pinX - layout.margin * 2.0f - 44.0f);
		const auto titleScale = std::min(1.0f, maxWidth / static_cast<float>(_impl->titleWidth));
		const SDL_FRect src = { 0.0f, 0.0f, static_cast<float>(_impl->titleWidth),
		                        static_cast<float>(_impl->titleHeight) };
		const SDL_FRect dst = { layout.margin * 2.0f,
		                        (layout.height - _impl->titleHeight * titleScale) / 2.0f,
		                        _impl->titleWidth * titleScale, _impl->titleHeight * titleScale };
		if (titleScale > 0.0f && !SDL_RenderTexture(_impl->renderer, _impl->titleTexture.get(), &src, &dst))
			return false;
	}

	return SDL_SetRenderDrawBlendMode(_impl->renderer, SDL_BLENDMODE_NONE);
}
