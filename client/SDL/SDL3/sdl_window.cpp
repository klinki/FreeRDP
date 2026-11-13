/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
 * Copyright 2023 Thincast Technologies GmbH
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
#include <limits>
#include <sstream>
#include <cmath>

#include <SDL3_ttf/SDL_ttf.h>

#include "sdl_window.hpp"
#include "sdl_hosted.hpp"
#ifdef WITH_SDL_LAUNCHER_BRIDGE
#include "sdl_launcher.hpp"
#endif
#include "sdl_render_geometry.hpp"
#include "sdl_utils.hpp"

#include "dialogs/res/sdl3_resource_manager.hpp"

#include <freerdp/utils/string.h>

namespace
{
/* Stalled-overlay tuning: screen dim alpha (128/255 ≈ 50%, so the session
 * reads as clearly faded over bright content) and indicator font size. */
constexpr Uint8 stalledDimAlpha = 128;
constexpr float stalledFontPts = 32.0f;
constexpr SDL_Color stalledTextColor = { 0xf2, 0xf6, 0xfb, 0xff };
constexpr SDL_Color stalledShadowColor = { 0x00, 0x00, 0x00, 0xc0 };

[[nodiscard]] SDL_FRect scaledDestination(const SDL_Rect& srcRect, const SDL_FPoint& scale)
{
	float ix = 0.0f;
	float iy = 0.0f;
	const auto modx = std::modf(static_cast<float>(srcRect.x) * scale.x, &ix);
	const auto mody = std::modf(static_cast<float>(srcRect.y) * scale.y, &iy);
	const auto sw = std::ceil(static_cast<float>(srcRect.w) * scale.x) + std::ceil(modx);
	const auto sh = std::ceil(static_cast<float>(srcRect.h) * scale.y) + std::ceil(mody);
	return { static_cast<float>(static_cast<Sint32>(ix)),
	         static_cast<float>(static_cast<Sint32>(iy)),
	         static_cast<float>(static_cast<Sint32>(sw)),
	         static_cast<float>(static_cast<Sint32>(sh)) };
}
} // namespace

SdlWindow::SdlWindow(SDL_DisplayID id, const std::string& title, const SDL_Rect& rect,
                     [[maybe_unused]] Uint32 flags)
    : _initialW(rect.w), _initialH(rect.h), _displayID(id)
{
	float pd = SDL_GetDisplayContentScale(id);
	if (pd <= 0.0f)
		pd = 1.0f;
	const int createW = static_cast<int>(std::ceil(static_cast<float>(rect.w) / pd));
	const int createH = static_cast<int>(std::ceil(static_cast<float>(rect.h) / pd));

	auto props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, createW);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, createH);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);

	if (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY)
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);

	if (flags & SDL_WINDOW_FULLSCREEN)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);

	if (flags & SDL_WINDOW_BORDERLESS)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);

	if (flags & SDL_WINDOW_TRANSPARENT)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);

	/* RAIL windows are created hidden until first paint to avoid black flash. */
	if (flags & SDL_WINDOW_HIDDEN)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);

	_window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
	_renderMetrics.setIdentity(_window ? SDL_GetWindowID(_window) : 0, id);
	SDL_SetHint(SDL_HINT_APP_NAME, "");
	std::ignore = SDL_SyncWindow(_window);

	/* Hosted: the requested size is in pixels and the launcher's scale is not known yet, so
	 * start from this display's; the launcher's first layout replaces it. */
	if (_window)
		std::ignore = SdlHosted::setLayout(SDL_GetWindowID(_window), { rect.w, rect.h, pd });

	_renderer = SDL_CreateRenderer(_window, nullptr);
	_topBar = std::make_unique<SdlTopBar>(_renderer, title);

	std::ignore = resizeToScale();

	_monitor = query(_window, id, true);
}

SdlWindow::SdlWindow(SdlWindow&& other) noexcept
    : _window(other._window), _renderer(other._renderer), _renderTarget(other._renderTarget),
      _gdiTexture(other._gdiTexture), _gdiTextureW(other._gdiTextureW),
      _gdiTextureH(other._gdiTextureH),
      _renderTargetNeedsFullRedraw(other._renderTargetNeedsFullRedraw),
      _gdiTextureNeedsFullRedraw(other._gdiTextureNeedsFullRedraw), _initialW(other._initialW),
      _initialH(other._initialH), _hostedRevealed(other._hostedRevealed), _displayID(other._displayID), _offset_x(other._offset_x),
      _offset_y(other._offset_y),
      _renderMetrics(std::move(other._renderMetrics)), _liveMetrics(std::move(other._liveMetrics)),
      _livePresents(other._livePresents), _performanceOverlay(std::move(other._performanceOverlay)), _monitor(other._monitor),
      _displaySize(other._displaySize),
      _topBar(std::move(other._topBar))
{
	other._window = nullptr;
	other._renderer = nullptr;
	other._renderTarget = nullptr;
	other._gdiTexture = nullptr;
	other._renderTargetNeedsFullRedraw = false;
	other._gdiTextureNeedsFullRedraw = false;
}

SdlWindow::~SdlWindow()
{
	/* Release owned textures before SDL_DestroyRenderer destroys them. */
	_performanceOverlay.reset();
	_topBar.reset();
	_stalled.reset();
	if (_gdiTexture)
		SDL_DestroyTexture(_gdiTexture);
	if (_renderTarget)
		SDL_DestroyTexture(_renderTarget);
	if (_renderer)
		SDL_DestroyRenderer(_renderer);
	if (_window)
	{
		SdlHosted::forget(SDL_GetWindowID(_window));
		SDL_DestroyWindow(_window);
	}
}

SDL_WindowID SdlWindow::id() const
{
	if (!_window)
		return 0;
	return SDL_GetWindowID(_window);
}

SDL_DisplayID SdlWindow::displayIndex() const
{
	if (!_window)
		return 0;
	return SDL_GetDisplayForWindow(_window);
}

SDL_Rect SdlWindow::rect() const
{
	return rect(_window);
}

SDL_Rect SdlWindow::bounds() const
{
	SDL_Rect rect = {};
	if (_window)
	{
		if (!SDL_GetWindowPosition(_window, &rect.x, &rect.y))
			return {};
		if (!SDL_GetWindowSize(_window, &rect.w, &rect.h))
			return {};
	}
	return rect;
}

SDL_Window* SdlWindow::window() const
{
	return _window;
}

SDL_Renderer* SdlWindow::renderer() const
{
	return _renderer;
}

Sint32 SdlWindow::offsetX() const
{
	return _offset_x;
}

void SdlWindow::setOffsetX(Sint32 x)
{
	_offset_x = x;
}

void SdlWindow::setOffsetY(Sint32 y)
{
	_offset_y = y;
}

Sint32 SdlWindow::offsetY() const
{
	return _offset_y;
}

rdpMonitor SdlWindow::monitor(bool isPrimary) const
{
	auto m = _monitor;
	if (isPrimary)
	{
		m.x = 0;
		m.y = 0;
	}
	return m;
}

void SdlWindow::setMonitor(rdpMonitor monitor, bool display)
{
	_monitor = monitor;
	if (display)
		_displaySize = { monitor.width, monitor.height };
}

SDL_Point SdlWindow::displaySize() const
{
	return _displaySize;
}

float SdlWindow::scale() const
{
	return SdlHosted::displayScale(_window);
}

SDL_DisplayOrientation SdlWindow::orientation() const
{
	const auto did = displayIndex();
	return SDL_GetCurrentDisplayOrientation(did);
}

bool SdlWindow::grabKeyboard(bool enable)
{
	if (!_window)
		return false;
	std::ignore = SdlHosted::grabKeyboard(_window, enable);
	return true;
}

bool SdlWindow::grabMouse(bool enable)
{
	if (!_window)
		return false;
	std::ignore = SdlHosted::grabMouse(_window, enable);
	return true;
}

void SdlWindow::setBordered(bool bordered)
{
	if (_window)
		SDL_SetWindowBordered(_window, bordered);
	std::ignore = SDL_SyncWindow(_window);
}

void SdlWindow::raise()
{
	std::ignore = SdlHosted::raise(_window);
	std::ignore = SDL_SyncWindow(_window);
}

bool SdlWindow::revealed() const
{
	if (SdlHosted::enabled())
		return _hostedRevealed;
	return (SDL_GetWindowFlags(_window) & SDL_WINDOW_HIDDEN) == 0;
}

void SdlWindow::reveal()
{
	_hostedRevealed = true;
	std::ignore = SdlHosted::show(_window);
	std::ignore = SdlHosted::raise(_window);
}

void SdlWindow::resizeable(bool use)
{
	SDL_SetWindowResizable(_window, use);
	std::ignore = SDL_SyncWindow(_window);
}

SdlWindow::FullscreenResult SdlWindow::fullscreen(bool enter, bool forceOriginalDisplay)
{
	if (enter && forceOriginalDisplay && _displayID != 0)
	{
		/* Move the window to the desired display. We should not wait
		 * for the window to be moved, because some backends can refuse
		 * the move. The intent of moving the window is enough for SDL
		 * to decide which display will be used for fullscreen. */
		SDL_Rect rect = {};
		std::ignore = SDL_GetDisplayBounds(_displayID, &rect);
		std::ignore = SDL_SetWindowPosition(_window, rect.x, rect.y);
	}
	if (!SdlHosted::fullscreen(_window, enter))
		return FullscreenResult::Failed;
	/* Native transitions (macOS Spaces) are asynchronous; SDL_SyncWindow
	 * gives up after a timeout while the transition may still complete. The
	 * window's enter/leave event decides the actual state either way. */
	if (!SDL_SyncWindow(_window))
		return FullscreenResult::Unsynced;
	return FullscreenResult::Applied;
}

void SdlWindow::minimize()
{
	std::ignore = SdlHosted::minimize(_window);
	std::ignore = SDL_SyncWindow(_window);
}

bool SdlWindow::resizeToScale()
{
	if (!_window || _initialW <= 0 || _initialH <= 0)
		return false;
	if ((SDL_GetWindowFlags(_window) & SDL_WINDOW_FULLSCREEN) != 0)
		return true;

	float pd = SdlHosted::pixelDensity(_window);
	if (pd <= 0.0f)
		pd = 1.0f;

	const int targetW = static_cast<int>(std::ceil(static_cast<float>(_initialW) / pd));
	const int targetH = static_cast<int>(std::ceil(static_cast<float>(_initialH) / pd));

	int curW = 0;
	int curH = 0;
	if (!SDL_GetWindowSize(_window, &curW, &curH))
		return false;

	if (curW == targetW && curH == targetH)
		return true;

	return resize({ targetW, targetH });
}

bool SdlWindow::resize(const SDL_Point& size)
{
	return SDL_SetWindowSize(_window, size.x, size.y);
}

bool SdlWindow::ensureRenderTarget()
{
	if (!_renderer)
		return false;

	int w = 0;
	int h = 0;
	if (!SdlHosted::pixelSize(_window, &w, &h))
		return false;
	if (w <= 0 || h <= 0)
		return false;

	/* Recreate if missing or if window size changed */
	if (_renderTarget)
	{
		float tw = 0;
		float th = 0;
		if (SDL_GetTextureSize(_renderTarget, &tw, &th) && static_cast<int>(tw) == w &&
		    static_cast<int>(th) == h)
			return true;
		SDL_DestroyTexture(_renderTarget);
		_renderTarget = nullptr;
	}

	_renderTarget =
	    SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_TARGET, w, h);
	if (!_renderTarget)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateTexture (render target): %s",
		             SDL_GetError());
		return false;
	}
	/* Verbatim, never blended: a transparent window's alpha<0xFF regions would render black. */
	std::ignore = SDL_SetTextureBlendMode(_renderTarget, SDL_BLENDMODE_NONE);
	_renderTargetNeedsFullRedraw = true;

	/* SDL does not guarantee the contents of a newly created target. Clear it
	 * (transparent, so windows never flash black) before any presentation, and
	 * retain the full-redraw flag until a source upload has painted the target. */
	if (!SDL_SetRenderTarget(_renderer, _renderTarget) ||
	    !SDL_SetRenderDrawColor(_renderer, 0x00, 0x00, 0x00, 0x00) || !SDL_RenderClear(_renderer))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_RenderClear (render target): %s",
		             SDL_GetError());
		(void)SDL_SetRenderTarget(_renderer, nullptr);
		SDL_DestroyTexture(_renderTarget);
		_renderTarget = nullptr;
		return false;
	}
	_renderMetrics.noteTargetRecreate();
	return true;
}

bool SdlWindow::needsFullRedraw() const
{
	return _renderTargetNeedsFullRedraw || _gdiTextureNeedsFullRedraw;
}

bool SdlWindow::needsFullRedraw(int surfaceWidth, int surfaceHeight) const
{
	return needsFullRedraw() || !_gdiTexture || _gdiTextureW != surfaceWidth ||
	       _gdiTextureH != surfaceHeight;
}

bool SdlWindow::drawRect(SDL_Surface* surface, SDL_Point offset, const SDL_Rect& srcRect)
{
	WINPR_ASSERT(surface);
	SDL_Rect dstRect = { offset.x + srcRect.x, offset.y + srcRect.y, srcRect.w, srcRect.h };
	return blit(surface, srcRect, dstRect);
}

bool SdlWindow::drawRects(SDL_Surface* surface, SDL_Point offset,
                          const std::vector<SDL_Rect>& rects)
{
	if (!surface || !ensureRenderTarget())
		return false;
	const bool fullRedraw = rects.empty() || needsFullRedraw(surface->w, surface->h);
	std::vector<DrawOperation> operations;
	if (fullRedraw)
	{
		operations.push_back({ { 0, 0, surface->w, surface->h },
		                       { static_cast<float>(offset.x), static_cast<float>(offset.y),
		                         static_cast<float>(surface->w), static_cast<float>(surface->h) } });
	}
	else
	{
		const SDL_Rect sourceBounds = { 0, 0, surface->w, surface->h };
		const auto viewport = pixelViewport();
		operations.reserve(rects.size());
		for (const auto& srcRect : rects)
		{
			const auto clipped =
			    sdl::render::clipSourceRect(srcRect, sourceBounds, offset, viewport);
			if ((clipped.w <= 0) || (clipped.h <= 0))
				continue;
			operations.push_back({ clipped,
			                       { static_cast<float>(static_cast<Sint64>(offset.x) + clipped.x),
			                         static_cast<float>(static_cast<Sint64>(offset.y) + clipped.y),
			                         static_cast<float>(clipped.w), static_cast<float>(clipped.h) } });
		}
	}

	if (!operations.empty() && !ensureGdiTexture(surface))
		return false;
	for (const auto& operation : operations)
	{
		if (!uploadTexture(surface, operation.src))
			return false;
	}
	if (!drawOperations(operations))
		return false;

	if (fullRedraw)
	{
		_renderTargetNeedsFullRedraw = false;
		_gdiTextureNeedsFullRedraw = false;
	}
	return true;
}

bool SdlWindow::drawScaledRect(SDL_Surface* surface, const SDL_FPoint& scale,
                               const SDL_Rect& srcRect)
{
	if (!surface)
		return false;
	const auto clipped = sdl::render::intersection(srcRect, { 0, 0, surface->w, surface->h });
	if (clipped.w <= 0 || clipped.h <= 0)
		return true;
	const auto dst = scaledDestination(clipped, scale);
	SDL_Rect dstRect = { static_cast<Sint32>(dst.x), static_cast<Sint32>(dst.y),
	                     static_cast<Sint32>(dst.w), static_cast<Sint32>(dst.h) };
	return blit(surface, clipped, dstRect);
}

bool SdlWindow::drawScaledRects(SDL_Surface* surface, const SDL_FPoint& scale,
                                const std::vector<SDL_Rect>& rects)
{
	if (!surface || !ensureRenderTarget())
		return false;
	const bool fullRedraw = rects.empty() || needsFullRedraw(surface->w, surface->h);
	std::vector<DrawOperation> operations;
	const SDL_Rect sourceBounds = { 0, 0, surface->w, surface->h };
	if (fullRedraw)
	{
		const auto srcRect = sourceBounds;
		operations.push_back({ srcRect, scaledDestination(srcRect, scale) });
	}
	else
	{
		operations.reserve(rects.size());
		for (const auto& srcRect : rects)
		{
			const auto clipped = sdl::render::intersection(srcRect, sourceBounds);
			if ((clipped.w <= 0) || (clipped.h <= 0))
				continue;
			operations.push_back({ clipped, scaledDestination(clipped, scale) });
		}
	}

	if (!operations.empty() && !ensureGdiTexture(surface))
		return false;
	for (const auto& operation : operations)
	{
		if (!uploadTexture(surface, operation.src))
			return false;
	}
	if (!drawOperations(operations))
		return false;

	if (fullRedraw)
	{
		_renderTargetNeedsFullRedraw = false;
		_gdiTextureNeedsFullRedraw = false;
	}
	return true;
}

bool SdlWindow::fill(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	if (_renderer)
	{
		if (!ensureRenderTarget())
			return false;
		if (!SDL_SetRenderTarget(_renderer, _renderTarget))
			return false;
		if (!SDL_SetRenderDrawColor(_renderer, r, g, b, a))
			return false;
		return SDL_RenderClear(_renderer);
	}
	return fill(_window, r, g, b, a);
}

bool SdlWindow::fill(SDL_Window* window, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	auto surface = SDL_GetWindowSurface(window);
	if (!surface)
		return false;
	SDL_Rect rect = { 0, 0, surface->w, surface->h };
	auto color = SDL_MapSurfaceRGBA(surface, r, g, b, a);

	return SDL_FillSurfaceRect(surface, &rect, color);
}

static rdpMonitor makeMonitor(SDL_DisplayID id, const SDL_Rect& r, float factor,
                              bool forceAsPrimary)
{
	const float dpi = std::roundf(factor * 100.0f);

	WINPR_ASSERT(r.w > 0);
	WINPR_ASSERT(r.h > 0);

	const auto primary = SDL_GetPrimaryDisplay();
	const auto orientation = SDL_GetCurrentDisplayOrientation(id);
	const auto rdp_orientation = sdl::utils::orientaion_to_rdp(orientation);

	rdpMonitor monitor{};
	monitor.orig_screen = id;
	monitor.x = r.x;
	monitor.y = r.y;
	monitor.width = r.w;
	monitor.height = r.h;
	monitor.is_primary = forceAsPrimary || (id == primary);
	monitor.attributes.desktopScaleFactor = static_cast<UINT32>(dpi);
	monitor.attributes.deviceScaleFactor = 100;
	monitor.attributes.orientation = rdp_orientation;
	/* The physical size is in millimetres and ignored outside 10 to 10000 ([MS-RDPBCGR]
	 * 2.2.1.3.9.1, [MS-RDPEDISP] 2.2.2.2.1). SDL3 does not know a display's physical size, so
	 * leave it at 0 (not given) instead of sending the size in pixels as millimetres. */
	monitor.attributes.physicalWidth = 0;
	monitor.attributes.physicalHeight = 0;

	const auto cat = SDL_LOG_CATEGORY_APPLICATION;
	SDL_LogDebug(cat, "monitor.orig_screen                   %" PRIu32, monitor.orig_screen);
	SDL_LogDebug(cat, "monitor.x                             %" PRId32, monitor.x);
	SDL_LogDebug(cat, "monitor.y                             %" PRId32, monitor.y);
	SDL_LogDebug(cat, "monitor.width                         %" PRId32, monitor.width);
	SDL_LogDebug(cat, "monitor.height                        %" PRId32, monitor.height);
	SDL_LogDebug(cat, "monitor.is_primary                    %" PRIu32, monitor.is_primary);
	SDL_LogDebug(cat, "monitor.attributes.desktopScaleFactor %" PRIu32,
	             monitor.attributes.desktopScaleFactor);
	SDL_LogDebug(cat, "monitor.attributes.deviceScaleFactor  %" PRIu32,
	             monitor.attributes.deviceScaleFactor);
	SDL_LogDebug(cat, "monitor.attributes.orientation        %s",
	             freerdp_desktop_rotation_flags_to_string(monitor.attributes.orientation));
	SDL_LogDebug(cat, "monitor.attributes.physicalWidth      %" PRIu32,
	             monitor.attributes.physicalWidth);
	SDL_LogDebug(cat, "monitor.attributes.physicalHeight     %" PRIu32,
	             monitor.attributes.physicalHeight);
	return monitor;
}

rdpMonitor SdlWindow::query(SDL_Window* window, SDL_DisplayID id, bool forceAsPrimary)
{
	if (!window)
		return {};
	return makeMonitor(id, rect(window, forceAsPrimary), SdlHosted::displayScale(window),
	                   forceAsPrimary);
}

/* Dummy windows report wrong sizes on Wayland and X11, use the display. */
static rdpMonitor queryDisplay(SDL_DisplayID id, bool forceAsPrimary)
{
	SDL_Rect r = {};
	const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
	if (!mode || !SDL_GetDisplayBounds(id, &r))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "display %" PRIu32 ": %s", id, SDL_GetError());
		return {};
	}
	if (forceAsPrimary)
	{
		r.x = 0;
		r.y = 0;
	}
	r.w = static_cast<int>(std::roundf(static_cast<float>(r.w) * mode->pixel_density));
	r.h = static_cast<int>(std::roundf(static_cast<float>(r.h) * mode->pixel_density));
	return makeMonitor(id, r, mode->pixel_density * SDL_GetDisplayContentScale(id), forceAsPrimary);
}

SDL_Rect SdlWindow::rect(SDL_Window* window, bool forceAsPrimary)
{
	SDL_Rect rect = {};
	if (!window)
		return {};

	if (!forceAsPrimary)
	{
		if (!SDL_GetWindowPosition(window, &rect.x, &rect.y))
			return {};
	}

	if (!SdlHosted::pixelSize(window, &rect.w, &rect.h))
		return {};

	const auto flags = SDL_GetWindowFlags(window);
	const auto mask = SDL_WINDOW_FULLSCREEN;
	const auto fs = (flags & mask) == mask;
	if (tryFallback(fs))
	{
		/* On wlroots compositors (Sway, river, etc.), windows that are hidden/unmapped
		 * don't get their actual display dimensions. The dummy window returns its creation size
		 * (64x64) instead of the display size. This causes validation errors since we require >=
		 * 200px. Workaround: If we got dimensions that are too small, query the display directly.
		 */

		const auto displayID = SDL_GetDisplayForWindow(window);
		SDL_Rect displayBounds = {};
		if (SDL_GetDisplayBounds(displayID, &displayBounds))
		{
			if (forceAsPrimary)
			{
				rect.x = 0;
				rect.y = 0;
			}
			else
			{
				/* The dummy window is centered on the display by the compositor, so its
				 * position is not the display origin. Use the display bounds as for w/h. */
				rect.x = displayBounds.x;
				rect.y = displayBounds.y;
			}
			rect.w = displayBounds.w;
			rect.h = displayBounds.h;

			const float contentScale = SDL_GetDisplayContentScale(displayID);
			if (contentScale > 1.0f)
			{
				const auto fw = static_cast<float>(rect.w);
				const auto fh = static_cast<float>(rect.h);
				rect.w = static_cast<int>(std::roundf(fw * contentScale));
				rect.h = static_cast<int>(std::roundf(fh * contentScale));
			}
		}
	}

	return rect;
}

SdlWindow::HighDPIMode SdlWindow::isHighDPIWindowsMode(SDL_Window* window)
{
	if (!window)
		return MODE_INVALID;

	const auto id = SDL_GetDisplayForWindow(window);
	if (id == 0)
		return MODE_INVALID;

	const auto cs = SDL_GetDisplayContentScale(id);
	const auto ds = SdlHosted::displayScale(window);
	const auto pd = SdlHosted::pixelDensity(window);

	/* mac os x style, but no HighDPI display */
	if ((cs == 1.0f) && (ds == 1.0f) && (pd == 1.0f))
		return MODE_NONE;

	/* mac os x style HighDPI */
	if ((cs == 1.0f) && (ds > 1.0f) && (pd > 1.0f))
		return MODE_MACOS;

	/* rest is windows style */
	return MODE_WINDOWS;
}

/* Lazily create or recreate the persistent streaming GDI texture to match `surface`. */
bool SdlWindow::ensureGdiTexture(SDL_Surface* surface)
{
	if (!_renderer || !surface || surface->w <= 0 || surface->h <= 0)
		return false;
	if (_gdiTexture && _gdiTextureW == surface->w && _gdiTextureH == surface->h)
		return true;

	if (_gdiTexture)
	{
		SDL_DestroyTexture(_gdiTexture);
		_gdiTexture = nullptr;
	}
	_gdiTexture = SDL_CreateTexture(_renderer, surface->format, SDL_TEXTUREACCESS_STREAMING,
	                                surface->w, surface->h);
	if (!_gdiTexture)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateTexture: %s", SDL_GetError());
		_gdiTextureW = 0;
		_gdiTextureH = 0;
		return false;
	}
	/* Verbatim, never blended: a transparent window's alpha<0xFF regions would render black. */
	std::ignore = SDL_SetTextureBlendMode(_gdiTexture, SDL_BLENDMODE_NONE);
	_gdiTextureW = surface->w;
	_gdiTextureH = surface->h;
	_gdiTextureNeedsFullRedraw = true;
	_renderMetrics.noteGdiRecreate();
	return true;
}

bool SdlWindow::uploadTexture(SDL_Surface* surface, const SDL_Rect& srcRect)
{
	if (!_renderer || !surface || !surface->pixels || !_gdiTexture || srcRect.w <= 0 ||
	    srcRect.h <= 0)
		return false;
	const Sint64 right = static_cast<Sint64>(srcRect.x) + srcRect.w;
	const Sint64 bottom = static_cast<Sint64>(srcRect.y) + srcRect.h;
	if (srcRect.x < 0 || srcRect.y < 0 || right > surface->w || bottom > surface->h)
		return false;

	const auto* details = SDL_GetPixelFormatDetails(surface->format);
	const int bpp = details ? details->bytes_per_pixel : 4;
	const auto* pixels = static_cast<const uint8_t*>(surface->pixels) +
	                     (1ll * srcRect.y * surface->pitch) + (1ll * srcRect.x * bpp);
	auto liveUploadTimer = _liveMetrics.beginUpload(static_cast<uint64_t>(srcRect.w)*srcRect.h,static_cast<uint64_t>(srcRect.w)*srcRect.h*bpp);
	auto uploadTimer = _renderMetrics.beginUpload(
	    static_cast<uint64_t>(srcRect.w) * srcRect.h,
	    static_cast<uint64_t>(srcRect.w) * srcRect.h * bpp);
	if (!SDL_UpdateTexture(_gdiTexture, &srcRect, pixels, surface->pitch))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_UpdateTexture: %s", SDL_GetError());
		return false;
	}
	uploadTimer.stop();
	return true;
}

bool SdlWindow::drawOperations(const std::vector<DrawOperation>& operations)
{
	if (operations.empty())
		return true;
	if (!_renderer || !_renderTarget || !_gdiTexture)
		return false;
	if (!SDL_SetRenderTarget(_renderer, _renderTarget))
		return false;

	for (const auto& operation : operations)
	{
		if (operation.src.w <= 0 || operation.src.h <= 0 || operation.dst.w <= 0.0f ||
		    operation.dst.h <= 0.0f)
			return false;
		const SDL_FRect src = { static_cast<float>(operation.src.x),
		                        static_cast<float>(operation.src.y),
		                        static_cast<float>(operation.src.w),
		                        static_cast<float>(operation.src.h) };
		auto liveDrawTimer = _liveMetrics.beginDraw();
		auto drawTimer = _renderMetrics.beginDraw();
		if (!SDL_RenderTexture(_renderer, _gdiTexture, &src, &operation.dst))
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_RenderTexture: %s", SDL_GetError());
			return false;
		}
		drawTimer.stop();
	}
	return true;
}

bool SdlWindow::blit(SDL_Surface* surface, const SDL_Rect& srcRect, SDL_Rect& dstRect)
{
	if (!_renderer || !surface)
		return false;
	if (!ensureRenderTarget())
		return false;
	if (!ensureGdiTexture(surface) || !uploadTexture(surface, srcRect))
		return false;

	const DrawOperation operation = { srcRect,
	                                  { static_cast<float>(dstRect.x),
	                                    static_cast<float>(dstRect.y),
	                                    static_cast<float>(dstRect.w),
	                                    static_cast<float>(dstRect.h) } };
	return drawOperations({ operation });
}

bool SdlWindow::updateSurface(bool showTopBar, bool pinned, const SDL_FPoint& pointer)
{
	if (!_renderer)
		return false;

	if (!ensureRenderTarget() || !_renderTarget)
		return false;

	/* Copy accumulated render target to screen and present */
	if (!SDL_SetRenderTarget(_renderer, nullptr))
		return false;
	if (!SDL_RenderTexture(_renderer, _renderTarget, nullptr, nullptr))
		return false;

    if(_performanceOverlay) _performanceOverlay->draw(pixelViewport(),SdlHosted::pixelDensity(_window));
	if (showTopBar && !drawTopBar(pinned, pointer))
		return false;

    const auto liveStart=_liveMetrics.nowNs();
    auto presentTimer = _renderMetrics.beginPresent();
    const auto result=SDL_RenderPresent(_renderer);
    if (_liveMetrics.enabled()) {
        if (result) {
            const auto now=_liveMetrics.nowNs();
            _liveMetrics.notePresent(now >= liveStart ? now-liveStart : 0);
            ++_livePresents;
        } else { _liveMetrics.notePresentSkip(); }
    }
    return result;
}

SdlWindow::StalledText::~StalledText()
{
	if (font)
		TTF_CloseFont(font);
	for (int i = 0; i < 3; i++)
	{
		if (tex[i])
			SDL_DestroyTexture(tex[i]);
		if (shadow[i])
			SDL_DestroyTexture(shadow[i]);
	}
}

bool SdlWindow::ensureStalledText()
{
	if (_stalled && _stalled->font)
		return true;
	if (!_renderer)
		return false;

	if (!_stalled)
	{
		_stalled = std::make_unique<StalledText>();
		if (!_stalled)
			return false;
	}

	auto ops = SDL3ResourceManager::get(SDLResourceManager::typeFonts(),
	                                    "OpenSans-VariableFont_wdth,wght.ttf");
	if (!ops)
		return false;

	_stalled->font = TTF_OpenFontIO(ops, true, stalledFontPts);
	if (!_stalled->font)
		return false;

	for (int i = 0; i < 3; i++)
	{
		const std::string text =
		    "Reconnecting" + std::string(static_cast<size_t>(i) + 1, '.');
		auto render = [&](const SDL_Color& color)
		{
			auto surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
			    TTF_RenderText_Blended(_stalled->font, text.c_str(), 0, color),
			    SDL_DestroySurface);
			if (!surface || (surface->w <= 0) || (surface->h <= 0))
				return static_cast<SDL_Texture*>(nullptr);
			_stalled->texW[i] = surface->w;
			_stalled->texH[i] = surface->h;
			return SDL_CreateTextureFromSurface(_renderer, surface.get());
		};
		_stalled->tex[i] = render(stalledTextColor);
		_stalled->shadow[i] = render(stalledShadowColor);
	}
	return true;
}

bool SdlWindow::renderStalledFrame(int dots, bool showTopBar, bool pinned,
                                   const SDL_FPoint& pointer)
{
	if (!_renderer)
		return false;

	(void)ensureRenderTarget();

	/* Re-present the last accumulated frame (or blank when none yet), then
	 * dim the session content. The connection bar is drawn after the dim so
	 * it stays readable and usable while reconnecting. The render target
	 * itself is never touched, so the next normal frame paints clean with no
	 * explicit clear needed. */
	if (!SDL_SetRenderTarget(_renderer, nullptr))
		return false;
	if (_renderTarget)
	{
		if (!SDL_RenderTexture(_renderer, _renderTarget, nullptr, nullptr))
			return false;
	}
	else
	{
		if (!SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 0xff))
			return false;
		if (!SDL_RenderClear(_renderer))
			return false;
	}
	if (_performanceOverlay)
		_performanceOverlay->draw(pixelViewport(), SdlHosted::pixelDensity(_window));

	const auto viewport = pixelViewport();
	if (viewport.w <= 0 || viewport.h <= 0)
		return false;

	if (!SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND))
		return false;
	if (!SDL_SetRenderDrawColor(_renderer, 0, 0, 0, stalledDimAlpha))
		return false;
	const SDL_FRect dim = { 0.0f, 0.0f, static_cast<float>(viewport.w),
		                    static_cast<float>(viewport.h) };
	if (!SDL_RenderFillRect(_renderer, &dim))
		return false;

	const int phase = (dots < 1 || dots > 3) ? 2 : dots - 1;
	if (ensureStalledText() && _stalled->tex[phase])
	{
		const float tw = static_cast<float>(_stalled->texW[phase]);
		const float th = static_cast<float>(_stalled->texH[phase]);
		const SDL_FRect dst = { (static_cast<float>(viewport.w) - tw) / 2.0f,
			                    (static_cast<float>(viewport.h) - th) / 2.0f, tw, th };
		/* Soft shadow for readability over bright remote content. */
		if (_stalled->shadow[phase])
		{
			const SDL_FRect shadow = { dst.x + 2.0f, dst.y + 2.0f, dst.w, dst.h };
			std::ignore =
			    SDL_RenderTexture(_renderer, _stalled->shadow[phase], nullptr, &shadow);
		}
		if (!SDL_RenderTexture(_renderer, _stalled->tex[phase], nullptr, &dst))
			return false;
	}

	return !showTopBar || drawTopBar(pinned, pointer);
}

/* Connection bar shared by normal and stalled frames, so both initialize the
 * same capabilities and record the draw in render metrics. */
bool SdlWindow::drawTopBar(bool pinned, const SDL_FPoint& pointer)
{
	if (!_topBar)
		return true;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	auto bridge = SdlLauncher::active();
	_topBar->setPerformanceAvailable(bridge && bridge->performanceAvailable());
#endif
	const auto viewport = pixelViewport();
	if (viewport.w <= 0 || viewport.h <= 0)
		return false;
	if (!_topBarRectInit)
		_topBarRect = SdlTopBar::defaultRect(viewport, _topBarCompact);
	_topBarRect = SdlTopBar::clampToViewport(_topBarRect, viewport, _topBarCompact);
	if (!_topBar->draw(_topBarRect, viewport, pinned, pointer))
		return false;
	_renderMetrics.noteTopBarDraw();
	return true;
}

bool SdlWindow::updateStalledSurface(int dots, bool showTopBar, bool pinned,
                                     const SDL_FPoint& pointer)
{
	if (!renderStalledFrame(dots, showTopBar, pinned, pointer))
		return false;

	/* Stalled overlay presents are counted separately from present_calls so
	 * that identical-input comparisons of present_calls stay stable. */
	_renderMetrics.noteStalledPresent();
	return SDL_RenderPresent(_renderer);
}

SdlTopBarRect SdlWindow::topBarRect() const
{
	return _topBarRect;
}

void SdlWindow::setTopBarRect(const SdlTopBarRect& rect)
{
	_topBarRect = rect;
	_topBarRectInit = true;
}

void SdlWindow::resetTopBarRect()
{
	_topBarRect = {};
	_topBarRectInit = false;
}

bool SdlWindow::topBarCompact() const
{
	return _topBarCompact;
}

void SdlWindow::setTopBarCompact(bool compact)
{
	_topBarCompact = compact;
}

static bool windowRenderPoint(SDL_Renderer* renderer, float x, float y, SDL_FPoint& point)
{
	if (!renderer)
		return false;
	return SDL_RenderCoordinatesFromWindow(renderer, x, y, &point.x, &point.y) != 0;
}

static SDL_Rect windowViewport(SDL_Window* window)
{
	int width = 0;
	int height = 0;
	if (window)
		SdlHosted::pixelSize(window, &width, &height);
	return { 0, 0, width, height };
}

SDL_Rect SdlWindow::pixelViewport() const
{
	int width = 0;
	int height = 0;
	if (_window)
		SdlHosted::pixelSize(_window, &width, &height);
	return { 0, 0, width, height };
}

bool SdlWindow::rebuildDesktopTextures()
{
    if (!_renderer || !SDL_SetRenderTarget(_renderer, nullptr)) return false;
    if (_renderTarget) SDL_DestroyTexture(_renderTarget);
    if (_gdiTexture) SDL_DestroyTexture(_gdiTexture);
    _renderTarget = nullptr; _gdiTexture = nullptr; _gdiTextureW = _gdiTextureH = 0;
    _renderTargetNeedsFullRedraw = _gdiTextureNeedsFullRedraw = true;
    return ensureRenderTarget();
}

bool SdlWindow::updateTopBarHover(float x, float y, uint64_t now)
{
    SDL_FPoint point{};
    return _topBar && _renderer && windowRenderPoint(_renderer, x, y, point) &&
        _topBar->updateHover(_topBarRect, pixelViewport(), point, now);
}
bool SdlWindow::serviceTopBarHover(uint64_t now) { return _topBar && _topBar->serviceHover(now); }
bool SdlWindow::refreshMenuOpen() const { return _topBar && _topBar->refreshMenuOpen(); }
void SdlWindow::closeRefreshMenu() { if (_topBar) _topBar->closeRefreshMenu(); }
void SdlWindow::setRefreshCapabilities(bool server, bool updates) { if (_topBar) _topBar->setRefreshCapabilities(server, updates); }
bool SdlWindow::refreshEnabled(SdlTopBarButton button) const { return _topBar && _topBar->refreshEnabled(button); }
void SdlWindow::noteRefresh(SdlTopBarButton button) { if (_topBar) _topBar->noteRefresh(button); }
SdlTopBarButton SdlWindow::selectedRefresh() const
{
    return _topBar ? _topBar->selectedRefresh() : SdlTopBarButton::RefreshCombined;
}

bool SdlWindow::topBarContains(float x, float y) const
{
	if (!_renderer || !_topBar)
		return false;

	SDL_FPoint pointer{};
	if (!windowRenderPoint(_renderer, x, y, pointer))
		return false;
	return _topBar->contains(_topBarRect, windowViewport(_window), pointer);
}

bool SdlWindow::topBarNearTop(float x, float y) const
{
	if (!_renderer || !_topBar)
		return false;

	SDL_FPoint pointer{};
	if (!windowRenderPoint(_renderer, x, y, pointer))
		return false;

	return _topBar->nearTop(windowViewport(_window), pointer, _topBarCompact);
}

SdlTopBarButton SdlWindow::topBarButtonAt(float x, float y) const
{
	if (!_renderer || !_topBar)
		return SdlTopBarButton::None;

	SDL_FPoint pointer{};
	if (!windowRenderPoint(_renderer, x, y, pointer))
		return SdlTopBarButton::None;

	return _topBar->hitTest(_topBarRect, windowViewport(_window), pointer);
}

bool SdlWindow::topBarMoveAt(float x, float y) const
{
	if (!_renderer || !_topBar)
		return false;

	SDL_FPoint pointer{};
	if (!windowRenderPoint(_renderer, x, y, pointer))
		return false;

	return _topBar->hitMove(_topBarRect, windowViewport(_window), pointer);
}

bool SdlWindow::topBarResizeAt(float x, float y) const
{
	if (!_renderer || !_topBar)
		return false;

	SDL_FPoint pointer{};
	if (!windowRenderPoint(_renderer, x, y, pointer))
		return false;

	return _topBar->hitResize(_topBarRect, windowViewport(_window), pointer);
}

bool SdlWindow::paintResizeFrame(SDL_Surface* surface, SDL_Point off, bool contentChanged,
                                 const SDL_Rect& inset, bool fillRevealed, bool dashedBorder)
{
	if (!_renderer || !surface)
		return false;
	if (!ensureRenderTarget())
		return false;

	const int prevW = _gdiTextureW;
	const int prevH = _gdiTextureH;
	if (!ensureGdiTexture(surface))
		return false;
	/* A recreated texture is empty, so upload even when the content did not change. */
	const bool recreated = (_gdiTextureW != prevW) || (_gdiTextureH != prevH);
	if ((contentChanged || recreated) &&
	    !SDL_UpdateTexture(_gdiTexture, nullptr, surface->pixels, surface->pitch))
		return false;

	if (!SDL_SetRenderTarget(_renderer, _renderTarget))
		return false;

	int ww = 0;
	int wh = 0;
	SdlHosted::pixelSize(_window, &ww, &wh);
	/* The visible frame; the ring outside it (the resize band) stays fully transparent. */
	const SDL_FRect frame = { static_cast<float>(inset.x), static_cast<float>(inset.y),
		                      static_cast<float>(ww - inset.x - inset.w),
		                      static_cast<float>(wh - inset.y - inset.h) };

	/* Translucent fill in revealed area awaiting server frame. */
	constexpr Uint8 kFillAlpha = 0x80;
	std::ignore = SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_NONE);
	std::ignore = SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 0);
	std::ignore = SDL_RenderClear(_renderer);
	if (fillRevealed)
	{
		std::ignore = SDL_SetRenderDrawColor(_renderer, 0x2B, 0x2B, 0x2B, kFillAlpha);
		std::ignore = SDL_RenderFillRect(_renderer, &frame);
	}
	SDL_FRect fdst = { static_cast<float>(off.x), static_cast<float>(off.y),
		               static_cast<float>(surface->w), static_cast<float>(surface->h) };
	/* Anchored frame clipped to visible bounds. */
	const SDL_Rect clip = { inset.x, inset.y, ww - inset.x - inset.w, wh - inset.y - inset.h };
	std::ignore = SDL_SetRenderClipRect(_renderer, &clip);
	std::ignore = SDL_RenderTexture(_renderer, _gdiTexture, nullptr, &fdst);
	std::ignore = SDL_SetRenderClipRect(_renderer, nullptr);

	/* Dashed border indicating pending resize target. */
	if (dashedBorder)
	{
		std::ignore = SDL_SetRenderDrawColor(_renderer, 0xC8, 0xC8, 0xC8, 0xFF);
		const float dash = 8.0F;
		const float gap = 5.0F;
		const float fx2 = frame.x + frame.w;
		const float fy2 = frame.y + frame.h;
		const float lo = frame.y + 0.5F;
		const float by = fy2 - 0.5F;
		const float lx = frame.x + 0.5F;
		const float rx = fx2 - 0.5F;
		const float step = dash + gap;
		const auto steps = [step](float len)
		{ return (len <= 0.0F) ? 0 : static_cast<int>(std::ceil(len / step)); };
		for (int i = 0; i < steps(fx2 - frame.x); i++)
		{
			const float x = frame.x + (static_cast<float>(i) * step);
			const float x2 = (x + dash < fx2) ? (x + dash) : rx;
			std::ignore = SDL_RenderLine(_renderer, x, lo, x2, lo);
			std::ignore = SDL_RenderLine(_renderer, x, by, x2, by);
		}
		for (int i = 0; i < steps(fy2 - frame.y); i++)
		{
			const float y = frame.y + (static_cast<float>(i) * step);
			const float y2 = (y + dash < fy2) ? (y + dash) : by;
			std::ignore = SDL_RenderLine(_renderer, lx, y, lx, y2);
			std::ignore = SDL_RenderLine(_renderer, rx, y, rx, y2);
		}
	}

	if (!SDL_SetRenderTarget(_renderer, nullptr))
		return false;
	std::ignore = SDL_RenderTexture(_renderer, _renderTarget, nullptr, nullptr);
	std::ignore = SDL_RenderPresent(_renderer);
	return true;
}

SdlWindow SdlWindow::create(SDL_DisplayID id, const std::string& title, Uint32 flags, Uint32 width,
                            Uint32 height)
{
	flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

	SDL_Rect rect = { static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id)),
		              static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id)), static_cast<int>(width),
		              static_cast<int>(height) };

	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
	{
		std::ignore = SDL_GetDisplayBounds(id, &rect);
	}

	SdlWindow window{ id, title, rect, flags };

	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
	{
		window.setOffsetX(rect.x);
		window.setOffsetY(rect.y);
	}

	return window;
}

SdlWindow SdlWindow::create(SDL_DisplayID id, const std::string& title, Uint32 flags,
                            const SDL_Rect& rect)
{
	return SdlWindow{ id, title, rect, flags };
}

/* Popup constructor: positioned relative to the parent origin. */
SdlWindow::SdlWindow(SDL_Window* parent, const SDL_Rect& rect, bool transparent, bool tooltip)
    : _initialW(rect.w), _initialH(rect.h)
{
	auto props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, parent);
	SDL_SetBooleanProperty(props,
	                       tooltip ? SDL_PROP_WINDOW_CREATE_TOOLTIP_BOOLEAN
	                               : SDL_PROP_WINDOW_CREATE_MENU_BOOLEAN,
	                       true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
#if defined(SDL_PROP_WINDOW_CREATE_CONSTRAIN_POPUP_BOOLEAN)
	/* The server places RemoteApp popups; a moved one would map clicks to other points. */
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_CONSTRAIN_POPUP_BOOLEAN, false);
#endif
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
	/* Transparent so menus' genuine per-pixel alpha (corners/shadow) isn't rendered black. */
	if (transparent)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, rect.w);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, rect.h);
	/* Hidden until first paint, like the app constructor. */
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);

	_window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
	if (_window)
	{
		std::ignore = SDL_SyncWindow(_window);
		_renderer = SDL_CreateRenderer(_window, nullptr);
		_displayID = SDL_GetDisplayForWindow(_window);
	}
}

SdlWindow SdlWindow::createPopup(SDL_Window* parent, const SDL_Rect& rect, bool transparent,
                                 bool tooltip)
{
	return SdlWindow{ parent, rect, transparent, tooltip };
}

static SDL_Window* createDummy(SDL_DisplayID id)
{
	/* Startup monitor probe: must never become visible. Previous versions
	 * created a visible window and toggled it fullscreen per display to
	 * read back pixel dimensions — a fullscreen slideshow across all
	 * displays before any network connect (fully meaningless when the
	 * host is offline and connect fails). Keep the probe hidden; pixel
	 * dimensions are derived from display bounds and pixel density below,
	 * so no positioning or fullscreen transition is needed. */
	const auto x = SDL_WINDOWPOS_CENTERED_DISPLAY(id);
	const auto y = SDL_WINDOWPOS_CENTERED_DISPLAY(id);
	const int w = 64;
	const int h = 64;

	auto props = SDL_CreateProperties();
	std::stringstream ss;
	ss << "SdlWindow::query(" << id << ")";
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, ss.str().c_str());
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);

	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, false);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);

	auto window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);

	return window;
}

/* Read scale and pixel density from a hidden window. Content scale controls
 * UI size; only pixel density converts display coordinates to pixels. */
static float probeDisplayScale(SDL_DisplayID id, float& pixelDensity)
{
	std::unique_ptr<SDL_Window, void (*)(SDL_Window*)> window(createDummy(id), SDL_DestroyWindow);
	float scale = 0.0f;
	pixelDensity = 0.0f;
	if (window)
	{
		scale = SDL_GetWindowDisplayScale(window.get());
		pixelDensity = SDL_GetWindowPixelDensity(window.get());
	}
	if (pixelDensity <= 0.0f)
	{
		const auto mode = SDL_GetCurrentDisplayMode(id);
		pixelDensity = mode ? mode->pixel_density : 1.0f;
	}
	if (pixelDensity <= 0.0f)
		pixelDensity = 1.0f;
	if (scale <= 0.0f)
		scale = SDL_GetDisplayContentScale(id) * pixelDensity;
	if (scale <= 0.0f)
		scale = 1.0f;
	return scale;
}

rdpMonitor SdlWindow::query(SDL_DisplayID id, bool forceAsPrimary)
{
	if (sdl::utils::isWaylandDriver() || sdl::utils::isX11Driver())
		return queryDisplay(id, forceAsPrimary);

	SDL_Rect bounds = {};
	if (!SDL_GetDisplayBounds(id, &bounds) || (bounds.w <= 0) || (bounds.h <= 0))
		return {};

	float pixelDensity = 1.0f;
	const float scale = probeDisplayScale(id, pixelDensity);
	const auto pw = static_cast<INT32>(std::roundf(static_cast<float>(bounds.w) * pixelDensity));
	const auto ph = static_cast<INT32>(std::roundf(static_cast<float>(bounds.h) * pixelDensity));
	if ((pw <= 0) || (ph <= 0))
		return {};

	const auto primary = SDL_GetPrimaryDisplay();
	const auto orientation = SDL_GetCurrentDisplayOrientation(id);
	const auto rdp_orientation = sdl::utils::orientaion_to_rdp(orientation);
	const auto dpi = static_cast<UINT32>(std::roundf(scale * 100.0f));

	rdpMonitor monitor{};
	monitor.orig_screen = id;
	monitor.x = forceAsPrimary ? 0 : bounds.x;
	monitor.y = forceAsPrimary ? 0 : bounds.y;
	monitor.width = pw;
	monitor.height = ph;
	monitor.is_primary = forceAsPrimary || (id == primary);
	monitor.attributes.desktopScaleFactor = dpi;
	monitor.attributes.deviceScaleFactor = 100;
	monitor.attributes.orientation = rdp_orientation;
	monitor.attributes.physicalWidth = WINPR_ASSERTING_INT_CAST(uint32_t, pw);
	monitor.attributes.physicalHeight = WINPR_ASSERTING_INT_CAST(uint32_t, ph);

	const auto cat = SDL_LOG_CATEGORY_APPLICATION;
	SDL_LogDebug(cat, "monitor.orig_screen                   %" PRIu32, monitor.orig_screen);
	SDL_LogDebug(cat, "monitor.x                             %" PRId32, monitor.x);
	SDL_LogDebug(cat, "monitor.y                             %" PRId32, monitor.y);
	SDL_LogDebug(cat, "monitor.width                         %" PRId32, monitor.width);
	SDL_LogDebug(cat, "monitor.height                        %" PRId32, monitor.height);
	SDL_LogDebug(cat, "monitor.is_primary                    %" PRIu32, monitor.is_primary);
	return monitor;
}

SDL_Rect SdlWindow::rect(SDL_DisplayID id, bool forceAsPrimary)
{
	SDL_Rect bounds = {};
	if (!SDL_GetDisplayBounds(id, &bounds))
		return {};
	float pixelDensity = 1.0f;
	std::ignore = probeDisplayScale(id, pixelDensity);
	const auto pw = static_cast<int>(std::roundf(static_cast<float>(bounds.w) * pixelDensity));
	const auto ph = static_cast<int>(std::roundf(static_cast<float>(bounds.h) * pixelDensity));
	SDL_Rect rect = {};
	rect.x = forceAsPrimary ? 0 : bounds.x;
	rect.y = forceAsPrimary ? 0 : bounds.y;
	rect.w = pw;
	rect.h = ph;
	return rect;
}

bool SdlWindow::tryFallback(bool isFullscreen)
{
	/* If we define a custom env variable to use the wlroots hack
	 * then enable/disable according to this setting only.
	 */
	const auto wlroots_hack = SDL_getenv("FREERDP_WLROOTS_HACK");
	if (wlroots_hack != nullptr)
	{
		const auto enabled = strcmp(wlroots_hack, "0") != 0;
		if (strcmp(wlroots_hack, "force") == 0)
			isFullscreen = true;
		return enabled && isFullscreen;
	}

	const auto platform = SDL_GetPlatform();
	if ((platform == nullptr) || (strcmp(platform, "Linux") != 0))
		return false;

	const auto driver = SDL_GetCurrentVideoDriver();
	if ((driver == nullptr) || (strcmp(driver, "wayland") != 0))
		return false;

	/* Check XDG_SESSION_DESKTOP and XDG_CURRENT_DESKTOP for wlroots-based
	 * compositors. The original check only matched Sway, but other wlroots
	 * compositors (Hyprland, river, etc.) have the same dummy-window sizing
	 * behavior where hidden/unmapped windows return 64x64 instead of the
	 * display size. Use strstr for substring matching since XDG_CURRENT_DESKTOP
	 * can be a colon-separated list (e.g. "sway:wlroots", "Hyprland").
	 */
	auto isWlrootsCompositor = [](const char* value) -> bool
	{
		if (!value)
			return false;
		if (strstr(value, "sway") || strstr(value, "Sway") || strstr(value, "Hyprland") ||
		    strstr(value, "hyprland") || strstr(value, "river") || strstr(value, "wlroots"))
			return true;
		return false;
	};

	const auto xdg_session = SDL_getenv("XDG_SESSION_DESKTOP");
	if (isWlrootsCompositor(xdg_session))
		return isFullscreen;

	const auto xdg_desktop = SDL_getenv("XDG_CURRENT_DESKTOP");
	if (isWlrootsCompositor(xdg_desktop))
		return isFullscreen;

	return false;
}
