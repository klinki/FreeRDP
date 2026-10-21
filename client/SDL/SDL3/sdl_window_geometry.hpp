/** Logical window content size tracking for the optional launcher bridge. */
#pragma once
#include <algorithm>
#include <optional>
#include <tuple>
#include <SDL3/SDL.h>

class SdlWindowGeometry
{
  public:
	static bool valid(SDL_Point size)
	{
		return size.x >= 64 && size.x <= 16384 && size.y >= 64 && size.y <= 16384;
	}
	static SDL_Point fit(SDL_Point size, SDL_Rect usable, int top, int left, int bottom, int right)
	{
		return { std::min(size.x, std::max(64, usable.w - left - right)),
		         std::min(size.y, std::max(64, usable.h - top - bottom)) };
	}
	SDL_Point normalSize() const { return _normal; }
	void initialize(SDL_Window* window, SDL_Point remembered)
	{
		_normal = remembered;
		_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
		_maximized = false;
		if (!_fullscreen && valid(_normal))
			restore(window);
	}
	std::optional<SDL_Point> observe(SDL_Window* window, Uint32 event)
	{
		const auto flags = SDL_GetWindowFlags(window);
		if (event == SDL_EVENT_WINDOW_MINIMIZED) return {};
		if (event == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || (flags & SDL_WINDOW_FULLSCREEN))
		{
			_fullscreen = true;
			return {};
		}
		if (event == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
		{
			_fullscreen = false;
			if (valid(_normal)) restore(window);
		}
		if (event == SDL_EVENT_WINDOW_MAXIMIZED || (flags & SDL_WINDOW_MAXIMIZED))
		{
			_maximized = true;
			return {};
		}
		if (event == SDL_EVENT_WINDOW_RESTORED)
			_maximized = false;
		if (_fullscreen || _maximized || (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)))
			return {};
		SDL_Point current{};
		if (!SDL_GetWindowSize(window, &current.x, &current.y) || !valid(current))
			return {};
		_normal = current;
		return current;
	}

  private:
	void restore(SDL_Window* window)
	{
		SDL_Rect usable{};
		if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable))
		{
			int top = 0, left = 0, bottom = 0, right = 0;
			std::ignore = SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
			_normal = fit(_normal, usable, top, left, bottom, right);
			std::ignore = SDL_SetWindowSize(window, _normal.x, _normal.y);
			// Restoring size must also keep the title bar and borders reachable.
			int x = usable.x, y = usable.y;
			std::ignore = SDL_GetWindowPosition(window, &x, &y);
			x = std::clamp(x, usable.x + left, std::max(usable.x + left, usable.x + usable.w - _normal.x - right));
			y = std::clamp(y, usable.y + top, std::max(usable.y + top, usable.y + usable.h - _normal.y - bottom));
			std::ignore = SDL_SetWindowPosition(window, x, y);
		}
		else
			std::ignore = SDL_SetWindowSize(window, _normal.x, _normal.y);
		std::ignore = SDL_SyncWindow(window);
	}
	SDL_Point _normal{};
	bool _fullscreen = false;
	bool _maximized = false;
};
