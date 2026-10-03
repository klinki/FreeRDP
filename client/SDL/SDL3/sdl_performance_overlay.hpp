/** Session stats overlay. Licensed under Apache-2.0. */
#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <string>
#include "dialogs/res/sdl3_resource_manager.hpp"
class SdlPerformanceOverlay
{
  public:
	enum Action
	{
		None,
		Drag,
		Details,
		Hide
	};
	explicit SdlPerformanceOverlay(SDL_Renderer* renderer) : renderer(renderer)
	{
	}
	~SdlPerformanceOverlay()
	{
		if (captured)
			SDL_CaptureMouse(false);
		clearTexture();
		if (font)
			TTF_CloseFont(font);
	}
	void set(bool show, std::string value)
	{
		visible = show;
		if (value != text)
		{
			text = std::move(value);
			dirty = true;
		}
	}
	bool capturing() const
	{
		return captured;
	}
	bool draw(const SDL_Rect& viewport, float density)
	{
		if (!visible)
			return true;
		scale = std::max(density, 1.0f);
		const float currentWidth = rect.w;
		const bool light = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT;
		if (light != lightTheme)
		{
			lightTheme = light;
			dirty = true;
		}
		const float width = std::min(300.0f * scale, static_cast<float>(viewport.w));
		const float height = std::min(160.0f * scale, static_cast<float>(viewport.h));
		if (!positioned)
		{
			rect = { viewport.w - width - 12 * scale, 70 * scale, width, height };
			positioned = true;
		}
		rect.w = width;
		rect.h = height;
		if (currentWidth != width)
			dirty = true;
		if (currentWidth != width)
			dirty = true;
		rect.x = std::clamp(rect.x, 0.0f, std::max(0.0f, viewport.w - width));
		rect.y = std::clamp(rect.y, 0.0f, std::max(0.0f, viewport.h - height));
		if (fontScale != scale)
		{
			if (font)
				TTF_CloseFont(font);
			font = nullptr;
			clearTexture();
			dirty = true;
			fontScale = scale;
			auto io = SDL3ResourceManager::get(SDLResourceManager::typeFonts(),
			                                   "OpenSans-VariableFont_wdth,wght.ttf");
			if (io)
				font = TTF_OpenFontIO(io, true, 16 * scale);
		}
		if (dirty && font)
		{
			clearTexture();
			const auto color =
			    light ? SDL_Color{ 18, 26, 38, 255 } : SDL_Color{ 239, 243, 250, 255 };
			auto surface = TTF_RenderText_Blended_Wrapped(
			    font, text.c_str(), 0, color, static_cast<int>(std::max(1.0f, width - 24 * scale)));
			if (surface)
			{
				texture = SDL_CreateTextureFromSurface(renderer, surface);
				tw = surface->w;
				th = surface->h;
				SDL_DestroySurface(surface);
			}
			dirty = false;
		}
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
		if (light)
			SDL_SetRenderDrawColor(renderer, 244, 247, 252, 240);
		else
			SDL_SetRenderDrawColor(renderer, 22, 30, 44, 235);
		SDL_RenderFillRect(renderer, &rect);
		SDL_SetRenderDrawColor(renderer, 100, 135, 180, 255);
		SDL_RenderRect(renderer, &rect);
		if (texture)
		{
			SDL_FRect dst{ rect.x + 12 * scale, rect.y + 8 * scale, tw, th };
			SDL_RenderTexture(renderer, texture, nullptr, &dst);
		}
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
		return true;
	}
	Action hit(SDL_FPoint p) const
	{
		if (!visible || p.x < rect.x || p.x >= rect.x + rect.w || p.y < rect.y ||
		    p.y >= rect.y + rect.h)
			return None;
		if (p.y < rect.y + 29 * scale)
			return Drag;
		if (p.y >= rect.y + rect.h - 31 * scale)
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
				if (pressed != Drag && hit(p) == pressed)
					action = pressed;
				captured = false;
				pressed = None;
				SDL_CaptureMouse(false);
			}
			return true;
		}
		if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button != SDL_BUTTON_LEFT)
			return false;
		pressed = hit(p);
		if (pressed == None)
			return false;
		captured = true;
		grab = { p.x - rect.x, p.y - rect.y };
		SDL_CaptureMouse(true);
		return true;
	}
	bool motion(SDL_FPoint p)
	{
		if (!captured)
			return false;
		if (pressed == Drag)
		{
			rect.x = p.x - grab.x;
			rect.y = p.y - grab.y;
		}
		return true;
	}

  private:
	void clearTexture()
	{
		if (texture)
			SDL_DestroyTexture(texture);
		texture = nullptr;
	}
	SDL_Renderer* renderer;
	TTF_Font* font = nullptr;
	SDL_Texture* texture = nullptr;
	std::string text;
	SDL_FRect rect{};
	SDL_FPoint grab{};
	float tw = 0, th = 0, scale = 1, fontScale = 0;
	bool visible = false, positioned = false, captured = false, dirty = true, lightTheme = false;
	Action pressed = None;
};
