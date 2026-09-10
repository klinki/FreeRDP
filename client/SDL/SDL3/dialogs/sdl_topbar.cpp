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
	float grip = 0.0f;
	float buttonY = 0.0f;
	float compactX = 0.0f;
	float pinX = 0.0f;
	float minimizeX = 0.0f;
	float restoreX = 0.0f;
	float closeX = 0.0f;
	float barX = 0.0f;
	float barW = 0.0f;
};

float topBarScale(const SDL_Rect& viewport)
{
	return std::clamp(static_cast<float>(viewport.w) / 1920.0f, 1.0f, 2.0f);
}

Layout layoutFor(const SdlTopBarRect& bar, const SDL_Rect& viewport)
{
	const auto scale = topBarScale(viewport);
	Layout layout{};
	layout.height = bar.h;
	layout.margin = 10.0f * scale;
	layout.gap = 4.0f * scale;
	/* Buttons shrink with the bar so the compact mode stays proportionate. */
	layout.button = std::clamp(bar.h - 6.0f * scale, 14.0f * scale, 30.0f * scale);
	layout.grip = 8.0f * scale;
	layout.buttonY = bar.y + (bar.h - layout.button) / 2.0f;
	layout.barX = bar.x;
	layout.barW = bar.w;
	layout.closeX = bar.x + bar.w - layout.margin - layout.button;
	layout.restoreX = layout.closeX - layout.gap - layout.button;
	layout.minimizeX = layout.restoreX - layout.gap - layout.button;
	layout.pinX = layout.minimizeX - layout.gap - layout.button;
	layout.compactX = layout.pinX - layout.gap - layout.button;
	return layout;
}


SDL_FRect buttonRect(const Layout& layout, SdlTopBarButton button)
{
	float x = 0.0f;
	switch (button)
	{
		case SdlTopBarButton::Compact:
			x = layout.compactX;
			break;
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

void drawCompact(SDL_Renderer* renderer, const SDL_FRect& rect)
{
	/* Density glyph: three horizontal lines, middle one shorter. */
	for (int i = 0; i < 3; i++)
	{
		const auto y = rect.y + rect.h * (0.32f + 0.18f * static_cast<float>(i));
		const auto inset = (i == 1) ? rect.w * 0.36f : rect.w * 0.24f;
		std::ignore = drawLine(renderer, rect.x + inset, y, rect.x + rect.w - inset, y);
	}
}
} // namespace

float SdlTopBar::fixedHeight(const SDL_Rect& viewport, bool compact)
{
	/* Normal 30px, compact 20px (3/4 and 1/2 of the original 40px bar). */
	return (compact ? 20.0f : 30.0f) * topBarScale(viewport);
}

float SdlTopBar::minWidth(const SDL_Rect& viewport)
{
	const auto scale = topBarScale(viewport);
	/* grips + 5 buttons + gaps + margins + a sliver of title so the bar stays
	 * grabbable */
	return 2.0f * 8.0f * scale + 5.0f * 30.0f * scale + 4.0f * 4.0f * scale +
	       2.0f * 10.0f * scale + 40.0f * scale;
}

float SdlTopBar::defaultWidth(const SDL_Rect& viewport)
{
	const auto scale = topBarScale(viewport);
	return std::min(static_cast<float>(viewport.w), 280.0f * scale);
}

SdlTopBarRect SdlTopBar::defaultRect(const SDL_Rect& viewport, bool compact)
{
	const auto w = defaultWidth(viewport);
	const auto h = fixedHeight(viewport, compact);
	return { (static_cast<float>(viewport.w) - w) / 2.0f, 0.0f, w, h };
}

SdlTopBarRect SdlTopBar::clampToViewport(SdlTopBarRect bar, const SDL_Rect& viewport, bool compact)
{
	const auto minW = minWidth(viewport);
	if (bar.w < minW)
		bar.w = minW;
	if (bar.w > static_cast<float>(viewport.w))
		bar.w = static_cast<float>(viewport.w);
	if (bar.x < 0.0f)
		bar.x = 0.0f;
	if (bar.x + bar.w > static_cast<float>(viewport.w))
		bar.x = static_cast<float>(viewport.w) - bar.w;
	if (bar.y < 0.0f)
		bar.y = 0.0f;
	if (bar.y + bar.h > static_cast<float>(viewport.h))
		bar.y = static_cast<float>(viewport.h) - bar.h;
	bar.h = fixedHeight(viewport, compact);
	return bar;
}


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

bool SdlTopBar::contains(const SdlTopBarRect& bar, const SDL_FPoint& pointer) const
{
	if (!_impl)
		return false;
	return pointer.x >= bar.x && pointer.x < bar.x + bar.w && pointer.y >= bar.y &&
	       pointer.y < bar.y + bar.h;
}

bool SdlTopBar::nearTop(const SDL_Rect& viewport, const SDL_FPoint& pointer,
                         bool compact) const
{
	if (!_impl)
		return false;
	const auto height = fixedHeight(viewport, compact);
	return pointer.y >= 0.0f && pointer.y < std::max(8.0f, height * 0.16f) && pointer.x >= 0.0f &&
	       pointer.x < static_cast<float>(viewport.w);
}

bool SdlTopBar::hitResize(const SdlTopBarRect& bar, const SDL_Rect& viewport,
                           const SDL_FPoint& pointer) const
{
	if (!_impl || bar.w <= 0.0f || bar.h <= 0.0f)
		return false;
	/* Resize grips: vertical strips on both outer edges, full bar height. */
	const float grip = 8.0f * topBarScale(viewport);
	const bool left = pointer.x >= bar.x && pointer.x < bar.x + grip && pointer.y >= bar.y &&
	                  pointer.y < bar.y + bar.h;
	const bool right = pointer.x >= bar.x + bar.w - grip && pointer.x < bar.x + bar.w &&
	                   pointer.y >= bar.y && pointer.y < bar.y + bar.h;
	return left || right;
}

bool SdlTopBar::hitMove(const SdlTopBarRect& bar, const SDL_Rect& viewport,
                         const SDL_FPoint& pointer) const
{
	if (!contains(bar, pointer))
		return false;
	if (hitTest(bar, viewport, pointer) != SdlTopBarButton::None)
		return false;
	return !hitResize(bar, viewport, pointer);
}

SdlTopBarButton SdlTopBar::hitTest(const SdlTopBarRect& bar, const SDL_Rect& viewport,
                                   const SDL_FPoint& pointer) const
{
	if (!contains(bar, pointer))
		return SdlTopBarButton::None;

	const auto layout = layoutFor(bar, viewport);
	for (const auto button : { SdlTopBarButton::Compact, SdlTopBarButton::Pin,
	                           SdlTopBarButton::Minimize, SdlTopBarButton::Restore,
	                           SdlTopBarButton::Close })
	{
		if (pointIn(buttonRect(layout, button), pointer))
			return button;
	}
	return SdlTopBarButton::None;
}

bool SdlTopBar::draw(const SdlTopBarRect& bar, const SDL_Rect& viewport, bool pinned,
                     const SDL_FPoint& pointer) const
{
	if (!_impl || !_impl->renderer || bar.w <= 0.0f || bar.h <= 0.0f)
		return false;

	const auto layout = layoutFor(bar, viewport);
	if (!SDL_SetRenderDrawBlendMode(_impl->renderer, SDL_BLENDMODE_BLEND))
		return false;
	if (!fillRect(_impl->renderer, { bar.x, bar.y, bar.w, bar.h }, barColor))
		return false;

	if (!setColor(_impl->renderer, barBorderColor) ||
	    !drawLine(_impl->renderer, bar.x, bar.y + bar.h - 1.0f, bar.x + bar.w,
                      bar.y + bar.h - 1.0f))
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
			case SdlTopBarButton::Compact:
				drawCompact(_impl->renderer, rect);
				break;
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

	if (!drawButton(SdlTopBarButton::Compact) || !drawButton(SdlTopBarButton::Pin) ||
	    !drawButton(SdlTopBarButton::Minimize) || !drawButton(SdlTopBarButton::Restore) ||
	    !drawButton(SdlTopBarButton::Close))
		return false;

	if (_impl->titleTexture && _impl->titleWidth > 0 && _impl->titleHeight > 0)
	{
		const auto titleX0 = bar.x + layout.grip + layout.margin;
		const auto maxWidth = std::max(0.0f, layout.compactX - layout.gap - titleX0);
		const auto widthScale =
		    std::min(1.0f, maxWidth / static_cast<float>(_impl->titleWidth));
		/* Fit the 20px font into thinner bars as well. */
		const auto heightScale =
		    std::min(1.0f, bar.h / (static_cast<float>(_impl->titleHeight) * 1.4f));
		const auto titleScale = std::min(widthScale, heightScale);
		const SDL_FRect src = { 0.0f, 0.0f, static_cast<float>(_impl->titleWidth),
		                        static_cast<float>(_impl->titleHeight) };
		const SDL_FRect dst = { titleX0, bar.y + (bar.h - _impl->titleHeight * titleScale) / 2.0f,
		                        _impl->titleWidth * titleScale, _impl->titleHeight * titleScale };
		if (titleScale > 0.0f && !SDL_RenderTexture(_impl->renderer, _impl->titleTexture.get(), &src, &dst))
			return false;
	}

	return SDL_SetRenderDrawBlendMode(_impl->renderer, SDL_BLENDMODE_NONE);
}
