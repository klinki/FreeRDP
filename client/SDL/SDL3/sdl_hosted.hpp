/** Hosted rendering: the launcher owns the session windows (launcher-bridge.md, "Hosted
 * rendering"). This is the one place that answers whether the process is hosted. Every call
 * that would put a session window on screen, and every read of a session window's pixel size
 * or scale, goes through here, so that a new call site cannot reveal a window or trust the
 * geometry of a window that is never shown. */
#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <SDL3/SDL.h>

class SdlHosted
{
  public:
	/** What the launcher shows a window as: its size in pixels and its backing scale. */
	struct Layout
	{
		int width = 0;
		int height = 0;
		float scale = 1.0f;
	};

	static bool valid(const Layout& layout)
	{
		return layout.width >= 1 && layout.width <= 16384 && layout.height >= 1 &&
		       layout.height <= 16384 && layout.scale >= 0.5f && layout.scale <= 8.0f;
	}

	/** Where a hosted window's picture goes instead of the window. */
	class Output
	{
	  public:
		virtual ~Output() = default;
		/** The texture to draw the next picture into; nullptr when there is nowhere to draw. */
		virtual SDL_Texture* target(SDL_Window* window, SDL_Renderer* renderer) = 0;
		/** The picture drawn into target() is complete. */
		virtual bool present(SDL_Window* window, SDL_Renderer* renderer) = 0;
		/** The window is going away; called while its renderer still exists. */
		virtual void forget(SDL_WindowID id) = 0;
	};
	/** Set with enable(), before the first session window is created. Not owned. */
	static void setOutput(Output* output) { state().output = output; }

	static bool enabled() { return state().enabled.load(); }
	/** Set once, before the first session window is created. */
	static void enable(bool on) { state().enabled.store(on); }

	/* Visibility. Hosted windows are never shown, raised, minimized or made fullscreen: a
	 * hidden window that is raised or restored becomes visible. Each returns true when the
	 * request needs no further action. */
	static bool show(SDL_Window* window) { return enabled() || SDL_ShowWindow(window); }
	static bool raise(SDL_Window* window) { return enabled() || SDL_RaiseWindow(window); }
	static bool restore(SDL_Window* window) { return enabled() || SDL_RestoreWindow(window); }
	static bool minimize(SDL_Window* window) { return enabled() || SDL_MinimizeWindow(window); }
	static bool fullscreen(SDL_Window* window, bool enter)
	{
		return enabled() || SDL_SetWindowFullscreen(window, enter);
	}
	static bool grabKeyboard(SDL_Window* window, bool grab)
	{
		return enabled() || SDL_SetWindowKeyboardGrab(window, grab);
	}
	static bool grabMouse(SDL_Window* window, bool grab)
	{
		return enabled() || SDL_SetWindowMouseGrab(window, grab);
	}

	/* Geometry. A hidden window keeps the backing scale of the display it was created on, so
	 * its own pixel size and scale are wrong as soon as the launcher shows it elsewhere. */
	static bool setLayout(SDL_WindowID id, const Layout& layout)
	{
		if (!enabled() || id == 0 || !valid(layout))
			return false;
		std::lock_guard lock(state().mutex);
		state().layouts[id] = layout;
		return true;
	}
	/* Presentation. "The screen" of a hosted window is the output's current texture. */
	static bool screenTarget(SDL_Window* window, SDL_Renderer* renderer)
	{
		if (!enabled())
			return SDL_SetRenderTarget(renderer, nullptr);
		auto texture = state().output ? state().output->target(window, renderer) : nullptr;
		return texture && SDL_SetRenderTarget(renderer, texture);
	}
	static bool present(SDL_Window* window, SDL_Renderer* renderer)
	{
		if (!enabled())
			return SDL_RenderPresent(renderer);
		return state().output && state().output->present(window, renderer);
	}

	static void forget(SDL_WindowID id)
	{
		if (state().output)
			state().output->forget(id);
		std::lock_guard lock(state().mutex);
		state().layouts.erase(id);
	}
	static std::optional<Layout> layout(SDL_WindowID id)
	{
		if (!enabled())
			return std::nullopt;
		std::lock_guard lock(state().mutex);
		const auto it = state().layouts.find(id);
		if (it == state().layouts.end())
			return std::nullopt;
		return it->second;
	}
	static bool pixelSize(SDL_Window* window, int* width, int* height)
	{
		if (const auto hosted = layout(SDL_GetWindowID(window)))
		{
			if (width)
				*width = hosted->width;
			if (height)
				*height = hosted->height;
			return true;
		}
		return SDL_GetWindowSizeInPixels(window, width, height);
	}
	static float pixelDensity(SDL_Window* window)
	{
		if (const auto hosted = layout(SDL_GetWindowID(window)))
			return hosted->scale;
		return SDL_GetWindowPixelDensity(window);
	}
	static float displayScale(SDL_Window* window)
	{
		if (const auto hosted = layout(SDL_GetWindowID(window)))
			return hosted->scale;
		return SDL_GetWindowDisplayScale(window);
	}

  private:
	struct State
	{
		std::atomic<bool> enabled{ false };
		std::mutex mutex;
		std::map<SDL_WindowID, Layout> layouts;
		Output* output = nullptr; // SDL main thread only
	};
	static State& state()
	{
		static State instance;
		return instance;
	}
};
