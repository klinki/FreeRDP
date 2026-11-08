/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL3 session fullscreen and connection-bar state.
 *
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 *
 * Native window fullscreen events are the source of truth. A request (the
 * session toggle, the bar's restore button, initial connection) only records
 * what was asked for; each window's state changes when SDL reports that it
 * actually entered or left fullscreen, whichever control caused it (FreeRDP's
 * toggle, the macOS green button, the Window menu, a failed transition).
 *
 * The connection bar belongs to one window. It is shown when its owner enters
 * fullscreen and hidden when the owner leaves, so leaving with the session
 * toggle and re-entering natively restores it. Other windows entering or
 * leaving fullscreen never affect the bar. The pin state is a user choice and
 * is never changed by fullscreen transitions.
 */
#pragma once

#include <cstdint>
#include <map>

#include <SDL3/SDL.h>

class SdlFullscreenState
{
  public:
	/* A request that has not produced its native event in this time no longer
	 * decides the direction of the next toggle; the actual state does. */
	static constexpr uint64_t pendingTimeoutMs = 5000;

	struct Observation
	{
		bool changed = false;   // the window's actual state changed
		bool owner = false;     // ... and it is the bar's window
		bool requested = false; // ... and the change answered a request
	};

	void clear()
	{
		_windows.clear();
		barOwner = 0;
		barPinned = true;
		barVisible = true;
		_requestAt = 0;
	}

	/* Forget the windows (they are being recreated) but keep the bar's pin
	 * and visibility choices. */
	void resetWindows()
	{
		_windows.clear();
		barOwner = 0;
		_requestAt = 0;
	}

	/* Windows start out windowed: a window created with the fullscreen flag
	 * is only reported fullscreen once SDL sends its enter event. */
	void addWindow(SDL_WindowID id)
	{
		_windows[id] = Window{};
		if (barOwner == 0)
			barOwner = id;
	}

	void removeWindow(SDL_WindowID id)
	{
		_windows.erase(id);
		if (barOwner == id)
			barOwner = 0;
	}

	/* Give the bar to `id` if it has no owner. The bar then follows the new
	 * owner's actual state, as if that window had just entered or left. */
	void ensureOwner(SDL_WindowID id)
	{
		if (barOwner == 0)
			setOwner(id, true);
	}

	/* Make `id` the bar's window. With `followState` the bar takes that
	 * window's actual state; without it the visibility choice is kept, for
	 * windows that are only being created. */
	void setOwner(SDL_WindowID id, bool followState)
	{
		if (_windows.find(id) == _windows.end())
			return;
		barOwner = id;
		if (followState)
			barVisible = windowFullscreen(id);
	}

	[[nodiscard]] bool hasWindow(SDL_WindowID id) const
	{
		return _windows.find(id) != _windows.end();
	}

	/* A deliberate session-wide request: every window should enter or leave. */
	void request(bool enter, uint64_t now)
	{
		_target = enter;
		_requestAt = now;
		for (auto& window : _windows)
			window.second.pending = true;
	}

	[[nodiscard]] bool pending(SDL_WindowID id, uint64_t now) const
	{
		const auto it = _windows.find(id);
		return it != _windows.end() && it->second.pending && live(now);
	}

	[[nodiscard]] bool anyPending(uint64_t now) const
	{
		if (!live(now))
			return false;
		for (const auto& window : _windows)
			if (window.second.pending)
				return true;
		return false;
	}

	[[nodiscard]] bool target() const
	{
		return _target;
	}

	/* SDL refused the transition. Forget the request for this window and
	 * record what the window actually is. */
	Observation requestFailed(SDL_WindowID id, bool actual)
	{
		auto it = _windows.find(id);
		if (it == _windows.end())
			return {};
		it->second.pending = false;
		return observe(id, actual);
	}

	/* A native enter or leave event, or the settled state of a shown window
	 * after a request (SDL sends no event when it is already in that state). */
	Observation observe(SDL_WindowID id, bool fullscreen)
	{
		auto it = _windows.find(id);
		if (it == _windows.end())
			return {};
		Observation result;
		if (it->second.pending && _target == fullscreen)
		{
			it->second.pending = false;
			result.requested = true;
		}
		if (it->second.fullscreen == fullscreen)
			return result;
		it->second.fullscreen = fullscreen;
		result.changed = true;
		if (id == barOwner)
		{
			result.owner = true;
			barVisible = fullscreen;
		}
		return result;
	}

	[[nodiscard]] bool windowFullscreen(SDL_WindowID id) const
	{
		const auto it = _windows.find(id);
		return it != _windows.end() && it->second.fullscreen;
	}

	[[nodiscard]] bool ownerFullscreen() const
	{
		return windowFullscreen(barOwner);
	}

	[[nodiscard]] bool allFullscreen() const
	{
		if (_windows.empty())
			return false;
		for (const auto& window : _windows)
			if (!window.second.fullscreen)
				return false;
		return true;
	}

	[[nodiscard]] bool anyFullscreen() const
	{
		for (const auto& window : _windows)
			if (window.second.fullscreen)
				return true;
		return false;
	}

	/* What the session is: the request in flight, otherwise fullscreen only
	 * when every window actually is. A mixed session is not fullscreen. */
	[[nodiscard]] bool sessionFullscreen(uint64_t now) const
	{
		if (anyPending(now))
			return _target;
		return allFullscreen();
	}

	/* Direction of a session toggle invoked from window `origin` (the focused
	 * window for the keyboard shortcut, the bar's window for its button).
	 * A toggle in flight is reversed. Otherwise a fullscreen origin takes the
	 * whole session out of fullscreen and a windowed one takes it all in, so
	 * a mixed session is made consistent in the direction the user expects
	 * from the window in front of them. An unknown origin decides by the
	 * session as a whole. */
	[[nodiscard]] bool toggleTarget(SDL_WindowID origin, uint64_t now) const
	{
		if (anyPending(now))
			return !_target;
		if (hasWindow(origin))
			return !windowFullscreen(origin);
		return !allFullscreen();
	}

	/* Whether `window` draws the bar. Multi-monitor sessions also allow it
	 * while windowed, where an unpinned bar can be revealed by hovering. */
	[[nodiscard]] bool barShown(SDL_WindowID window, bool multimon) const
	{
		return barVisible && window != 0 && window == barOwner &&
		       (ownerFullscreen() || multimon);
	}

	[[nodiscard]] bool barAllowed(bool multimon) const
	{
		return barOwner != 0 && (ownerFullscreen() || multimon);
	}

	SDL_WindowID barOwner = 0;
	bool barPinned = true;
	bool barVisible = true;

  private:
	struct Window
	{
		bool fullscreen = false;
		bool pending = false;
	};

	[[nodiscard]] bool live(uint64_t now) const
	{
		return now - _requestAt < pendingTimeoutMs;
	}

	std::map<SDL_WindowID, Window> _windows;
	bool _target = false;
	uint64_t _requestAt = 0;
};
