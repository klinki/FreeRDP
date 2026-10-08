/** Session stats overlay. Licensed under Apache-2.0. */
#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "dialogs/res/sdl3_resource_manager.hpp"
class SdlPerformanceOverlay
{
  public:
	enum class Style { Panel, Quake };
	enum Action { None, Drag, Details, Hide };
	explicit SdlPerformanceOverlay(SDL_Renderer* renderer) : renderer(renderer) {}
	~SdlPerformanceOverlay()
	{
		if (captured) SDL_CaptureMouse(false);
		clearTextures();
		if (font) TTF_CloseFont(font);
	}
	void setVisible(bool show, Style appearance)
	{
		visible = show;
		if (style != appearance)
		{
			style = appearance;
			dirty = true;
			// Complete an already-local mouse gesture without activating an action
			// that has moved because the style's layout changed.
			if (captured) pressed = None;
		}
	}
	void set(bool show, std::string value, Style appearance = Style::Panel)
	{
		setVisible(show, appearance);
		if (value != text) { text = std::move(value); dirty = true; }
	}
	bool capturing() const { return captured; }
	Style appearance() const { return style; }
	SDL_FRect bounds() const { return rect; }
	bool draw(const SDL_Rect& viewport, float density)
	{
		if (!visible) return true;
		scale = std::max(density, 1.0f);
		const bool quake = style == Style::Quake;
		const bool light = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT;
		if (light != lightTheme) { lightTheme = light; dirty = true; }
		const float width = std::min(300.0f * scale, static_cast<float>(viewport.w));
		if (!positioned)
		{
			rect = { viewport.w - width - 12 * scale, 70 * scale, width, 0 };
			positioned = true;
		}
		if (rect.w != width) dirty = true;
		rect.w = width;
		const float padding = (quake ? 2 : 12) * scale;
		const float pixels = (quake ? 14 : 16) * scale;
		if (fontPixels != pixels)
		{
			if (font) TTF_CloseFont(font);
			font = nullptr;
			clearTextures();
			dirty = true;
			fontPixels = pixels;
			auto io = SDL3ResourceManager::get(SDLResourceManager::typeFonts(),
			                                   "OpenSans-VariableFont_wdth,wght.ttf");
			if (io) font = TTF_OpenFontIO(io, true, pixels);
		}
		if (dirty && font)
		{
			clearTextures();
			const auto color = quake || !light ? SDL_Color{239, 243, 250, 255}
			                                  : SDL_Color{18, 26, 38, 255};
			const std::string body = quake ? text : "Performance — drag to move\n" + text;
			const int wrap = static_cast<int>(std::max(1.0f, width - 2 * padding));
			makeTexture(body, wrap, color, quake, texture, tw, th);
			makeTexture("Details", 0, color, quake, detailsTexture, detailsWidth, detailsHeight);
			makeTexture("Hide", 0, color, quake, hideTexture, hideWidth, hideHeight);
			dirty = false;
		}
		footerHeight = std::max(detailsHeight, hideHeight);
		rect.h = std::min(th + footerHeight + 2 * padding + 8 * scale,
		                  static_cast<float>(viewport.h));
		rect.x = std::clamp(rect.x, 0.0f, std::max(0.0f, viewport.w - rect.w));
		rect.y = std::clamp(rect.y, 0.0f, std::max(0.0f, viewport.h - rect.h));
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
		if (!quake)
		{
			if (light) SDL_SetRenderDrawColor(renderer, 244, 247, 252, 240);
			else SDL_SetRenderDrawColor(renderer, 22, 30, 44, 235);
			SDL_RenderFillRect(renderer, &rect);
			SDL_SetRenderDrawColor(renderer, 100, 135, 180, 255);
			SDL_RenderRect(renderer, &rect);
		}
		// Clip small drawable areas, preserving the renderer's prior clipping.
		const bool clipped = SDL_RenderClipEnabled(renderer);
		SDL_Rect previous{};
		SDL_GetRenderClipRect(renderer, &previous);
		SDL_Rect clip{static_cast<int>(rect.x), static_cast<int>(rect.y),
		              static_cast<int>(rect.w), static_cast<int>(rect.h)};
		SDL_SetRenderClipRect(renderer, &clip);
		drawTexture(texture, rect.x + padding, rect.y + padding, tw, th);
		const float footerY = rect.y + rect.h - padding - footerHeight;
		drawTexture(detailsTexture, rect.x + padding, footerY, detailsWidth, detailsHeight);
		drawTexture(hideTexture, rect.x + rect.w - padding - hideWidth, footerY, hideWidth, hideHeight);
		SDL_SetRenderClipRect(renderer, clipped ? &previous : nullptr);
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
		return true;
	}
	Action hit(SDL_FPoint p) const
	{
		if (!visible || p.x < rect.x || p.x >= rect.x + rect.w ||
		    p.y < rect.y || p.y >= rect.y + rect.h) return None;
		if (p.y < rect.y + (style == Style::Quake ? 24 : 29) * scale) return Drag;
		const float padding = (style == Style::Quake ? 2 : 12) * scale;
		if (p.y >= rect.y + rect.h - padding - footerHeight - 4 * scale)
			return p.x < rect.x + rect.w * 0.65f ? Details : Hide;
		return None;
	}
	bool button(const SDL_MouseButtonEvent& e, SDL_FPoint p, Action& action)
	{
		action = None;
		if (captured)
		{
			if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button == SDL_BUTTON_LEFT)
			{
				if (pressed != Drag && hit(p) == pressed) action = pressed;
				captured = false; pressed = None; SDL_CaptureMouse(false);
			}
			return true;
		}
		if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button != SDL_BUTTON_LEFT) return false;
		pressed = hit(p);
		if (pressed == None) return false;
		captured = true;
		grab = {p.x - rect.x, p.y - rect.y};
		SDL_CaptureMouse(true);
		return true;
	}
	bool motion(SDL_FPoint p)
	{
		if (!captured) return false;
		if (pressed == Drag) { rect.x = p.x - grab.x; rect.y = p.y - grab.y; }
		return true;
	}

  private:
	void makeTexture(const std::string& value, int wrap, SDL_Color color, bool outline,
	                 SDL_Texture*& result, float& width, float& height)
	{
		auto surface = TTF_RenderText_Blended_Wrapped(font, value.c_str(), 0, color, wrap);
		if (!surface) return;
		if (outline)
		{
			const int edge = static_cast<int>(std::ceil(scale));
			auto composed = SDL_CreateSurface(surface->w + 2 * edge, surface->h + 2 * edge,
			                                  SDL_PIXELFORMAT_RGBA32);
			if (composed)
			{
				SDL_FillSurfaceRect(composed, nullptr, 0);
				SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
				SDL_SetSurfaceColorMod(surface, 0, 0, 0);
				for (int y : {-edge, 0, edge}) for (int x : {-edge, 0, edge})
				{
					if (x == 0 && y == 0) continue;
					SDL_Rect destination{edge + x, edge + y, surface->w, surface->h};
					SDL_BlitSurface(surface, nullptr, composed, &destination);
				}
				SDL_SetSurfaceColorMod(surface, 255, 255, 255);
				SDL_Rect destination{edge, edge, surface->w, surface->h};
				SDL_BlitSurface(surface, nullptr, composed, &destination);
				SDL_DestroySurface(surface);
				surface = composed;
			}
		}
		result = SDL_CreateTextureFromSurface(renderer, surface);
		width = surface->w; height = surface->h;
		SDL_DestroySurface(surface);
	}
	void drawTexture(SDL_Texture* value, float x, float y, float width, float height)
	{
		if (value) { SDL_FRect destination{x, y, width, height}; SDL_RenderTexture(renderer, value, nullptr, &destination); }
	}
	void clearTextures()
	{
		for (auto value : {texture, detailsTexture, hideTexture}) if (value) SDL_DestroyTexture(value);
		texture = detailsTexture = hideTexture = nullptr;
		tw = th = detailsWidth = detailsHeight = hideWidth = hideHeight = 0;
	}
	SDL_Renderer* renderer;
	TTF_Font* font = nullptr;
	SDL_Texture *texture = nullptr, *detailsTexture = nullptr, *hideTexture = nullptr;
	std::string text = "Starting…";
	Style style = Style::Panel;
	SDL_FRect rect{};
	SDL_FPoint grab{};
	float tw = 0, th = 0, detailsWidth = 0, detailsHeight = 0, hideWidth = 0, hideHeight = 0;
	float footerHeight = 0, scale = 1, fontPixels = 0;
	bool visible = false, positioned = false, captured = false, dirty = true, lightTheme = false;
	Action pressed = None;
};
