/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Display Control Channel
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
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

#include <vector>
#include <winpr/sysinfo.h>
#include <winpr/assert.h>

#include <freerdp/gdi/gdi.h>

#include <SDL3/SDL.h>

#include "sdl_disp.hpp"
#include "sdl_input.hpp"
#include "sdl_context.hpp"

#include <freerdp/log.h>
#define TAG CLIENT_TAG("sdl.disp")

static constexpr UINT64 RESIZE_MIN_DELAY = 500; /* minimum delay in ms between two resizes */
static constexpr unsigned MAX_RETRIES = 5;
/* RDP_NEG_RSP flag (MS-RDPBCGR 2.2.1.2.1), private to libfreerdp/core/nego.h. */
static constexpr UINT32 NEG_EXTENDED_CLIENT_DATA_SUPPORTED = 0x01;

static auto operator==(const DISPLAY_CONTROL_MONITOR_LAYOUT& a,
                       const DISPLAY_CONTROL_MONITOR_LAYOUT& b)
{
	if (a.Flags != b.Flags)
		return false;
	if (a.Left != b.Left)
		return false;
	if (a.Top != b.Top)
		return false;
	if (a.Width != b.Width)
		return false;
	if (a.Height != b.Height)
		return false;
	if (a.PhysicalWidth != b.PhysicalWidth)
		return false;
	if (a.PhysicalHeight != b.PhysicalHeight)
		return false;
	if (a.Orientation != b.Orientation)
		return false;
	if (a.DesktopScaleFactor != b.DesktopScaleFactor)
		return false;
	if (a.DeviceScaleFactor != b.DeviceScaleFactor)
		return false;
	return true;
}

/* MS-RDPEDISP 2.2.2.2.1: the server ignores physical dimensions outside 10..10000 mm. A layout
 * whose only difference is such a size requests nothing new. */
static bool physicalIgnored(const DISPLAY_CONTROL_MONITOR_LAYOUT& layout)
{
	const auto valid = [](UINT32 mm) { return (mm >= 10) && (mm <= 10000); };
	return !valid(layout.PhysicalWidth) || !valid(layout.PhysicalHeight);
}

static bool sameRequest(const DISPLAY_CONTROL_MONITOR_LAYOUT& next,
                        const DISPLAY_CONTROL_MONITOR_LAYOUT& last)
{
	if (!physicalIgnored(next))
		return next == last;
	auto adjusted = next;
	adjusted.PhysicalWidth = last.PhysicalWidth;
	adjusted.PhysicalHeight = last.PhysicalHeight;
	return adjusted == last;
}

bool sdlDispContext::settings_changed(const std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT>& layout)
{
	if (_forceLayout || (layout.size() != _last_sent_layout.size()))
		return true;
	for (size_t i = 0; i < layout.size(); i++)
	{
		if (!sameRequest(layout[i], _last_sent_layout[i]))
			return true;
	}
	return false;
}

bool sdlDispContext::sendResize(UINT64 now)
{
	auto settings = _sdl->context()->settings;

	if (!settings)
		return false;

	std::lock_guard lock(_channelMutex);
	if (!_activated || !_disp)
		return true;
	/* service() takes its time before it may block for a while (display refresh, fullscreen
	 * transition), and the caps can arrive meanwhile. Take their seed and send time into
	 * account here, under the lock, rather than relying on the state service() started with. */
	applySeed();
	if (tooSoon(now))
		return true;
	_lastSentDate = now;

	const UINT32 mcount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
	auto monitors = static_cast<const rdpMonitor*>(
	    freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray));
	if (mcount == 0 || !monitors)
		return true;
	return sendLayout(monitors, mcount);
}

void sdlDispContext::applySeed()
{
	if (!_seedRequested.exchange(false))
		return;
	_last_sent_layout = std::move(_seedLayout);
	_seedLayout.clear();
	_forceLayout = false;
	WLog_Print(_sdl->getWLog(), WLOG_DEBUG,
	           "Display channel opened: the connect-time monitor layout counts as sent");
}

bool sdlDispContext::tooSoon(UINT64 now) const
{
	/* now may predate a send time set later by the caps; do not let it wrap around. */
	return (now < _lastSentDate) || (now - _lastSentDate < RESIZE_MIN_DELAY);
}

bool sdlDispContext::setWindowResizeable()
{
	_resizeableUpdate = 1;
	return scheduleResize();
}

static bool sdl_disp_check_context(void* context, SdlContext** ppsdl, sdlDispContext** ppsdlDisp,
                                   rdpSettings** ppSettings)
{
	if (!context)
		return false;

	auto sdl = get_context(context);
	if (!sdl)
		return false;

	if (!sdl->context()->settings)
		return false;

	*ppsdl = sdl;
	*ppsdlDisp = &sdl->getDisplayChannelContext();
	*ppSettings = sdl->context()->settings;
	return true;
}

void sdlDispContext::OnActivated(void* context, const ActivatedEventArgs* e)
{
	SdlContext* sdl = nullptr;
	sdlDispContext* sdlDisp = nullptr;
	rdpSettings* settings = nullptr;

	if (!sdl_disp_check_context(context, &sdl, &sdlDisp, &settings))
		return;

	if (!sdlDisp->_activated)
		return;

	/* The desktop size agreed on at connect time is capped by the protocol (4096x2048,
	 * MS-RDPBCGR 2.2.1.3.2), so a larger window starts out with a smaller session. Only the
	 * display control channel can raise it, and in fullscreen nothing else triggers a layout:
	 * the window is never resized by hand. */
	if (!freerdp_settings_get_bool(settings, FreeRDP_Fullscreen))
	{
		if (!sdlDisp->setWindowResizeable())
			return;

		/* The connect only carries a single monitor, so a canvas the page splits into several
		 * (sdl_monitors_emscripten.js) needs a layout at once: its size may never change. Ask
		 * the page again, a reconnect starts from the settings of the command line. */
#if defined(__EMSCRIPTEN__)
		if (!sdl->updateWindowList())
			return;
		const bool split = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) > 1;
#else
		const bool split = false;
#endif
		if (e->firstActivation && !split)
			return;
	}

	std::ignore = sdlDisp->scheduleResize();
}

void sdlDispContext::OnStateChanged(void* context, const StateChangedEventArgs* e)
{
	SdlContext* sdl = nullptr;
	sdlDispContext* sdlDisp = nullptr;
	rdpSettings* settings = nullptr;

	WINPR_ASSERT(e);
	if (!sdl_disp_check_context(context, &sdl, &sdlDisp, &settings))
		return;
	/* The client monitor data (CS_MONITOR, CS_MONITOR_EX) is written right after this
	 * transition, on the same thread. */
	if ((e->newState == CONNECTION_STATE_MCS_CREATE_REQUEST) &&
	    (e->oldState != CONNECTION_STATE_MCS_CREATE_REQUEST))
		sdlDisp->captureConnectLayout();
}

void sdlDispContext::captureConnectLayout()
{
	auto settings = _sdl->context()->settings;
	WINPR_ASSERT(settings);

	/* Mirror the conditions under which gcc_write_conference_create_request() sends both
	 * monitor blocks. Overrides apply to layout PDUs only, so their values were not sent. */
	const UINT64 overrides = FREERDP_MONITOR_OVERRIDE_ORIENTATION |
	                         FREERDP_MONITOR_OVERRIDE_DESKTOP_SCALE |
	                         FREERDP_MONITOR_OVERRIDE_DEVICE_SCALE;
	const UINT32 count = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
	auto monitors = static_cast<const rdpMonitor*>(
	    freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray));
	const bool sent =
	    freerdp_settings_get_bool(settings, FreeRDP_UseMultimon) &&
	    !freerdp_settings_get_bool(settings, FreeRDP_SpanMonitors) &&
	    ((freerdp_settings_get_uint32(settings, FreeRDP_NegotiationFlags) &
	      NEG_EXTENDED_CLIENT_DATA_SUPPORTED) != 0) &&
	    freerdp_settings_get_bool(settings, FreeRDP_HasMonitorAttributes) &&
	    ((freerdp_settings_get_uint64(settings, FreeRDP_MonitorOverrideFlags) & overrides) == 0) &&
	    (count > 0) && monitors;

	/* Without multimon, the client core data always describes the single remote monitor:
	 * desktop size, physical size, orientation and both scale factors. */
	const bool single = !freerdp_settings_get_bool(settings, FreeRDP_UseMultimon);

	std::lock_guard lock(_channelMutex);
	_connectLayout.clear();
	if (sent)
		_connectLayout = buildLayout(settings, monitors, count);
	else if (single)
	{
		DISPLAY_CONTROL_MONITOR_LAYOUT layout = {};
		layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
		layout.Width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
		layout.Height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
		layout.PhysicalWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalWidth);
		layout.PhysicalHeight =
		    freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalHeight);
		layout.Orientation = freerdp_settings_get_uint16(settings, FreeRDP_DesktopOrientation);
		layout.DesktopScaleFactor =
		    freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
		layout.DeviceScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor);
		_connectLayout.push_back(layout);
	}
	for (const auto& layout : _connectLayout)
		WLog_Print(_sdl->getWLog(), WLOG_DEBUG,
		           "Connect-time layout: flags=0x%" PRIx32 " %" PRId32 ",%" PRId32 " %" PRIu32
		           "x%" PRIu32 " physical=%" PRIu32 "x%" PRIu32 " orientation=%" PRIu32
		           " scale=%" PRIu32 "/%" PRIu32,
		           layout.Flags, layout.Left, layout.Top, layout.Width, layout.Height,
		           layout.PhysicalWidth, layout.PhysicalHeight, layout.Orientation,
		           layout.DesktopScaleFactor, layout.DeviceScaleFactor);
	WLog_Print(_sdl->getWLog(), WLOG_DEBUG,
	           "Connect-time %s layout %s (%" PRIuz " monitors; multimon=%d span=%d "
	           "negotiation=0x%08" PRIx32 " attributes=%d overrides=0x%" PRIx64 " count=%" PRIu32
	           ")",
	           sent ? "monitor" : "desktop", _connectLayout.empty() ? "not sent" : "recorded",
	           _connectLayout.size(),
	           freerdp_settings_get_bool(settings, FreeRDP_UseMultimon),
	           freerdp_settings_get_bool(settings, FreeRDP_SpanMonitors),
	           freerdp_settings_get_uint32(settings, FreeRDP_NegotiationFlags),
	           freerdp_settings_get_bool(settings, FreeRDP_HasMonitorAttributes),
	           freerdp_settings_get_uint64(settings, FreeRDP_MonitorOverrideFlags), count);
}

void sdlDispContext::OnGraphicsReset(void* context, const GraphicsResetEventArgs* e)
{
	SdlContext* sdl = nullptr;
	sdlDispContext* sdlDisp = nullptr;
	rdpSettings* settings = nullptr;

	WINPR_UNUSED(e);
	if (!sdl_disp_check_context(context, &sdl, &sdlDisp, &settings))
		return;

	if (!sdlDisp->_activated)
		return;

	if (freerdp_settings_get_bool(settings, FreeRDP_Fullscreen))
		std::ignore = sdlDisp->scheduleResize();
	else if (sdlDisp->setWindowResizeable())
		std::ignore = sdlDisp->scheduleResize();
}

void sdlDispContext::service()
{
	service(GetTickCount64());
}

void sdlDispContext::service(UINT64 now)
{
	if (_topologyRequested.exchange(false))
	{
		_refreshTopology = true;
		_pending = true;
		_retries = 0;
		_nextAttempt = now + RESIZE_MIN_DELAY;
	}
	if (_resizeRequested.exchange(false))
	{
		_pending = true;
		_retries = 0;
		if (!_refreshTopology)
			_nextAttempt = now;
	}
	{
		std::lock_guard lock(_channelMutex);
		applySeed();
	}
	if (_forceRequested.exchange(false))
		_forceLayout = true;
	// These requests originate in channel callbacks on the RDP thread.
	const int resizeable = _resizeableUpdate.exchange(-1);
	if (resizeable >= 0)
		std::ignore = _sdl->setResizeable(resizeable != 0);
	if (!_pending || now < _nextAttempt)
		return;
	{
		/* Wait out the minimum delay after the last send (or the capabilities) instead of
		 * spending a retry on it. */
		std::lock_guard lock(_channelMutex);
		if (_activated && tooSoon(now))
		{
			_nextAttempt = _lastSentDate + RESIZE_MIN_DELAY;
			return;
		}
	}
	bool refreshed = true;
	if (_refreshTopology)
		refreshed = _sdl->refreshDisplayWindows();
	if (refreshed)
		refreshed = sendResize(now);
	_nextAttempt = now + 1000;
	if (++_retries > MAX_RETRIES)
	{
		_pending = _refreshTopology = false;
		if (!refreshed)
			WLog_Print(_sdl->getWLog(), WLOG_WARN,
			           "Display refresh did not settle; retaining the live session until the next "
			           "display event");
	}
}

std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT>
sdlDispContext::buildLayout(const rdpSettings* settings, const rdpMonitor* monitors,
                            size_t nmonitors)
{
	WINPR_ASSERT(settings);
	WINPR_ASSERT(monitors);

	std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> layouts;
	layouts.reserve(nmonitors);

	for (size_t i = 0; i < nmonitors; i++)
	{
		auto monitor = &monitors[i];
		DISPLAY_CONTROL_MONITOR_LAYOUT layout = {};

		layout.Flags = (monitor->is_primary ? DISPLAY_CONTROL_MONITOR_PRIMARY : 0);
		layout.Left = monitor->x;
		layout.Top = monitor->y;
		layout.Width = WINPR_ASSERTING_INT_CAST(uint32_t, monitor->width);
		layout.Height = WINPR_ASSERTING_INT_CAST(uint32_t, monitor->height);
		layout.Orientation = ORIENTATION_LANDSCAPE;
		layout.PhysicalWidth = monitor->attributes.physicalWidth;
		layout.PhysicalHeight = monitor->attributes.physicalHeight;

		switch (monitor->attributes.orientation)
		{
			case ORIENTATION_PORTRAIT:
				layout.Orientation = ORIENTATION_PORTRAIT;
				break;

			case ORIENTATION_LANDSCAPE_FLIPPED:
				layout.Orientation = ORIENTATION_LANDSCAPE_FLIPPED;
				break;

			case ORIENTATION_PORTRAIT_FLIPPED:
				layout.Orientation = ORIENTATION_PORTRAIT_FLIPPED;
				break;

			case ORIENTATION_LANDSCAPE:
			default:
				/* MS-RDPEDISP - 2.2.2.2.1:
				 * Orientation (4 bytes): A 32-bit unsigned integer that specifies the
				 * orientation of the monitor in degrees. Valid values are 0, 90, 180
				 * or 270
				 *
				 * So we default to ORIENTATION_LANDSCAPE
				 */
				layout.Orientation = ORIENTATION_LANDSCAPE;
				break;
		}

		layout.DesktopScaleFactor = monitor->attributes.desktopScaleFactor;
		layout.DeviceScaleFactor = monitor->attributes.deviceScaleFactor;

		auto mask = freerdp_settings_get_uint64(settings, FreeRDP_MonitorOverrideFlags);
		if ((mask & FREERDP_MONITOR_OVERRIDE_ORIENTATION) != 0)
			layout.Orientation = freerdp_settings_get_uint16(settings, FreeRDP_DesktopOrientation);
		if ((mask & FREERDP_MONITOR_OVERRIDE_DESKTOP_SCALE) != 0)
			layout.DesktopScaleFactor =
			    freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
		if ((mask & FREERDP_MONITOR_OVERRIDE_DEVICE_SCALE) != 0)
			layout.DeviceScaleFactor =
			    freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor);
		layouts.emplace_back(layout);
	}
	return layouts;
}

bool sdlDispContext::sendLayout(const rdpMonitor* monitors, size_t nmonitors)
{
	WINPR_ASSERT(monitors);
	WINPR_ASSERT(nmonitors > 0);

	auto settings = _sdl->context()->settings;
	WINPR_ASSERT(settings);

	auto layouts = buildLayout(settings, monitors, nmonitors);
	if (!settings_changed(layouts))
		return true;

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "Sending display layout: %s",
	           _forceLayout                ? "forced"
	           : _last_sent_layout.empty() ? "first on this channel"
	                                       : "layout changed");

	WINPR_ASSERT(_disp);
	const size_t len = layouts.size();
	WINPR_ASSERT(len <= UINT32_MAX);
	const auto ret = IFCALLRESULT(CHANNEL_RC_OK, _disp->SendMonitorLayout, _disp,
	                              static_cast<UINT32>(len), layouts.data());
	if (ret != CHANNEL_RC_OK)
		return false;
	_last_sent_layout = layouts;
	_forceLayout = false;
	return true;
}

bool sdlDispContext::scheduleResize()
{
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
    {
		_resizeRequested = true;
		return false;
	}
	if (_resizeRequested.exchange(true))
		return true;
	SDL_Event wake{};
	wake.type = static_cast<SDL_EventType>(SDL_EVENT_USER_DISPLAY_REFRESH);
	return SDL_PushEvent(&wake);
}

bool sdlDispContext::updateMonitor(SDL_WindowID id)
{
	if (!freerdp_settings_get_bool(_sdl->context()->settings, FreeRDP_DynamicResolutionUpdate))
		return true;

	if (!_sdl->updateWindow(id))
		return false;

	if (!_sdl->updateWindowList())
		return false;

	return scheduleResize();
}

bool sdlDispContext::updateMonitors(SDL_EventType type, SDL_DisplayID displayID)
{
	auto settings = _sdl->context()->settings;
	if (!freerdp_settings_get_bool(settings, FreeRDP_UseMultimon))
		return true;

	if (!freerdp_settings_get_bool(settings, FreeRDP_DynamicResolutionUpdate))
		return true;

	WINPR_UNUSED(displayID);
	bool forceLayout = true;
#if SDL_VERSION_ATLEAST(3, 3, 2)
	// Work-area notifications can accompany focus changes without changing the
	// remote layout. Refresh geometry, but let unchanged layouts be deduplicated.
	forceLayout = type != SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED;
#else
	WINPR_UNUSED(type);
#endif
	// Coalesce cable/dock event bursts and query again while modes settle.
	// Reconcile against current inventory, not a possibly obsolete queued ID.
	_topologyRequested = true;
	// Do not clear a forced reconnect/activation request in the same event burst.
	if (forceLayout)
	{
		WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "Display event 0x%x forces the next layout",
		           static_cast<unsigned>(type));
		_forceRequested = true;
	}
	return scheduleResize();
}

bool sdlDispContext::handleEvent(const SDL_DisplayEvent& ev)
{
	const auto cat = SDL_LOG_CATEGORY_APPLICATION;
	switch (ev.type)
	{
		case SDL_EVENT_DISPLAY_ADDED:
			SDL_LogDebug(cat, "A new display with id %u was connected", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
		case SDL_EVENT_DISPLAY_REMOVED:
			SDL_LogDebug(cat, "The display with id %u was disconnected", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
		case SDL_EVENT_DISPLAY_ORIENTATION:
			SDL_LogDebug(cat, "The orientation of display with id %u was changed", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
		case SDL_EVENT_DISPLAY_MOVED:
			SDL_LogDebug(cat, "The display with id %u was moved", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
		case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
			SDL_LogDebug(cat, "The display with id %u changed scale", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
#if SDL_VERSION_ATLEAST(3, 3, 2)
		case SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED:
			SDL_LogDebug(cat, "The display with id %u changed usable bounds", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
#endif
		case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
			SDL_LogDebug(cat, "The display with id %u changed mode", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
		case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
			SDL_LogDebug(cat, "The display with id %u changed desktop mode", ev.displayID);
			return updateMonitors(ev.type, ev.displayID);
		default:
			return true;
	}
}

bool sdlDispContext::handleEvent(const SDL_WindowEvent& ev)
{
	auto window = _sdl->getWindowForId(ev.windowID);
	if (!window)
		return true;

	auto bordered = freerdp_settings_get_bool(_sdl->context()->settings, FreeRDP_Decorations);
	window->setBordered(bordered);

	switch (ev.type)
	{
		case SDL_EVENT_WINDOW_HIDDEN:
		case SDL_EVENT_WINDOW_MINIMIZED:
			/* Other monitors may still show the session. */
			if (!_sdl->allWindowsHidden())
				return true;
			return _sdl->redraw(true);
		case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
			return updateMonitor(ev.windowID);
		case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
			return updateMonitor(ev.windowID);

		case SDL_EVENT_WINDOW_EXPOSED:
		case SDL_EVENT_WINDOW_SHOWN:
		case SDL_EVENT_WINDOW_MAXIMIZED:
		case SDL_EVENT_WINDOW_RESTORED:
			if (!_sdl->redraw())
				return false;

			/* fallthrough */
			WINPR_FALLTHROUGH
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_RESIZED:
			return updateMonitor(ev.windowID);
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			WINPR_ASSERT(_sdl);
			return _sdl->getInputChannelContext().keyboard_grab(ev.windowID, false);
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
			WINPR_ASSERT(_sdl);
			if (!_sdl->getInputChannelContext().keyboard_grab(ev.windowID, true))
				return false;
			return _sdl->getInputChannelContext().keyboard_focus_in();
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			return _sdl->getInputChannelContext().keyboard_focus_in();

		default:
			return true;
	}
}

UINT sdlDispContext::DisplayControlCaps(DispClientContext* disp, UINT32 maxNumMonitors,
                                        UINT32 maxMonitorAreaFactorA, UINT32 maxMonitorAreaFactorB)
{
	/* we're called only if dynamic resolution update is activated */
	WINPR_ASSERT(disp);

	auto sdlDisp = reinterpret_cast<sdlDispContext*>(disp->custom);
	return sdlDisp->DisplayControlCaps(maxNumMonitors, maxMonitorAreaFactorA,
	                                   maxMonitorAreaFactorB);
}

UINT sdlDispContext::DisplayControlCaps(UINT32 maxNumMonitors, UINT32 maxMonitorAreaFactorA,
                                        UINT32 maxMonitorAreaFactorB)
{
	auto settings = _sdl->context()->settings;
	WINPR_ASSERT(settings);

	WLog_DBG(TAG,
	         "DisplayControlCapsPdu: MaxNumMonitors: %" PRIu32 " MaxMonitorAreaFactorA: %" PRIu32
	         " MaxMonitorAreaFactorB: %" PRIu32 "",
	         maxNumMonitors, maxMonitorAreaFactorA, maxMonitorAreaFactorB);
	{
		/* Windows already applies the monitor data sent at connect in its own display
		 * reconfiguration after logon. A layout PDU that arrives after that reconfiguration has
		 * started is applied in a separate one seconds later, even when it is unchanged. On
		 * hosts with a hardware H.264 encoder that late reconfiguration can deadlock the remote
		 * display driver (RdpIdd watchdog, session ends with reason 17). So on the first channel
		 * of a connection, count the connect-time layout as sent: an identical layout is not
		 * sent at all, a different one still is. */
		std::lock_guard lock(_channelMutex);
		if (!_connectLayout.empty())
		{
			_seedLayout = std::move(_connectLayout);
			_connectLayout.clear();
			_seedRequested = true;
		}
		else
		{
			/* A new display channel means a new server instance, e.g. after a server
			 * redirection. It does not know the layout sent to the previous instance, so force
			 * the current one. */
			_forceRequested = true;
		}

		/* Windows ignores a layout sent right after the caps. Count the caps as a send, so a
		 * layout that still has to go out does so once RESIZE_MIN_DELAY has passed. */
		_lastSentDate = GetTickCount64();
	}
	/* Activate only now: the SDL thread must not send before the seed is in place. */
	_activated = true;

	if (freerdp_settings_get_bool(settings, FreeRDP_Fullscreen))
	{
		/* Window events have long passed by the time the channel comes up, and a fullscreen
		 * window produces no further ones, so schedule the layout. */
		return scheduleResize() ? CHANNEL_RC_OK : CHANNEL_RC_NO_MEMORY;
	}

	WLog_DBG(TAG, "DisplayControlCapsPdu: setting the window as resizable");
	return setWindowResizeable() ? CHANNEL_RC_OK : CHANNEL_RC_NO_MEMORY;
}

bool sdlDispContext::init(DispClientContext* disp)
{
	if (!disp)
		return false;

	auto settings = _sdl->context()->settings;

	if (!settings)
		return false;

	{
		std::lock_guard lock(_channelMutex);
		_disp = disp;
		_activated = false;
	}
	disp->custom = this;

	if (freerdp_settings_get_bool(settings, FreeRDP_DynamicResolutionUpdate))
	{
		disp->DisplayControlCaps = sdlDispContext::DisplayControlCaps;
	}

	_resizeableUpdate = 1;
	return scheduleResize();
}

bool sdlDispContext::uninit(DispClientContext* disp)
{
	if (!disp)
		return false;

	{
		std::lock_guard lock(_channelMutex);
		_disp = nullptr;
		_activated = false;
	}
	_resizeableUpdate = 0;
	return scheduleResize();
}

sdlDispContext::sdlDispContext(SdlContext* sdl) : _sdl(sdl)
{
	WINPR_ASSERT(_sdl);
	WINPR_ASSERT(_sdl->context()->settings);
	WINPR_ASSERT(_sdl->context()->pubSub);

	auto pubSub = _sdl->context()->pubSub;

	if (PubSub_SubscribeActivated(pubSub, sdlDispContext::OnActivated) < 0)
		throw std::exception();
	if (PubSub_SubscribeGraphicsReset(pubSub, sdlDispContext::OnGraphicsReset) < 0)
		throw std::exception();
	if (PubSub_SubscribeStateChanged(pubSub, sdlDispContext::OnStateChanged) < 0)
		throw std::exception();
	std::ignore = scheduleResize();
}

sdlDispContext::~sdlDispContext()
{
	wPubSub* pubSub = _sdl->context()->pubSub;
	WINPR_ASSERT(pubSub);

	PubSub_UnsubscribeActivated(pubSub, sdlDispContext::OnActivated);
	PubSub_UnsubscribeGraphicsReset(pubSub, sdlDispContext::OnGraphicsReset);
	PubSub_UnsubscribeStateChanged(pubSub, sdlDispContext::OnStateChanged);
}
