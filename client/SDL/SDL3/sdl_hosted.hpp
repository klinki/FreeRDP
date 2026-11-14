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
#include <set>
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

	/** The scale a window starts with until the launcher's first layout for it arrives. */
	static void setInitialScale(float scale) { state().initialScale.store(scale); }
	static float initialScale(float fallback)
	{
		const auto scale = state().initialScale.load();
		return scale > 0.0f ? scale : fallback;
	}

	/* Visibility. Hosted windows are never shown, raised, minimized or made fullscreen: a
	 * hidden window that is raised or restored becomes visible. Each returns true when the
	 * request needs no further action. */
	static bool show(SDL_Window* window) { return enabled() || SDL_ShowWindow(window); }
	static bool raise(SDL_Window* window) { return enabled() || SDL_RaiseWindow(window); }
	static bool restore(SDL_Window* window) { return enabled() || SDL_RestoreWindow(window); }
	static bool minimize(SDL_Window* window)
	{
		return enabled() ? ask(window, "minimize") : SDL_MinimizeWindow(window);
	}
	static bool fullscreen(SDL_Window* window, bool enter)
	{
		if (!enabled())
			return SDL_SetWindowFullscreen(window, enter);
		/* Asking for the state the launcher's window already has would only be noise. */
		return isFullscreen(SDL_GetWindowID(window)) == enter ||
		       ask(window, enter ? "enter_fullscreen" : "leave_fullscreen");
	}
	/** What the launcher last said about its window; windowed until it says otherwise.
	 * Returns whether the state changed. */
	static bool setFullscreen(SDL_WindowID id, bool fullscreen)
	{
		std::lock_guard lock(state().mutex);
		if (state().layouts.count(id) == 0)
			return false;
		return fullscreen ? state().fullscreen.insert(id).second : state().fullscreen.erase(id) != 0;
	}
	static bool isFullscreen(SDL_WindowID id)
	{
		std::lock_guard lock(state().mutex);
		return state().fullscreen.count(id) != 0;
	}
	/** Who hears that the session wants a window minimized or made fullscreen: the launcher
	 * owns the windows, so the client can only ask. Set with enable(). */
	using Request = void (*)(SDL_WindowID id, const char* action);
	static void setRequest(Request request) { state().request = request; }
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
	/* Whether the launcher shows the window at all. Nothing is drawn for a window that is not
	 * visible; the client repaints it completely when it becomes visible again. */
	static void setVisible(SDL_WindowID id, bool visible)
	{
		std::lock_guard lock(state().mutex);
		if (visible)
			state().hidden.erase(id);
		else if (state().layouts.count(id) != 0)
			state().hidden.insert(id);
	}
	static bool visible(SDL_WindowID id)
	{
		if (!enabled())
			return true;
		std::lock_guard lock(state().mutex);
		return state().hidden.count(id) == 0;
	}
	static bool visible(SDL_Window* window) { return visible(SDL_GetWindowID(window)); }
	/** True when hosted and the launcher shows none of the session windows. */
	static bool allHidden()
	{
		if (!enabled())
			return false;
		std::lock_guard lock(state().mutex);
		return !state().layouts.empty() && state().hidden.size() == state().layouts.size();
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
		state().hidden.erase(id);
		state().fullscreen.erase(id);
	}
	static std::map<SDL_WindowID, Layout> layouts()
	{
		if (!enabled())
			return {};
		std::lock_guard lock(state().mutex);
		return state().layouts;
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
		std::atomic<float> initialScale{ 0.0f };
		std::mutex mutex;
		std::map<SDL_WindowID, Layout> layouts;
		std::set<SDL_WindowID> hidden; // guarded by mutex; always a subset of layouts
		std::set<SDL_WindowID> fullscreen; // guarded by mutex; always a subset of layouts
		Output* output = nullptr;      // SDL main thread only
		Request request = nullptr;     // set before the first session window is created
	};
	static bool ask(SDL_Window* window, const char* action)
	{
		if (state().request)
			state().request(SDL_GetWindowID(window), action);
		return true;
	}
	static State& state()
	{
		static State instance;
		return instance;
	}
};
