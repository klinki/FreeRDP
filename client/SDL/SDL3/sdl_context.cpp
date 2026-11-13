/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client
 *
 * Copyright 2022 Armin Novak <armin.novak@thincast.com>
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <string>
#include <freerdp/client/cmdline.h>
#include <freerdp/codec/yuv.h>

#include "sdl_context.hpp"
#include "sdl_hosted.hpp"
#ifdef WITH_SDL_LAUNCHER_BRIDGE
#include "sdl_launcher.hpp"
#endif
#include "sdl_config.hpp"
#include "sdl_channels.hpp"
#include "sdl_input_mapping.hpp"
#include "sdl_monitor.hpp"
#include "sdl_monitor_layout.hpp"
#include "sdl_fullscreen_inset.hpp"
#include "sdl_pointer.hpp"
#include "sdl_render_geometry.hpp"
#include "sdl_touch.hpp"
#include "sdl_prefs.hpp"
#include "sdl_session_loop.hpp"

#include <sdl_common_utils.hpp>
#include <scoped_guard.hpp>

#include "dialogs/sdl_dialogs.hpp"
#include <freerdp/client/aad_helper.h>

static constexpr auto sdl_allow_screensaver = "sdl-allow-screensaver";
static constexpr auto sdl_connection_dialog = "sdl-connection-dialog";
static constexpr auto sdl_monitor_scale = "sdl-monitor-scale";
static constexpr auto sdl_refresh_menu = "sdl-refresh-menu";

SdlContext::SdlContext(rdpContext* context)
    : _context(context), _log(WLog_Get(CLIENT_TAG("SDL"))), _cursor(nullptr, sdl_Pointer_FreeCopy),
      _rdpThreadRunning(false), _primary(nullptr, SDL_DestroySurface), _disp(this), _input(this),
      _clip(this), _rail(this), _dialog(_log)
{
	WINPR_ASSERT(context);
	setMetadata();

	auto instance = _context->instance;
	WINPR_ASSERT(instance);

	instance->PreConnect = preConnect;
	instance->PostConnect = postConnect;
	instance->PostDisconnect = postDisconnect;
	instance->PostFinalDisconnect = postFinalDisconnect;
	instance->AuthenticateEx = sdl_authenticate_ex;
	instance->VerifyCertificateEx = sdl_verify_certificate_ex;
	instance->VerifyChangedCertificateEx = sdl_verify_changed_certificate_ex;
	instance->LogonErrorInfo = sdl_logon_error_info;
	instance->PresentGatewayMessage = sdl_present_gateway_message;
	instance->ChooseSmartcard = sdl_choose_smartcard;
	instance->RetryDialog = [](freerdp* i, const char* what, size_t current, void* user) -> SSIZE_T
	{
		auto sdl = get_context(i->context);
		if (sdl->_stopRequested)
			return -1;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
		if (auto bridge = SdlLauncher::active())
			return bridge->retry(current, what);
#endif
		return sdl_retry_dialog(i, what, current, user);
	};
	instance->GetAccessToken = client_failsafe_get_access_token;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	instance->AuthenticateEx = [](freerdp* i, char** u, char** p, char** d,
	                              rdp_auth_reason reason) -> BOOL
	{
		if (auto bridge = SdlLauncher::active())
			return bridge->authenticate(u, p, d, reason);
		return sdl_authenticate_ex(i, u, p, d, reason);
	};
	instance->VerifyCertificateEx = [](freerdp* i, const char* host, UINT16 port, const char* cn,
	                                   const char* subject, const char* issuer, const char* fp,
	                                   DWORD flags) -> DWORD
	{
		if (auto bridge = SdlLauncher::active())
			return bridge->certificate(host, port, cn, subject, issuer, fp, nullptr, nullptr,
			                           nullptr, flags);
		return sdl_verify_certificate_ex(i, host, port, cn, subject, issuer, fp, flags);
	};
	instance->VerifyChangedCertificateEx =
	    [](freerdp* i, const char* host, UINT16 port, const char* cn, const char* subject,
	       const char* issuer, const char* fp, const char* oldSubject, const char* oldIssuer,
	       const char* oldFp, DWORD flags) -> DWORD
	{
		if (auto bridge = SdlLauncher::active())
			return bridge->certificate(host, port, cn, subject, issuer, fp, oldSubject, oldIssuer,
			                           oldFp, flags);
		return sdl_verify_changed_certificate_ex(i, host, port, cn, subject, issuer, fp, oldSubject,
		                                         oldIssuer, oldFp, flags);
	};
	instance->LogonErrorInfo = [](freerdp* i, UINT32 data, UINT32 type) -> int
	{
		if (SdlLauncher::active())
			return 0;
		return sdl_logon_error_info(i, data, type);
	};
#endif

	/* TODO: Client display set up */

	_args.push_back({ sdl_allow_screensaver, COMMAND_LINE_VALUE_BOOL, nullptr, BoolValueFalse,
	                  nullptr, -1, nullptr, "Allow local screensaver to activate" });
	/* BoolValueTrue does not compile in C++, any non null default means on */
	_args.push_back({ sdl_connection_dialog, COMMAND_LINE_VALUE_BOOL, nullptr, "on", nullptr, -1,
	                  nullptr, "the connection dialog with progress and errors" });
	_args.push_back({ sdl_monitor_scale, COMMAND_LINE_VALUE_REQUIRED,
	                  "<id>=<desktop>/<device>[,<id>=<desktop>/<device>...]", nullptr, nullptr, -1,
	                  nullptr, "Override RDP scaling for individual SDL monitors. Monitor IDs are "
	                           "shown by /list:monitor." });
	_args.push_back({ sdl_refresh_menu, COMMAND_LINE_VALUE_BOOL, nullptr, BoolValueFalse, nullptr,
	                  -1, nullptr,
	                  "the A-E screen refresh method panel on hovering the connection bar's "
	                  "refresh button (off: the button always runs combined recovery)" });

	/* Push a null element used as abort when iterating the array */
	_args.push_back({ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr });
}

void SdlContext::setHasCursor(bool val)
{
	this->_cursor_visible = val;
}

bool SdlContext::hasCursor() const
{
	return _cursor_visible;
}

void SdlContext::setMetadata()
{
	auto wmclass = freerdp_settings_get_string(_context->settings, FreeRDP_WmClass);
	if (!wmclass || (strlen(wmclass) == 0))
		wmclass = SDL_CLIENT_UUID;

	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, wmclass);
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, SDL_CLIENT_NAME);
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING, SDL_CLIENT_VERSION);
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, SDL_CLIENT_VENDOR);
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_COPYRIGHT_STRING, SDL_CLIENT_COPYRIGHT);
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING, SDL_CLIENT_URL);
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, SDL_CLIENT_TYPE);
}

int SdlContext::start()
{
	_thread = std::thread(rdpThreadRun, this);
	return 0;
}

bool SdlContext::requestStop()
{
	/* A UI failure is not a network disconnect. Reconnect clears the core
	 * abort event, so keep a separate, permanent stop request. Preserve the
	 * original error rather than replacing it with CONNECT_CANCELLED. */
	_stopRequested = true;
	return SetEvent(freerdp_abort_event(context())) != FALSE;
}

int SdlContext::join()
{
	if (!requestStop())
		return -1;
	if (_thread.joinable())
		_thread.join();
	return 0;
}

void SdlContext::cleanup()
{
	std::unique_lock lock(_critical);
	_windows.clear();
	_fullscreenState.clear();
	_topBarPointer = { -1.0f, -1.0f };
	_topBarHoveredButton = SdlTopBarButton::None;
	_topBarPointerOver = false;
	_topBarGestureOwner = TopBarGestureOwner::None;
	_topBarCaptures = {};
	_topBarDrag = TopBarDragMode::None;
	_topBarDragWindow = 0;
	_dialog.destroy();
	_primary.reset();
}

bool SdlContext::shallAbort(bool ignoreDialogs)
{
	if (_stopRequested)
		return true;
	std::unique_lock lock(_critical);
	if (freerdp_shall_disconnect_context(context()))
	{
		if (ignoreDialogs)
			return true;
		if (_rdpThreadRunning)
			return false;
		return !getDialog().isRunning();
	}
	return false;
}

/* Called before a connection is established.
 * Set all configuration options to support and load channels here. */
BOOL SdlContext::preConnect(freerdp* instance)
{
	WINPR_ASSERT(instance);
	WINPR_ASSERT(instance->context);

	auto sdl = get_context(instance->context);
	if (sdl->_stopRequested)
		return FALSE;

	auto settings = instance->context->settings;
	WINPR_ASSERT(settings);
	if (!sdl->validateMonitorScaleOverrides())
		return FALSE;
	/* Include the overridden attributes in the initial CS_MONITOR_EX block. */
	if (!sdl->_monitorScaleOverrides.empty() &&
	    !freerdp_settings_set_bool(settings, FreeRDP_HasMonitorAttributes, TRUE))
		return FALSE;

	if (!freerdp_settings_set_bool(settings, FreeRDP_CertificateCallbackPreferPEM, TRUE))
		return FALSE;

	/* Optional OS identifier sent to server */
	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX))
		return FALSE;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_SDL))
		return FALSE;
	/* OrderSupport is initialized at this point.
	 * Only override it if you plan to implement custom order
	 * callbacks or deactivate certain features. */
	/* Register the channel listeners.
	 * They are required to set up / tear down channels if they are loaded. */
	if (PubSub_SubscribeChannelConnected(instance->context->pubSub,
	                                     sdl_OnChannelConnectedEventHandler) < 0)
		return FALSE;
	if (PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
	                                        sdl_OnChannelDisconnectedEventHandler) < 0)
		return FALSE;
	if (PubSub_SubscribeUserNotification(instance->context->pubSub,
	                                     sdl_OnUserNotificationEventHandler) < 0)
		return FALSE;

	if (!freerdp_settings_get_bool(settings, FreeRDP_AuthenticationOnly))
	{
		UINT32 maxWidth = 0;
		UINT32 maxHeight = 0;

		if (!sdl_detect_monitors(sdl, &maxWidth, &maxHeight))
			return FALSE;

		if ((maxWidth != 0) && (maxHeight != 0) &&
		    !freerdp_settings_get_bool(settings, FreeRDP_SmartSizing))
		{
			WLog_Print(sdl->getWLog(), WLOG_INFO, "Update size to %ux%u", maxWidth, maxHeight);
			if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, maxWidth))
				return FALSE;
			if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, maxHeight))
				return FALSE;
		}

		/**
		 * If /f is specified in combination with /smart-sizing:widthxheight then
		 * we run the session in the /smart-sizing dimensions scaled to full screen
		 */

		const uint32_t sw = freerdp_settings_get_uint32(settings, FreeRDP_SmartSizingWidth);
		const uint32_t sh = freerdp_settings_get_uint32(settings, FreeRDP_SmartSizingHeight);
		const BOOL sm = freerdp_settings_get_bool(settings, FreeRDP_SmartSizing);
		if (sm && (sw > 0) && (sh > 0))
		{
			const BOOL mm = freerdp_settings_get_bool(settings, FreeRDP_UseMultimon);
			if (mm)
				WLog_Print(sdl->getWLog(), WLOG_WARN,
				           "/smart-sizing and /multimon are currently not supported, ignoring "
				           "/smart-sizing!");
			else
			{
				sdl->_windowWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
				sdl->_windowHeight = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

				if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, sw))
					return FALSE;
				if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, sh))
					return FALSE;
			}
		}
	}
	else
	{
		/* Check +auth-only has a username and password. */
		if (!freerdp_settings_get_string(settings, FreeRDP_Password))
		{
			WLog_Print(sdl->getWLog(), WLOG_INFO,
			           "auth-only, but no password set. Please provide one.");
			return FALSE;
		}

		if (!freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, TRUE))
			return FALSE;

		WLog_Print(sdl->getWLog(), WLOG_INFO, "Authentication only. Don't connect SDL.");
	}

	if (!sdl->getInputChannelContext().initialize())
		return FALSE;

	sdl->_credentialsRead = false;

	/* Settings are consistent at this point, the dialog can be created. */
	auto prefs = SdlPref::instance();
	auto dialog = prefs->get_bool("SDL_ConnectionDialog", true);
	if (sdl->_connectionDialog && prefs->is_commandline_override_allowed())
		dialog = *sdl->_connectionDialog;
	if (dialog && !freerdp_settings_get_bool(settings, FreeRDP_AuthenticationOnly))
	{
		sdl->getDialog().create(instance->context);
		sdl->getDialog().setTitle("Connecting to '%s'", freerdp_settings_get_server_name(settings));
		sdl->getDialog().showInfo("The connection is being established\n\nPlease wait...");
	}

	/* TODO: Any code your client requires */
	return TRUE;
}

/* Called after a RDP connection was successfully established.
 * Settings might have changed during negotiation of client / server feature
 * support.
 *
 * Set up local framebuffers and paing callbacks.
 * If required, register pointer callbacks to change the local mouse cursor
 * when hovering over the RDP window
 */
BOOL SdlContext::postConnect(freerdp* instance)
{
	WINPR_ASSERT(instance);

	auto context = instance->context;
	WINPR_ASSERT(context);

	auto sdl = get_context(context);

	// Retry was successful, discard dialog
	sdl->getDialog().show(false);

	if (freerdp_settings_get_bool(context->settings, FreeRDP_AuthenticationOnly))
	{
		/* Check +auth-only has a username and password. */
		if (!freerdp_settings_get_string(context->settings, FreeRDP_Password))
		{
			WLog_Print(sdl->getWLog(), WLOG_INFO,
			           "auth-only, but no password set. Please provide one.");
			return FALSE;
		}

		WLog_Print(sdl->getWLog(), WLOG_INFO, "Authentication only. Don't connect to X.");
		return TRUE;
	}

	if (!sdl->waitForWindowsCreated())
		return FALSE;

	sdl->_sdlPixelFormat = SDL_PIXELFORMAT_BGRA32;
	if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
		return FALSE;

	if (!sdl->createPrimary())
		return FALSE;

	if (!sdl_register_pointer(instance->context->graphics))
		return FALSE;

	WINPR_ASSERT(context->update);

	context->update->BeginPaint = beginPaint;
	context->update->EndPaint = endPaint;
	context->update->PlaySound = playSound;
	context->update->DesktopResize = desktopResize;
	context->update->SetKeyboardIndicators = sdlInput::keyboard_set_indicators;
	context->update->SetKeyboardImeStatus = sdlInput::keyboard_set_ime_status;

	if (!sdl->setResizeable(false))
		return FALSE;
	if (!sdl->setFullscreen(freerdp_settings_get_bool(context->settings, FreeRDP_Fullscreen) ||
	                            freerdp_settings_get_bool(context->settings, FreeRDP_UseMultimon),
	                        true))
		return FALSE;
	sdl->setConnected(true);
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active())
		bridge->state("connected");
#endif
	return TRUE;
}

/* This function is called whether a session ends by failure or success.
 * Clean up everything allocated by pre_connect and post_connect.
 */
void SdlContext::postDisconnect(freerdp* instance)
{
	if (!instance)
		return;

	if (!instance->context)
		return;

	auto sdl = get_context(instance->context);
	// Serialize teardown with output control and completed-frame refreshes.
	std::lock_guard outputLock(sdl->_screenOutputMutex);
	std::unique_lock frameLock(sdl->_critical);
	sdl->setConnected(false);
	sdl->_updates.clear();
	gdi_free(instance);
}

void SdlContext::postFinalDisconnect(freerdp* instance)
{
	if (!instance)
		return;

	if (!instance->context)
		return;

	PubSub_UnsubscribeChannelConnected(instance->context->pubSub,
	                                   sdl_OnChannelConnectedEventHandler);
	PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
	                                      sdl_OnChannelDisconnectedEventHandler);
	PubSub_UnsubscribeUserNotification(instance->context->pubSub,
	                                   sdl_OnUserNotificationEventHandler);
}

/* Create a SDL surface from the GDI buffer */
bool SdlContext::createPrimary()
{
	auto gdi = context()->gdi;
	WINPR_ASSERT(gdi);

	_primary = SDLSurfacePtr(
	    SDL_CreateSurfaceFrom(static_cast<int>(gdi->width), static_cast<int>(gdi->height),
	                          pixelFormat(), gdi->primary_buffer, static_cast<int>(gdi->stride)),
	    SDL_DestroySurface);
	if (!_primary)
		return false;

	SDL_SetSurfaceBlendMode(_primary.get(), SDL_BLENDMODE_NONE);
	SDL_Rect surfaceRect = { 0, 0, gdi->width, gdi->height };
	SDL_FillSurfaceRect(_primary.get(), &surfaceRect,
	                    SDL_MapSurfaceRGBA(_primary.get(), 0, 0, 0, 0xff));

	return true;
}

bool SdlContext::createWindows()
{
	auto settings = context()->settings;
	const auto& title = windowTitle();

	ScopeGuard guard1([&]() { _windowsCreatedEvent.set(); });

	UINT32 windowCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);

	/* The monitor definitions are sorted (primary first), so they need not be in the same
	 * order as the monitor IDs. Match them by display, fall back to the index. */
	auto monitorFor = [&](UINT32 index, int64_t id)
	{
		for (UINT32 y = 0; y < windowCount; y++)
		{
			auto cur = static_cast<rdpMonitor*>(
			    freerdp_settings_get_pointer_array_writable(settings, FreeRDP_MonitorDefArray, y));
			if (cur && (static_cast<int64_t>(cur->orig_screen) == id))
				return cur;
		}
		return static_cast<rdpMonitor*>(
		    freerdp_settings_get_pointer_array_writable(settings, FreeRDP_MonitorDefArray, index));
	};

	Sint32 originX = 0;
	Sint32 originY = 0;
	/* Windows that already exist stay tracked: forgetting them would leave
	 * their fullscreen state out of the session's. */
	if (_windows.empty())
		_fullscreenState.resetWindows();
	SDL_WindowID primaryWindow = 0;
	_topBarPointer = { -1.0f, -1.0f };
	_topBarHoveredButton = SdlTopBarButton::None;
	_topBarPointerOver = false;
	_topBarGestureOwner = TopBarGestureOwner::None;
	_topBarCaptures = {};
	_topBarDrag = TopBarDragMode::None;
	_topBarDragWindow = 0;
	for (UINT32 x = 0; x < windowCount; x++)
	{
		auto id = monitorId(x);
		if (id < 0)
			return false;

		auto monitor = monitorFor(x, id);

		originX = std::min<Sint32>(monitor->x, originX);
		originY = std::min<Sint32>(monitor->y, originY);
	}

	for (UINT32 x = 0; x < windowCount; x++)
	{
		auto id = monitorId(x);
		if (id < 0)
			return false;

		auto monitor = monitorFor(x, id);

		Uint32 w = WINPR_ASSERTING_INT_CAST(Uint32, monitor->width);
		Uint32 h = WINPR_ASSERTING_INT_CAST(Uint32, monitor->height);
		if (!(freerdp_settings_get_bool(settings, FreeRDP_UseMultimon) ||
		      freerdp_settings_get_bool(settings, FreeRDP_Fullscreen)))
		{
			if (_windowWidth > 0)
				w = _windowWidth;
			else
				w = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);

			if (_windowHeight > 0)
				h = _windowHeight;
			else
				h = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
		}

		Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;

		if (freerdp_settings_get_bool(settings, FreeRDP_Fullscreen) &&
		    !freerdp_settings_get_bool(settings, FreeRDP_UseMultimon))
		{
			flags |= SDL_WINDOW_FULLSCREEN;
		}

		if (freerdp_settings_get_bool(settings, FreeRDP_UseMultimon))
		{
			flags |= SDL_WINDOW_BORDERLESS;
		}

		if (!freerdp_settings_get_bool(settings, FreeRDP_Decorations))
			flags |= SDL_WINDOW_BORDERLESS;

		/* RemoteApp: the desktop only shows when needed (drawToWindows). */
		if (freerdp_settings_get_bool(settings, FreeRDP_RemoteApplicationMode))
			flags |= SDL_WINDOW_HIDDEN;

		// FreeRDP sorts definitions by primary and position. Keep each window
		// attached to its definition's display, not the unsorted selection index.
		const auto did = (freerdp_settings_get_bool(settings, FreeRDP_UseMultimon) ||
		                  freerdp_settings_get_bool(settings, FreeRDP_Fullscreen))
		                     ? monitor->orig_screen
		                     : WINPR_ASSERTING_INT_CAST(SDL_DisplayID, id);
		auto window = SdlWindow::create(did, title, flags, w, h);
#ifdef WITH_SDL_LAUNCHER_BRIDGE
		if (auto bridge = SdlLauncher::active()) bridge->configureWindowGeometry(window.window());
#endif
		if (freerdp_settings_get_bool(settings, FreeRDP_UseMultimon))
		{
			// Cache local coordinates; negotiated coordinates are relative to the
			// selected primary and must not be mixed with newly added displays.
			window.setMonitor(getDisplay(did));
			// Until its fullscreen transition completes, report the window as shorter
			// already, as the connect-time monitor was sent. Otherwise a layout sent
			// meanwhile differs from it, and Windows reconfigures a second time.
			const auto inset = fullscreenTopInset(did);
			auto shortened = getDisplay(did);
			if (!freerdp_settings_get_bool(settings, FreeRDP_SpanMonitors) &&
			    !freerdp_settings_get_bool(settings, FreeRDP_RemoteApplicationMode) &&
			    (inset > 0) && (inset < shortened.height))
			{
				shortened.height -= inset;
				window.setMonitor(shortened, false);
			}
		}

		if (freerdp_settings_get_bool(settings, FreeRDP_UseMultimon))
		{
			window.setOffsetX(originX - monitor->x);
			window.setOffsetY(originY - monitor->y);
		}

		_fullscreenState.addWindow(window.id());
		/* Definitions follow the display selection, not the primary: give
		 * the bar to the negotiated primary's window. */
		if (monitor->is_primary && primaryWindow == 0)
			primaryWindow = window.id();
		_windows.insert({ window.id(), std::move(window) });
	}
	if (primaryWindow != 0)
		_fullscreenState.setOwner(primaryWindow, false);

	return true;
}

#if defined(__EMSCRIPTEN__)
/* in sdl_monitors_emscripten.js */
extern "C" int freerdp_web_monitors_js(int32_t* edges, int max);

/* A browser has one canvas, so one window. The page may show it across several screens and
 * split it into monitors (sdl_monitors_emscripten.js): the server then places its windows,
 * panels and maximized windows on each, as on separate physical screens. canvas is the
 * window's monitor; returns the monitors it is split into, or none. */
static std::vector<rdpMonitor> sdl_web_monitors(const rdpMonitor& canvas)
{
	constexpr int max = 16;
	std::array<int32_t, 5 * max> edges{};
	const auto n = freerdp_web_monitors_js(edges.data(), max);
	if ((n < 2) || (n > max))
		return {};

	const auto at = [](int32_t edge, int32_t size)
	{ return static_cast<INT32>(static_cast<int64_t>(edge) * size / 10000); };
	std::vector<rdpMonitor> list;
	for (int i = 0; i < n; i++)
	{
		const int32_t* e = &edges.at(5 * static_cast<size_t>(i));
		const auto left = at(e[0], canvas.width);
		const auto top = at(e[1], canvas.height);
		const auto right = at(e[2], canvas.width);
		const auto bottom = at(e[3], canvas.height);
		if ((right <= left) || (bottom <= top))
			return {};

		rdpMonitor m = canvas;
		m.x = left;
		m.y = top;
		m.width = right - left;
		m.height = bottom - top;
		m.is_primary = (e[4] != 0);
		m.attributes.physicalWidth =
		    static_cast<UINT32>(static_cast<uint64_t>(canvas.attributes.physicalWidth) *
		                        static_cast<UINT32>(m.width) / static_cast<UINT32>(canvas.width));
		m.attributes.physicalHeight =
		    static_cast<UINT32>(static_cast<uint64_t>(canvas.attributes.physicalHeight) *
		                        static_cast<UINT32>(m.height) / static_cast<UINT32>(canvas.height));
		list.push_back(m);
	}
	return list;
}
#endif

bool SdlContext::updateWindowList()
{
	std::vector<rdpMonitor> list;
	std::vector<rdpMonitor> displays;
	list.reserve(_windows.size());
	displays.reserve(_windows.size());
	for (const auto& win : _windows)
	{
		auto monitor = win.second.monitor(false);
		// Several removals can be queued after SDL has removed all of them.
		if (!SDL_GetDisplayName(monitor.orig_screen))
			continue;
		applyMonitorScaleOverride(monitor);
		list.push_back(monitor);
		// The full display bounds, which tell which monitors touch each other.
		const auto size = win.second.displaySize();
		monitor.width = std::max(monitor.width, size.x);
		monitor.height = std::max(monitor.height, size.y);
		displays.push_back(monitor);
	}

	// Keep the previous valid layout until the remaining removal events arrive.
	if (list.empty())
		return true;
	if (list.size() == 1)
	{
		list.front().x = 0;
		list.front().y = 0;
		list.front().is_primary = true;
	}
	else if (!sdl_close_monitor_gaps(list, displays))
		WLog_Print(getWLog(), WLOG_WARN,
		           "multimon layout has monitors that are not adjacent to another one");

	applyPrimaryMonitor(list);
	Sint32 originX = list.front().x, originY = list.front().y;
	for (const auto& monitor : list)
	{
		originX = std::min(originX, monitor.x);
		originY = std::min(originY, monitor.y);
	}
	for (auto& win : _windows)
	{
		const auto id = win.second.monitor(false).orig_screen;
		const auto monitor = std::find_if(list.cbegin(), list.cend(),
		                                 [id](const rdpMonitor& m) { return m.orig_screen == id; });
		if (monitor != list.cend())
		{
			win.second.setOffsetX(originX - monitor->x);
			win.second.setOffsetY(originY - monitor->y);
		}
	}

#if defined(__EMSCRIPTEN__)
	if ((list.size() == 1) && (list.at(0).width > 0) && (list.at(0).height > 0))
	{
		auto split = sdl_web_monitors(list.at(0));
		if (!split.empty())
			list = std::move(split);
	}
#endif

	return freerdp_settings_set_monitor_def_array_sorted(context()->settings, list.data(),
	                                                     list.size());
}

bool SdlContext::updateWindow(SDL_WindowID id)
{
	/* In fullscreen the monitor kept the size measured with the probe window at startup and was
	 * never updated, so a session the server started at another size stayed that way. Only the
	 * window knows its real size, so ask it in fullscreen as well. */
	auto& w = _windows.at(id);
	const bool multimon = freerdp_settings_get_bool(_context->settings, FreeRDP_UseMultimon);
	if (multimon)
	{
		// A migrated window still belongs to its removed display until topology
		// reconciliation. Each owning fullscreen window can use its drawable,
		// which excludes the space macOS reserves above notched content.
		if (w.displayIndex() != w.monitor(false).orig_screen)
			return true;
		if ((_windows.size() > 1) && !(SDL_GetWindowFlags(w.window()) & SDL_WINDOW_FULLSCREEN))
			return true; // Retain physical geometry while leaving multimon fullscreen.
	}

	auto r = w.rect();
	if ((r.w <= 0) || (r.h <= 0))
		return true; // Retain the last valid geometry until the window settles.
	// On a notched display, macOS keeps fullscreen content below the menu bar band, so a
	// fullscreen window as tall as the display has not finished its transition. Report the
	// height it will have, as sent at connect, or the layout sent meanwhile differs from it.
	// Windows that are not fullscreen or entering it keep their drawable height. SDL's flag is
	// already clear when the leave event is handled, before the session's fullscreen state
	// observes that event, so only the flag and a request to enter count.
	const auto now = SDL_GetTicks();
	const bool fullscreen =
	    ((SDL_GetWindowFlags(w.window()) & SDL_WINDOW_FULLSCREEN) != 0) ||
	    (_fullscreenState.pending(w.id(), now) && _fullscreenState.target());
	const auto display = _displays.find(SDL_GetDisplayForWindow(w.window()));
	if (fullscreen && !freerdp_settings_get_bool(_context->settings, FreeRDP_SpanMonitors) &&
	    !freerdp_settings_get_bool(_context->settings, FreeRDP_RemoteApplicationMode) &&
	    (display != _displays.end()))
	{
		const auto inset = fullscreenTopInset(display->first);
		if ((inset > 0) && (r.h == display->second.height) && (r.h > inset))
			r.h -= inset;
	}
	// Multimon origins stay in the shared desktop coordinate space.
	auto m = w.monitor(!multimon);
	applyMonitorScaleOverride(m);
	m.width = r.w;
	m.height = r.h;
	w.setMonitor(m, false);
	return true;
}

std::string SdlContext::windowTitle() const
{
	const char* prefix = "FreeRDP:";

	const auto windowTitle = freerdp_settings_get_string(context()->settings, FreeRDP_WindowTitle);
	if (windowTitle)
		return windowTitle;

	const auto name = freerdp_settings_get_server_name(context()->settings);
	const auto port = freerdp_settings_get_uint32(context()->settings, FreeRDP_ServerPort);
	const auto addPort = (port != 3389);

	std::stringstream ss;
	ss << prefix << " " << name;

	if (addPort)
		ss << ":" << port;

	return ss.str();
}

bool SdlContext::waitForWindowsCreated()
{
	{
		std::unique_lock<CriticalSection> lock(_critical);
		if (_stopRequested)
			return false;
		_windowsCreatedEvent.clear();
		if (!sdl_push_user_event(SDL_EVENT_USER_CREATE_WINDOWS, this))
			return false;
	}

	HANDLE handles[] = { _windowsCreatedEvent.handle(), freerdp_abort_event(context()) };

	const DWORD rc = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE);
	switch (rc)
	{
		case WAIT_OBJECT_0:
			return true;
		default:
			return false;
	}
}

/* This function is called when the library completed composing a new
 * frame. Read out the changed areas and blit them to your output device.
 * The image buffer will have the format specified by gdi_init
 */
BOOL SdlContext::endPaint(rdpContext* context)
{
	auto sdl = get_context(context);
	WINPR_ASSERT(sdl);

	auto gdi = context->gdi;
	WINPR_ASSERT(gdi);
	WINPR_ASSERT(gdi->primary);
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	// The composing thread owns this completed BGRA frame. Only a bounded copy runs here;
	// PNG encoding and IPC run on a worker outside the render critical section.
	if (auto bridge = SdlLauncher::active())
		std::ignore = bridge->captureThumbnail(gdi->primary_buffer, gdi->width, gdi->height, gdi->stride);
#endif

	HGDI_DC hdc = gdi->primary->hdc;
	WINPR_ASSERT(hdc);
	if (!hdc->hwnd)
		return TRUE;

	HGDI_WND hwnd = hdc->hwnd;
	WINPR_ASSERT(hwnd->invalid || (hwnd->ninvalid == 0));

	if (hwnd->invalid->null)
		return TRUE;

	WINPR_ASSERT(hwnd->invalid);
	if (gdi->suppressOutput || hwnd->invalid->null)
		return TRUE;

	const INT32 ninvalid = hwnd->ninvalid;
	const GDI_RGN* cinvalid = hwnd->cinvalid;

	if (ninvalid < 1)
		return TRUE;

	std::vector<SDL_Rect> rects;
	for (INT32 x = 0; x < ninvalid; x++)
	{
		auto& rgn = cinvalid[x];
		rects.push_back({ rgn.x, rgn.y, rgn.w, rgn.h });
	}

	return sdl->push(rects);
}

void SdlContext::sdl_client_cleanup(int exit_code, const std::string& error_msg)
{
	rdpSettings* settings = context()->settings;
	WINPR_ASSERT(settings);

	_rdpThreadRunning = false;
	/* Tearing down: the error dialog or quit owns user feedback from here.
	 * Store directly — no overlay event, the main loop is draining. */
	_reconnecting = false;
	bool showError = false;
	if (freerdp_settings_get_bool(settings, FreeRDP_AuthenticationOnly))
		WLog_Print(getWLog(), WLOG_INFO, "Authentication only, exit status %s [%" PRId32 "]",
		           sdl::error::exitCodeToTag(exit_code), exit_code);
	else
	{
		switch (exit_code)
		{
			case sdl::error::SUCCESS:
			case sdl::error::DISCONNECT:
			case sdl::error::LOGOFF:
			case sdl::error::DISCONNECT_BY_USER:
			case sdl::error::CONNECT_CANCELLED:
				break;
			default:
			{
#ifdef WITH_SDL_LAUNCHER_BRIDGE
				/* The launcher reports errors itself. */
				if (!SdlLauncher::active())
#endif
				{
					showError = true;
					getDialog().showError(error_msg);
				}
			}
			break;
		}
	}

	if (!showError)
		getDialog().show(false);

	_exitDetail = error_msg;
	_exitCode = exit_code;
	std::ignore = sdl_push_user_event(SDL_EVENT_USER_QUIT);
	SDL_CleanupTLS();
}

int SdlContext::sdl_client_thread_connect(std::string& error_msg)
{
	auto instance = context()->instance;
	WINPR_ASSERT(instance);

	_rdpThreadRunning = true;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active())
		bridge->state("connecting");
#endif
	BOOL rc = freerdp_connect(instance);
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	// A rejected credential is offered back for editing; never silently reuse it.
	if (auto bridge = SdlLauncher::active())
	{
		while (!rc && !_stopRequested && !bridge->cancelled() &&
		       SdlLauncher::isAuthenticationError(freerdp_get_last_error(context())))
		{
			auto settings = context()->settings;
			const auto duplicate = [settings](FreeRDP_Settings_Keys_String key) -> char*
			{
				const auto value = freerdp_settings_get_string(settings, key);
				return value ? _strdup(value) : nullptr;
			};
			char *u = duplicate(FreeRDP_Username), *d = duplicate(FreeRDP_Domain), *p = nullptr;
			const bool accepted = bridge->authenticate(&u, &p, &d, AUTH_NLA, true);
			if (accepted)
			{
				std::ignore = freerdp_settings_set_string(settings, FreeRDP_Username, u);
				std::ignore = freerdp_settings_set_string(settings, FreeRDP_Domain, d);
				std::ignore = freerdp_settings_set_string(settings, FreeRDP_Password, p);
			}
			if (p)
				SecureZeroMemory(p, strlen(p));
			free(u);
			free(d);
			free(p);
			if (!accepted || _stopRequested)
				break;
			bridge->state("connecting");
			rc = freerdp_connect(instance);
		}
	}
#endif

	rdpSettings* settings = context()->settings;
	WINPR_ASSERT(settings);

	int exit_code = sdl::error::SUCCESS;
	if (!rc)
	{
		UINT32 error = freerdp_get_last_error(context());
		exit_code = sdl::error::errorToExitCode(error);
	}

	if (freerdp_settings_get_bool(settings, FreeRDP_AuthenticationOnly))
	{
		DWORD code = freerdp_get_last_error(context());
		freerdp_abort_connect_context(context());
		WLog_Print(getWLog(), WLOG_ERROR, "Authentication only, %s [0x%08" PRIx32 "] %s",
		           freerdp_get_last_error_name(code), code, freerdp_get_last_error_string(code));
		return exit_code;
	}

	if (!rc)
	{
		DWORD code = freerdp_error_info(instance);
		if (exit_code == sdl::error::SUCCESS)
		{
			char* msg = nullptr;
			size_t len = 0;
			exit_code = error_info_to_error(&code, &msg, &len);
			if (msg)
				error_msg = msg;
			free(msg);
		}

		auto last = freerdp_get_last_error(context());
		if (error_msg.empty())
		{
			char* msg = nullptr;
			size_t len = 0;
			winpr_asprintf(&msg, &len, "%s [0x%08" PRIx32 "]\n%s",
			               freerdp_get_last_error_name(last), last,
			               freerdp_get_last_error_string(last));
			if (msg)
				error_msg = msg;
			free(msg);
		}

		if (exit_code == sdl::error::SUCCESS)
		{
			if (last == FREERDP_ERROR_AUTHENTICATION_FAILED)
				exit_code = sdl::error::AUTH_FAILURE;
			else if (code == ERRINFO_SUCCESS)
				exit_code = sdl::error::CONN_FAILED;
		}

		getDialog().show(false);
	}

	return exit_code;
}

int SdlContext::sdl_client_thread_run(std::string& error_msg)
{
	auto instance = context()->instance;
	WINPR_ASSERT(instance);

	int exit_code = sdl::error::SUCCESS;
	const auto stopped = [this]() {
#ifdef WITH_SDL_LAUNCHER_BRIDGE
		if (auto bridge = SdlLauncher::active(); bridge && bridge->cancelled())
			return true;
#endif
		return _stopRequested.load();
	};
	const auto result = sdl_run_session_loop(stopped,
	    [this]() { return freerdp_shall_disconnect_context(context()) != FALSE; },
	    [this, instance, &stopped]() {
		    // Focus synchronization writes can discover transport loss before the
		    // event-handle check. They must use the same recovery path.
		    if (freerdp_focus_required(instance))
		    {
			    auto& input = getInputChannelContext();
			    if (!input.keyboard_focus_in() || !input.keyboard_focus_in())
			    {
				    WLog_Print(getWLog(), WLOG_DEBUG, "Reconnect checkpoint: focus-sync write failed");
				    return SdlSessionStep::FocusWriteFailure;
			    }
		    }
		    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
		    const DWORD count = freerdp_get_event_handles(context(), handles, ARRAYSIZE(handles) - 1);
		    if (count == 0)
		    {
			    WLog_Print(getWLog(), WLOG_ERROR, "freerdp_get_event_handles failed");
			    return freerdp_get_last_error(context()) == FREERDP_ERROR_CONNECT_TRANSPORT_FAILED
			               ? SdlSessionStep::TransportFailure : SdlSessionStep::LocalFailure;
		    }
		    handles[count] = _screenRefreshEvent.handle();
		    if (WaitForMultipleObjects(count + 1, handles, FALSE, _screenResumePending ? 250 : INFINITE) == WAIT_FAILED)
		    {
			    WLog_Print(getWLog(), WLOG_ERROR, "WaitForMultipleObjects WAIT_FAILED");
			    return SdlSessionStep::LocalFailure;
		    }
		    if (stopped())
			    return SdlSessionStep::Stopped;
		    if (!freerdp_check_event_handles(context()))
		    {
			    WLog_Print(getWLog(), WLOG_DEBUG, "Reconnect checkpoint: event processing failed");
			    return SdlSessionStep::TransportFailure;
		    }
		    if (!serviceScreenRefresh()) {
			    WLog_Print(getWLog(), WLOG_ERROR, "Screen refresh could not resume graphics output; recovering transport");
			    freerdp_set_last_error_if_not(context(), FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
			    return SdlSessionStep::TransportFailure;
		    }
		    return SdlSessionStep::Continue;
	    },
	    [this, instance, &stopped]() {
		    if (stopped())
			    return false;
		    WLog_Print(getWLog(), WLOG_DEBUG, "Reconnect checkpoint: evaluating recovery policy");
		    setReconnecting(true);
		    const auto reconnect = client_auto_reconnect_ex(instance, [](freerdp* i) -> BOOL {
			    if (get_context(i->context)->_stopRequested)
				    return FALSE;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
			    if (auto bridge = SdlLauncher::active(); bridge && bridge->cancelled())
				    return FALSE;
#endif
			    return TRUE;
		    });
		    if (reconnect && !stopped())
		    {
			    setReconnecting(false);
			    getDialog().show(false);
		    }
		    return reconnect != FALSE;
	    });

	// Zero server error info only means no server reason was supplied; it does
	// not erase a failed local transport operation or an exhausted retry loop.
	if (result == SdlSessionResult::Failed && freerdp_error_info(instance) == ERRINFO_SUCCESS)
	{
		const auto last = freerdp_get_last_error(context());
		exit_code = last == FREERDP_ERROR_CONNECT_CANCELLED ? sdl::error::CONNECT_CANCELLED
		                                                  : sdl::error::errorToExitCode(last);
		error_msg = last == FREERDP_ERROR_SUCCESS ? "Connection event processing failed without server error information"
		                                         : freerdp_get_last_error_string(last);
	}

	if (exit_code == sdl::error::SUCCESS)
	{
		DWORD code = 0;
		{
			char* emsg = nullptr;
			size_t elen = 0;
			exit_code = error_info_to_error(&code, &emsg, &elen);
			if (emsg)
				error_msg = emsg;
			free(emsg);
		}

		if ((code == ERRINFO_LOGOFF_BY_USER) &&
		    (freerdp_get_disconnect_ultimatum(context()) == Disconnect_Ultimatum_user_requested))
		{
			const char* msg = "Error info says user did not initiate but disconnect ultimatum says "
			                  "they did; treat this as a user logoff";

			char* emsg = nullptr;
			size_t elen = 0;
			winpr_asprintf(&emsg, &elen, "%s", msg);
			if (emsg)
				error_msg = emsg;
			free(emsg);

			/* This situation might be limited to Windows XP. */
			WLog_Print(getWLog(), WLOG_INFO, "%s", msg);
			exit_code = sdl::error::LOGOFF;
		}
	}

	if ((exit_code != sdl::error::SUCCESS) && error_msg.empty())
	{
		const auto last = freerdp_get_last_error(context());
		char* msg = nullptr;
		size_t len = 0;
		winpr_asprintf(&msg, &len, "%s [0x%08" PRIx32 "]\n%s", freerdp_get_last_error_name(last),
		               last, freerdp_get_last_error_string(last));
		if (msg)
			error_msg = msg;
		free(msg);
	}

	freerdp_disconnect(instance);

	return exit_code;
}

/* RDP main loop.
 * Connects RDP, loops while running and handles event and dispatch, cleans up
 * after the connection ends. */
DWORD SdlContext::rdpThreadRun(SdlContext* sdl)
{
	WINPR_ASSERT(sdl);

	std::string error_msg;
	int exit_code = sdl->_stopRequested ? sdl::error::SUCCESS
	                                  : sdl->sdl_client_thread_connect(error_msg);
	if (exit_code == sdl::error::SUCCESS && !sdl->_stopRequested)
		exit_code = sdl->sdl_client_thread_run(error_msg);
	sdl->sdl_client_cleanup(exit_code, error_msg);

	return static_cast<DWORD>(exit_code);
}

int SdlContext::error_info_to_error(DWORD* pcode, char** msg, size_t* len) const
{
	const DWORD code = freerdp_error_info(context()->instance);
	const char* name = freerdp_get_error_info_name(code);
	const char* str = freerdp_get_error_info_string(code);
	const int exit_code = sdl::error::errorToExitCode(code);

	winpr_asprintf(msg, len, "Terminate with %s due to ERROR_INFO %s [0x%08" PRIx32 "]: %s",
	               sdl::error::errorToExitCodeTag(code), name, code, str);
	SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%s", *msg);
	if (pcode)
		*pcode = code;
	return exit_code;
}

void SdlContext::applyMonitorOffset(SDL_WindowID window, float& x, float& y) const
{
	if (!freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon))
		return;

	/* The window offsets normalize the negotiated monitor origin into the
	 * desktop surface's nonnegative coordinate space, also used by input. */
	auto w = getWindowForId(window);
	x -= static_cast<float>(w->offsetX());
	y -= static_cast<float>(w->offsetY());
}

static bool alignX(const SDL_Rect& a, const SDL_Rect& b)
{
	if (a.x + a.w == b.x)
		return true;
	if (b.x + b.w == a.x)
		return true;
	return false;
}

static bool alignY(const SDL_Rect& a, const SDL_Rect& b)
{
	if (a.y + a.h == b.y)
		return true;
	if (b.y + b.h == a.y)
		return true;
	return false;
}

std::vector<SDL_DisplayID>
SdlContext::updateDisplayOffsetsForNeighbours(SDL_DisplayID id,
                                              const std::vector<SDL_DisplayID>& ignore)
{
	auto first = _offsets.at(id);
	std::vector<SDL_DisplayID> neighbours;

	for (auto& entry : _offsets)
	{
		if (entry.first == id)
			continue;
		if (std::find(ignore.begin(), ignore.end(), entry.first) != ignore.end())
			continue;

		bool neighbor = false;
		const bool ax = alignX(entry.second.first, first.first);
		const bool ay = alignY(entry.second.first, first.first);

		/* Offset along the axis that does not touch, converted from logical to pixel
		 * units of the reference display. Without this, a monitor that is adjacent on
		 * one axis but shifted on the other (e.g. a wider monitor above another, or a
		 * rotated monitor that is not top aligned) keeps offset 0 on that axis, the
		 * layout sent to the server no longer matches the local one and pointer input
		 * lands offset by that difference. */
		auto crossOffset = [](int ePos, int fPos, int fLogical, int fPixel) -> int
		{
			if (fLogical <= 0)
				return ePos - fPos;
			return static_cast<int>(
			    std::lround(static_cast<double>(ePos - fPos) * fPixel / fLogical));
		};

		if (ax)
		{
			if (entry.second.first.x < first.first.x)
				entry.second.second.x = first.second.x - entry.second.second.w;
			else
				entry.second.second.x = first.second.x + first.second.w;
			if (!ay)
				entry.second.second.y =
				    first.second.y +
				    crossOffset(entry.second.first.y, first.first.y, first.first.h, first.second.h);
			neighbor = true;
		}
		if (ay)
		{
			if (entry.second.first.y < first.first.y)
				entry.second.second.y = first.second.y - entry.second.second.h;
			else
				entry.second.second.y = first.second.y + first.second.h;
			if (!ax)
				entry.second.second.x =
				    first.second.x +
				    crossOffset(entry.second.first.x, first.first.x, first.first.w, first.second.w);
			neighbor = true;
		}

		if (neighbor)
			neighbours.push_back(entry.first);
	}
	return neighbours;
}

void SdlContext::updateMonitorDataFromOffsets()
{
	for (auto& entry : _displays)
	{
		auto offsets = _offsets.at(entry.first);
		entry.second.x = offsets.second.x;
		entry.second.y = offsets.second.y;
	}

	for (auto& entry : _windows)
	{
		const auto id = entry.second.monitor(false).orig_screen;
		const auto monitor = _displays.find(id);
		if (monitor != _displays.end())
			entry.second.setMonitor(monitor->second);
	}
}

bool SdlContext::drawToWindow(SdlWindow& window, const std::vector<SDL_Rect>& rects, bool remoteContent)
{
	auto size = window.rect();
	std::unique_lock lock(_critical);
	if (!isConnected()) return true;
	auto gdi = context()->gdi;
	if (!gdi || !_primary) return true;
	auto surface = _primary.get();
	auto& metrics = window.renderMetrics();
	auto& live = window.liveMetrics();
	live.setMemoryEnabled(_performanceEnabled && remoteContent);
	uint64_t attemptedPixels = 0;
	if (metrics.enabled() && surface)
	{
		if (rects.empty())
			attemptedPixels = static_cast<uint64_t>(surface->w) * surface->h;
		else
		{
			for (const auto& rect : rects)
			{
				if ((rect.w > 0) && (rect.h > 0))
					attemptedPixels += static_cast<uint64_t>(rect.w) * rect.h;
			}
		}
	}
	metrics.beginFrame(attemptedPixels);
	live.beginFrame(attemptedPixels);
	struct EndMetricsFrame
	{
		SdlRenderMetrics& metrics;
		SdlRenderMetrics& live;
		~EndMetricsFrame() { metrics.endFrame(); live.endFrame();live.setMemoryEnabled(false); }
	} endMetricsFrame{ metrics,live };

	if (useLocalScale())
	{
		window.setOffsetX(0);
		window.setOffsetY(0);
		if (gdi->width < size.w)
		{
			window.setOffsetX((size.w - gdi->width) / 2);
		}
		if (gdi->height < size.h)
		{
			window.setOffsetY((size.h - gdi->height) / 2);
		}

		_localScale = { static_cast<float>(size.w) / static_cast<float>(gdi->width),
			            static_cast<float>(size.h) / static_cast<float>(gdi->height) };
		if (!window.drawScaledRects(surface, _localScale, rects))
			return false;
	}
	else
	{
		SDL_Point offset{ 0, 0 };
		if (freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon))
			offset = { window.offsetX(), window.offsetY() };
		if (!window.drawRects(surface, offset, rects))
			return false;
	}

	const auto settings = context()->settings;
	const bool showTopBar = showTopBarOn(window);
	const auto topBarPointer = topBarPointerForDraw();
	window.setRefreshCapabilities(freerdp_settings_get_bool(settings, FreeRDP_RefreshRect),
	                              freerdp_settings_get_bool(settings, FreeRDP_SuppressOutput));
	/* While the transport is down, any repaint (late queued update, connection
	 * bar hover, performance overlay) must keep the faded "Reconnecting..."
	 * frame; presenting the clean frame here made the overlay blink. */
	if (isReconnecting())
		return window.updateStalledSurface(stalledDots(), showTopBar, _fullscreenState.barPinned,
		                                   topBarPointer);
	return window.updateSurface(showTopBar, _fullscreenState.barPinned, topBarPointer);
}

bool SdlContext::showTopBarOn(const SdlWindow& window) const
{
	return _fullscreenState.barShown(
	    window.id(), freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon));
}

SDL_FPoint SdlContext::topBarPointerForDraw() const
{
	return _topBarHoveredButton != SdlTopBarButton::None ? _topBarPointer
	                                                     : SDL_FPoint{ -1.0f, -1.0f };
}

int SdlContext::stalledDots()
{
	return static_cast<int>((SDL_GetTicks() / 500) % 3) + 1;
}

bool SdlContext::redrawWindows()
{
	if (isReconnecting())
		return redrawStalled();
	if (!isConnected())
		return true;

	for (auto& entry : _windows)
	{
		if (!drawToWindow(entry.second))
			return false;
	}
	return true;
}

bool SdlContext::minimizeAllWindows()
{
	for (auto& w : _windows)
		w.second.minimize();
	return true;
}

int SdlContext::exitCode() const
{
	return _exitCode;
}

SDL_PixelFormat SdlContext::pixelFormat() const
{
	return _sdlPixelFormat;
}

bool SdlContext::addDisplayWindow(SDL_DisplayID id)
{
	if (!SDL_GetDisplayName(id))
		return true; // The added display may already have been removed.
	for (const auto& entry : _windows)
	{
		if (entry.second.monitor(false).orig_screen == id)
			return true;
	}
	const auto found = _displays.find(id);
	const auto monitor = found != _displays.end() ? found->second : SdlWindow::query(id);
	if (monitor.width <= 0 || monitor.height <= 0)
		return false;
	const auto flags =
	    SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
	auto title = sdl::utils::windowTitle(context()->settings);
	auto w = SdlWindow::create(id, title, flags);
	if (!w.window() || !w.renderer())
		return false;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active()) bridge->configureWindowGeometry(w.window());
#endif
	w.setMonitor(monitor);
	std::ignore = SdlHosted::show(w.window());
	_fullscreenState.addWindow(w.id());
	_windows.emplace(w.id(), std::move(w));
	return true;
}

bool SdlContext::removeDisplayWindow(SDL_DisplayID id)
{
	const bool hadWindows = !_windows.empty();
	for (auto it = _windows.begin(); it != _windows.end();)
	{
		// macOS may already have moved the window to another display. Its
		// negotiated monitor still identifies the display that owned it.
		if (it->second.monitor(false).orig_screen == id)
		{
			_fullscreenState.removeWindow(it->first);
			it = _windows.erase(it);
		}
		else
			++it;
	}
	if (hadWindows && _windows.empty())
	{
		// Keep the session reachable when all explicitly selected screens
		// disappear, using the remaining primary display (e.g. the laptop).
		const auto primary = SDL_GetPrimaryDisplay();
		if (primary != 0 && !addDisplayWindow(primary))
			return false;
	}
	if (!_windows.empty())
		reassignTopBar(primaryWindowId());
	return true;
}

bool SdlContext::detectDisplays()
{
	int count = 0;
	auto ids = SDL_GetDisplays(&count);
	if (!ids || count <= 0)
	{
		SDL_free(ids);
		return false;
	}
	std::map<SDL_DisplayID, rdpMonitor> current;
	std::map<SDL_DisplayID, int> insets;
	for (int i = 0; i < count; i++)
	{
		const auto monitor = SdlWindow::query(ids[i], false);
		if (monitor.width <= 0 || monitor.height <= 0)
		{
			SDL_free(ids);
			return false;
		}
		current.emplace(ids[i], monitor);
		insets.emplace(ids[i], sdl_fullscreen_top_inset(ids[i]));
	}
	SDL_free(ids);
	// Replace, rather than emplace into, the startup cache. Replugging can
	// change both SDL IDs and backing dimensions; removed displays must vanish.
	_displays = std::move(current);
	_fullscreenTopInsets = std::move(insets);
	updateDisplayOffsets();
	return true;
}

bool SdlContext::setHotplugMonitorSelection(const std::vector<SDL_DisplayID>& ids,
                                            const SdlMonitorScaleOverrides& scales)
{
	if (ids.empty())
		return false;
	if (!freerdp_settings_set_pointer_len(context()->settings, FreeRDP_MonitorIds, ids.data(),
	                                      ids.size()))
		return false;
	setMonitorIds(ids);
	_monitorScaleOverrides = scales;
	_monitorScaleOverridesConfigured = true;
	return true;
}

bool SdlContext::refreshDisplayWindows()
{
	if (!detectDisplays())
		return false;
	if (_windows.empty())
		return true; // Startup still owns initial window creation.
	auto settings = context()->settings;
	if (!freerdp_settings_get_bool(settings, FreeRDP_UseMultimon))
	{
		for (auto& entry : _windows)
		{
			const auto id = SDL_GetDisplayForWindow(entry.second.window());
			if (_displays.count(id) == 0)
				continue;
			entry.second.setMonitor(_displays.at(id));
			if (!updateWindow(entry.first))
				return false;
		}
		return updateWindowList();
	}
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active(); bridge && bridge->hasDisplaySelection())
		if (!bridge->refreshDisplaySelection(*this))
			return false;
#endif
	auto desired = getDisplayIds();
	if (freerdp_settings_get_uint32(settings, FreeRDP_NumMonitorIds) > 0)
	{
		desired.clear();
		for (const auto id : _monitorIds)
			if (_displays.count(id))
				desired.push_back(id);
	}
	if (desired.empty())
	{
		const auto primary = SDL_GetPrimaryDisplay();
		if (_displays.count(primary))
			desired.push_back(primary);
	}
	if (desired.empty())
		return false;
	for (auto it = _windows.begin(); it != _windows.end();)
	{
		const auto id = it->second.monitor(false).orig_screen;
		if (std::find(desired.begin(), desired.end(), id) == desired.end())
		{
			_fullscreenState.removeWindow(it->first);
			it = _windows.erase(it);
		}
		else
		{
			it->second.setMonitor(_displays.at(id));
			if (SDL_GetDisplayForWindow(it->second.window()) != id &&
			    !applyFullscreen(it->first, true, true))
				return false;
			++it;
		}
	}
	/* A remaining window takes the bar before new displays are added, so a
	 * window that only just appeared never takes it from one that stayed. */
	const bool kept = !_windows.empty();
	if (kept)
		reassignTopBar(primaryWindowId());
	for (const auto id : desired)
		if (!addDisplayWindow(id))
			return false;
	/* Every window is new: the first one created took the bar; give it to
	 * the primary's window instead. */
	if (!kept && !_windows.empty())
		reassignTopBar(primaryWindowId(), true);
	// A fallback window may be created after its size event has already fired.
	// Refresh drawable geometry before sending this topology's monitor layout;
	// subsequent pixel-size events and settling retries refine it as needed.
	for (const auto& entry : _windows)
		if (!updateWindow(entry.first))
			return false;
	return updateWindowList();
}

rdpMonitor SdlContext::getDisplay(SDL_DisplayID id) const
{
	return _displays.at(id);
}

int SdlContext::fullscreenTopInset(SDL_DisplayID id) const
{
	const auto it = _fullscreenTopInsets.find(id);
	return (it != _fullscreenTopInsets.end()) ? it->second : 0;
}

void SdlContext::setFullscreenTopInset(SDL_DisplayID id, int pixels)
{
	_fullscreenTopInsets[id] = pixels;
}

std::pair<UINT32, UINT32> SdlContext::sessionCoreScale()
{
	if (!_sessionCoreScale)
		_sessionCoreScale = std::make_pair(
		    freerdp_settings_get_uint32(context()->settings, FreeRDP_DesktopScaleFactor),
		    freerdp_settings_get_uint32(context()->settings, FreeRDP_DeviceScaleFactor));
	return *_sessionCoreScale;
}

void SdlContext::resetSessionCoreScale()
{
	_sessionCoreScale.reset();
}

std::vector<SDL_DisplayID> SdlContext::getDisplayIds() const
{
	std::vector<SDL_DisplayID> keys;
	keys.reserve(_displays.size());
	for (const auto& entry : _displays)
	{
		keys.push_back(entry.first);
	}
	return keys;
}

const SdlWindow* SdlContext::getWindowForId(SDL_WindowID id) const
{
	auto it = _windows.find(id);
	if (it == _windows.end())
		return nullptr;
	return &it->second;
}

SdlWindow* SdlContext::getWindowForId(SDL_WindowID id)
{
	auto it = _windows.find(id);
	if (it == _windows.end())
		return nullptr;
	return &it->second;
}

bool SdlContext::allWindowsHidden() const
{
	return std::all_of(_windows.begin(), _windows.end(),
	                   [](const auto& entry)
	                   {
		                   const auto flags = SDL_GetWindowFlags(entry.second.window());
		                   return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0;
	                   });
}

SdlWindow* SdlContext::getFirstWindow()
{
	if (_windows.empty())
		return nullptr;
	return &_windows.begin()->second;
}

sdlDispContext& SdlContext::getDisplayChannelContext()
{
	return _disp;
}

sdlInput& SdlContext::getInputChannelContext()
{
	return _input;
}

sdlClip& SdlContext::getClipboardChannelContext()
{
	return _clip;
}

SdlRail& SdlContext::getRailChannelContext()
{
	return _rail;
}

SdlConnectionDialogWrapper& SdlContext::getDialog()
{
	return _dialog;
}

wLog* SdlContext::getWLog()
{
	return _log;
}

bool SdlContext::moveMouseTo(const SDL_FPoint& pos)
{
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (SdlHosted::enabled())
	{
		/* The launcher owns the pointer. Report the place in window pixels, the space its
		 * input messages use. */
		auto first = getFirstWindow();
		auto bridge = SdlLauncher::active();
		if (!first || !first->renderer() || !bridge)
			return true;
		const auto spos = pixelToScreen(first->id(), pos);
		float x = 0, y = 0;
		if (SDL_RenderCoordinatesFromWindow(first->renderer(), spos.x, spos.y, &x, &y))
			bridge->hostedWarp(first->id(), x, y);
		return true;
	}
#endif
	auto window = SDL_GetMouseFocus();
	if (!window)
		return true;

	const auto id = SDL_GetWindowID(window);
	const auto spos = pixelToScreen(id, pos);
	WLog_Print(_log, WLOG_DEBUG, "warp mouse to %.0f,%.0f on window %u", spos.x, spos.y,
	           (unsigned)id);
	SDL_WarpMouseInWindow(window, spos.x, spos.y);
	return true;
}

static bool onWindowEdge(SDL_WindowID id, float x, float y)
{
	int w = 0;
	int h = 0;
	auto* window = SDL_GetWindowFromID(id);
	if (!window || !SDL_GetWindowSize(window, &w, &h))
		return false;
	return (x <= 0.0f) || (y <= 0.0f) || (x >= static_cast<float>(w - 1)) ||
	       (y >= static_cast<float>(h - 1));
}

bool SdlContext::handleEvent(const SDL_MouseMotionEvent& ev)
{
	if(performanceMotion(ev)) return true;
	if (handleTopBarMotion(ev))
		return true;

	SDL_Event copy{};
	copy.motion = ev;
	/* SDL follows a release outside the window with a motion clamped to its edge: drop it. */
	if (_rail.enabled() && (ev.state == 0) && (ev.windowID == _lastRelease.window) &&
	    (ev.timestamp - _lastRelease.time < SDL_MS_TO_NS(2)) &&
	    onWindowEdge(ev.windowID, ev.x, ev.y))
		return true;
	/* WM owns the drag (#12447); backstop: button released but button-up swallowed by grab. */
	if (_rail.enabled() && _rail.suppressServerMotion(ev.windowID))
	{
		if (!(SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK))
			_rail.completeLocalMoveIfPending();
		return true;
	}
	if (_rail.enabled() && _rail.translateToServer(ev.windowID, copy.motion.x, copy.motion.y))
	{
		_lastMotion = { ev.windowID, { ev.x, ev.y }, { copy.motion.x, copy.motion.y } };
		_rail.noteServerPointer(copy.motion.x, copy.motion.y);
		return SdlTouch::handleEvent(this, copy.motion);
	}

	if (!getWindowForId(ev.windowID))
		return true; /* Event for an untracked window (e.g. closed dialog) */
	if (!eventToPixelCoordinates(ev.windowID, copy))
		return true;
	/* Relative deltas retain the source renderer's scale. Absolute positions
	 * can belong to a different monitor while SDL captures a held drag. */
	removeLocalScaling(copy.motion.xrel, copy.motion.yrel);
	SDL_FPoint pos{};
	if (!screenToRdp(ev.windowID, { ev.x, ev.y }, pos))
		return true;
	copy.motion.x = pos.x;
	copy.motion.y = pos.y;

	return SdlTouch::handleEvent(this, copy.motion);
}

bool SdlContext::handleTopBarMotion(const SDL_MouseMotionEvent& ev)
{
	/* Active move/resize gesture takes precedence over hover handling below:
	 * it must run even though the gesture owner is Local (which would
	 * otherwise return early), or drags never move. */
	if (_topBarDrag != TopBarDragMode::None)
	{
		auto window = getWindowForId(_topBarDragWindow);
		if (!window)
		{
			_topBarDrag = TopBarDragMode::None;
			_topBarDragWindow = 0;
			std::ignore = SDL_CaptureMouse(false);
			return true;
		}
		const auto cur = screenToPixel(_topBarDragWindow, { ev.x, ev.y });
		auto rect = _topBarDragRect;
		if (_topBarDrag == TopBarDragMode::Move)
		{
			/* Glued to the top like mstsc: horizontal dragging only. */
			rect.x += cur.x - _topBarDragGrab.x;
		}
		else
		{
			if (_topBarDragLeftEdge)
			{
				const float dx = cur.x - _topBarDragGrab.x;
				rect.x += dx;
				rect.w -= dx;
			}
			else
			{
				rect.w += cur.x - _topBarDragGrab.x;
			}
		}
		rect = SdlTopBar::clampToViewport(rect, window->pixelViewport(),
		                                  window->topBarCompact());
		window->setTopBarRect(rect);
		_topBarPointer = cur;
		if (!redrawWindows())
			WLog_Print(_log, WLOG_WARN, "Unable to redraw the SDL connection bar");
		return true;
	}

	/* A local topbar gesture owns motion until its matching button release. A
	 * remote gesture must continue to receive motion even when the pointer
	 * crosses the overlay. */
	if (_topBarGestureOwner == TopBarGestureOwner::Local)
		return true;
	if (_topBarGestureOwner == TopBarGestureOwner::Remote)
		return false;

	const auto settings = context()->settings;
	auto window = getWindowForId(ev.windowID);
	if (!_fullscreenState.barOwner || !topBarAllowed() || !window ||
	    ev.windowID != _fullscreenState.barOwner)
	{
		/* The bar went away or the pointer is in another window: hand the
		 * cursor back to the server. */
		if (!setTopBarPointerOver(false))
			WLog_Print(_log, WLOG_WARN, "Unable to update the cursor over the SDL connection bar");
		return false;
	}

	_topBarPointer = screenToPixel(ev.windowID, { ev.x, ev.y });
	bool panelChanged =
	    _fullscreenState.barVisible && window->updateTopBarHover(ev.x, ev.y, SDL_GetTicks());
	const bool overBar = window->topBarContains(ev.x, ev.y);
	const bool nearTop = window->topBarNearTop(ev.x, ev.y);
	const bool wasVisible = _fullscreenState.barVisible;

	if (!_fullscreenState.barPinned)
	{
		if (nearTop || window->refreshMenuOpen())
			_fullscreenState.barVisible = true;
		else if (!overBar)
			_fullscreenState.barVisible = false;
	}

	if (_fullscreenState.barVisible && !wasVisible)
		panelChanged = window->updateTopBarHover(ev.x, ev.y, SDL_GetTicks()) || panelChanged;
	const auto hoveredButton = _fullscreenState.barVisible && overBar
	                               ? window->topBarButtonAt(ev.x, ev.y)
	                               : SdlTopBarButton::None;
	const bool hoverChanged = hoveredButton != _topBarHoveredButton;
	_topBarHoveredButton = hoveredButton;

	if ((_fullscreenState.barVisible != wasVisible || hoverChanged || panelChanged) &&
	    !redrawWindows())
		WLog_Print(_log, WLOG_WARN, "Unable to redraw the SDL connection bar");

	const bool pointerOver = _fullscreenState.barVisible && overBar;
	if (!setTopBarPointerOver(pointerOver))
		WLog_Print(_log, WLOG_WARN, "Unable to update the cursor over the SDL connection bar");
	return pointerOver;
}

bool SdlContext::handleEvent(const SDL_MouseWheelEvent& ev)
{
	if (auto window = getWindowForId(ev.windowID); window && refreshMenuOpen() &&
	    window->topBarContains(ev.mouse_x, ev.mouse_y))
		return true;
	SDL_Event copy{};
	copy.wheel = ev;
	if (_rail.enabled() &&
	    _rail.translateToServer(ev.windowID, copy.wheel.mouse_x, copy.wheel.mouse_y))
		return SdlTouch::handleEvent(this, copy.wheel);

	if (!getWindowForId(ev.windowID))
		return true;
	if (!eventToPixelCoordinates(ev.windowID, copy))
		return true;
	removeLocalScaling(copy.wheel.mouse_x, copy.wheel.mouse_y);
	return SdlTouch::handleEvent(this, copy.wheel);
}

bool SdlContext::handleEvent(const SDL_WindowEvent& ev)
{
	if (!getDisplayChannelContext().handleEvent(ev))
		return false;

	auto window = getWindowForId(ev.windowID);
	if (!window)
	{
		/* RAIL windows aren't in _windows; handle their events here. */
		if (_rail.enabled() && _rail.ownsWindow(ev.windowID))
		{
			switch (ev.type)
			{
				case SDL_EVENT_WINDOW_MOUSE_ENTER:
					/* Re-enter fires on move-grab end. */
					if (!(SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK))
						_rail.completeLocalMoveIfPending(); /* X11 */
					_rail.completeWaylandResize();          /* Wayland: compositor grab ended */
					/* Restore the cursor or the pointer stays hidden over RemoteApp windows. */
					return restoreCursor();
				case SDL_EVENT_WINDOW_MOUSE_LEAVE:
					/* Compositor took the pointer for the pending Wayland resize grab. */
					_rail.noteResizeGrab(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_EXPOSED:
					/* Force repaint for exposed windows; skip during local drag to avoid redundant
					 * frames. */
					if (!_rail.dragActive(ev.windowID))
						_rail.invalidateWindow(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_MAXIMIZED:
					/* Maximize must complete move first. */
					_rail.completeLocalMoveIfPending();
					_rail.handleMaximized(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_MINIMIZED:
					_rail.handleMinimized(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_RESTORED:
					_rail.handleRestored(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					/* Send SC_CLOSE, don't abort the session. */
					_rail.handleClose(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
					_rail.handleFocus(ev.windowID, true);
					return true;
				case SDL_EVENT_WINDOW_FOCUS_LOST:
					_rail.handleFocus(ev.windowID, false);
					return true;
				case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
					_rail.handleDisplayChanged(ev.windowID, static_cast<SDL_DisplayID>(ev.data1));
					return true;
				case SDL_EVENT_WINDOW_MOVED:
					_rail.notePopupMoved(ev.windowID);
					if (!_rail.dragActive(ev.windowID))
						_rail.syncGeometry(ev.windowID);
					return true;
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					/* No server frame during the drag: repaint synchronously each configure step;
					 * also report the new size to catch a compositor snap/tile. */
					_rail.noteDragResize(ev.windowID, ev.data1, ev.data2);
					_rail.syncGeometry(ev.windowID);
					_rail.noteResizeGrab(ev.windowID);
					std::ignore = drawToWindows({});
					return true;
				default:
					break;
			}
		}
		return true;
	}

#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active()) bridge->observeWindowGeometry(window->window(), ev.type);
#endif

	if (ev.type == SDL_EVENT_WINDOW_HIDDEN || ev.type == SDL_EVENT_WINDOW_MINIMIZED ||
	    ev.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
		window->closeRefreshMenu();
	{
		const auto& r = window->rect();
		const auto& b = window->bounds();
		const auto& scale = window->scale();
		const auto& orientation = window->orientation();
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
		             "%s: [%u] %dx%d-%dx%d {%dx%d-%dx%d}{scale=%f,orientation=%s}",
		             sdl::utils::toString(ev.type).c_str(), ev.windowID, r.x, r.y, r.w, r.h, b.x,
		             b.y, b.w, b.h, static_cast<double>(scale),
		             sdl::utils::toString(orientation).c_str());
	}

	switch (ev.type)
	{
		case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
		case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
			if (!observeFullscreen(ev.windowID, ev.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN))
				return false;
			break;
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
			return restoreCursor();
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			window->closeRefreshMenu();
			std::ignore = redrawWindows();
			break;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			window->closeRefreshMenu();
			if (ev.windowID == _fullscreenState.barOwner &&
			    _topBarGestureOwner == TopBarGestureOwner::None)
			{
				const bool wasVisible = _fullscreenState.barVisible;
				if (!_fullscreenState.barPinned)
					_fullscreenState.barVisible = false;
				_topBarPointer = { -1.0f, -1.0f };
				const bool hoverChanged = _topBarHoveredButton != SdlTopBarButton::None;
				_topBarHoveredButton = SdlTopBarButton::None;
				if (!setTopBarPointerOver(false))
					return false;
				if ((_fullscreenState.barVisible != wasVisible || hoverChanged) && !redrawWindows())
					return false;
			}
			break;
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			if (!resizeToScale(window))
				return false;
			if (isConnected())
			{
				if (!window->fill())
					return false;
				if (!drawToWindow(*window))
					return false;
				if (!restoreCursor())
					return false;
			}
			break;
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			if (!resizeToScale(window))
				return false;
			if (!window->fill())
				return false;
			if (!drawToWindow(*window))
				return false;
			if (!restoreCursor())
				return false;
			break;
		case SDL_EVENT_WINDOW_MOVED:
		{
			auto r = window->rect();
			auto id = window->id();
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%u: %dx%d-%dx%d", id, r.x, r.y, r.w, r.h);
		}
		break;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		{
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Window closed, terminating RDP session...");
#ifdef WITH_SDL_LAUNCHER_BRIDGE
			if (auto bridge = SdlLauncher::active())
			{
				std::ignore = bridge->requestClose();
				break;
			}
#endif
			freerdp_abort_connect_context(context());
		}
		break;
		default:
			break;
	}
	return true;
}

bool SdlContext::handleEvent(const SDL_DisplayEvent& ev)
{
	// SDL's inventory is updated before queued events are consumed. Bounds,
	// scale and mode events for a removed display are therefore expected.
	if (ev.type != SDL_EVENT_DISPLAY_REMOVED && !SDL_GetDisplayName(ev.displayID))
	{
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Ignoring stale display event 0x%x for %u",
		             ev.type, ev.displayID);
		return true;
	}
	if (!getDisplayChannelContext().handleEvent(ev))
		return false;

	switch (ev.type)
	{
		case SDL_EVENT_DISPLAY_REMOVED: // Can't show details for this one...
			break;
		default:
		{
			// Details are diagnostic only; hotplug can invalidate them mid-query.
			SDL_Rect r = {};
			if (!SDL_GetDisplayBounds(ev.displayID, &r))
				return true;
			const auto name = SDL_GetDisplayName(ev.displayID);
			if (!name)
				return true;
			const auto orientation = SDL_GetCurrentDisplayOrientation(ev.displayID);
			const auto scale = SDL_GetDisplayContentScale(ev.displayID);
			const auto mode = SDL_GetCurrentDisplayMode(ev.displayID);
			if (!mode)
				return true;

			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			             "%s: [%u, %s] %dx%d-%dx%d {orientation=%s, scale=%f}%s",
			             sdl::utils::toString(ev.type).c_str(), ev.displayID, name, r.x, r.y, r.w,
			             r.h, sdl::utils::toString(orientation).c_str(), static_cast<double>(scale),
			             sdl::utils::toString(mode).c_str());
		}
		break;
	}
	return true;
}

bool SdlContext::handleEvent(const SDL_MouseButtonEvent& ev)
{
	auto window = getWindowForId(ev.windowID);
	const bool overBar = window && _fullscreenState.barVisible &&
	                     ev.windowID == _fullscreenState.barOwner && topBarAllowed() &&
	                     window->topBarContains(ev.x, ev.y);
	if ((_topBarGestureOwner == TopBarGestureOwner::Local ||
	     (overBar && _topBarGestureOwner == TopBarGestureOwner::None && !_performanceCapture && !_performanceOrphanCapture)) &&
	    handleTopBarButton(ev)) return true;
	if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && window && !overBar && window->refreshMenuOpen()) {
		window->closeRefreshMenu(); _topBarHoveredButton = SdlTopBarButton::None;
		if (!_fullscreenState.barPinned) _fullscreenState.barVisible = false;
		std::ignore = redrawWindows();
	}
	if(performanceButton(ev)) return true;
	const auto* capture = ev.type == SDL_EVENT_MOUSE_BUTTON_UP ? topBarCapture(ev.button) : nullptr;
	const auto capturedWindowId = capture && capture->active ? capture->windowId : 0;

	if (handleTopBarButton(ev))
		return true;

	SDL_Event copy = {};
	copy.button = ev;
	if (_rail.enabled())
	{
		/* Suppress X11 raw button during WM op. */
		const bool suppress = _rail.dragActive(ev.windowID);
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP)
			_rail.completeLocalMoveIfPending();
		/* Backstop for a Wayland resize whose MOUSE_ENTER completion never arrived; a fresh press
		 * proves the old grab is over, so a no-op click grab is cancelled here too. */
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			_rail.completeWaylandResize(true);
		/* activate the clicked window before forwarding, so the click routes to it. */
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			_rail.ensureActive(ev.windowID);
		if (suppress)
			return true;
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP)
		{
			_lastRelease = { ev.windowID, ev.timestamp };
		}
		if (_rail.translateToServer(ev.windowID, copy.button.x, copy.button.y))
		{
			/* The window may have moved since the last motion: keep the server's position. */
			if ((ev.windowID == _lastMotion.window) &&
			    (fabsf(ev.x - _lastMotion.local.x) < FLT_EPSILON) &&
			    (fabsf(ev.y - _lastMotion.local.y) < FLT_EPSILON))
			{
				copy.button.x = _lastMotion.server.x;
				copy.button.y = _lastMotion.server.y;
			}
			/* Record the forwarded left press: it is the anchor of any server modal move/size
			 * loop this press starts (the loop moves the window by release - anchor). */
			if ((ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) && (ev.button == SDL_BUTTON_LEFT))
				_rail.noteLeftPress(copy.button.x, copy.button.y);
			return SdlTouch::handleEvent(this, copy.button);
		}
	}

	/* SDL can report a release without a tracked window after the pointer has
	 * left the client. Route it through the window that received the press so
	 * the remote button-up is not lost. */
	auto windowId = ev.windowID;
	if (!getWindowForId(windowId) && capturedWindowId)
		windowId = capturedWindowId;

	if (!getWindowForId(windowId))
		return true;
	SDL_FPoint pos{};
	if (!screenToRdp(windowId, { ev.x, ev.y }, pos))
		return true;
	auto button = ev;
	button.windowID = windowId;
	button.x = pos.x;
	button.y = pos.y;
	return SdlTouch::handleEvent(this, button);
}

bool SdlContext::handleTopBarButton(const SDL_MouseButtonEvent& ev)
{
	auto* capture = topBarCapture(ev.button);
	if (ev.button == SDL_BUTTON_LEFT && _topBarDrag != TopBarDragMode::None)
	{
		_topBarDrag = TopBarDragMode::None;
		_topBarDragWindow = 0;
		std::ignore = SDL_CaptureMouse(false);
	}
	if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && capture && capture->active)
	{
		const auto gesture = *capture;
		*capture = {};
		if (!hasTopBarCapture(gesture.local))
			_topBarGestureOwner = TopBarGestureOwner::None;

		/* Remote presses are deliberately left for the normal RDP input path. */
		if (!gesture.local)
			return false;

		/* A local press owns its release, even if the pointer is now outside the
		 * bar or the window state changed in the meantime. */
		if (gesture.windowId != ev.windowID || gesture.button == SdlTopBarButton::None) {
			if (auto source = getWindowForId(gesture.windowId); source && source->refreshMenuOpen()) {
				source->closeRefreshMenu(); std::ignore = redrawWindows();
			}
			return true;
		}

		const auto settings = context()->settings;
		if (!_fullscreenState.barVisible || !topBarAllowed())
			return true;

		auto window = getWindowForId(gesture.windowId);
		if (!window) return true;
		_topBarPointer = screenToPixel(ev.windowID, { ev.x, ev.y });
		std::ignore = window->updateTopBarHover(ev.x, ev.y, SDL_GetTicks());
		_topBarHoveredButton = window->topBarButtonAt(ev.x, ev.y);
		if (!window->topBarContains(ev.x, ev.y) || _topBarHoveredButton != gesture.button) {
			std::ignore = redrawWindows(); return true;
		}

		switch (gesture.button)
		{
            case SdlTopBarButton::Performance:
#ifdef WITH_SDL_LAUNCHER_BRIDGE
                if(auto bridge=SdlLauncher::active()) bridge->requestPerformanceOverlay((bridge->performanceMask()&2)==0);
#endif
                break;
			case SdlTopBarButton::Refresh:
			case SdlTopBarButton::RefreshLocal:
			case SdlTopBarButton::RefreshTextures:
			case SdlTopBarButton::RefreshServer:
			case SdlTopBarButton::RefreshUpdates:
			case SdlTopBarButton::RefreshCombined:
			{
				const auto button = gesture.button == SdlTopBarButton::Refresh
				                        ? window->selectedRefresh() : gesture.button;
				SdlRefreshMode mode = SdlRefreshMode::Server;
				if ((gesture.button == SdlTopBarButton::Refresh || window->refreshEnabled(button)) &&
				    SdlTopBar::refreshMode(button, mode))
				{
					window->noteRefresh(button);
					requestScreenRefresh(mode);
				}
				break;
			}
			case SdlTopBarButton::Compact:
			{
				/* Test toggle between the normal (3/4) and compact (1/2)
				 * bar heights. */
				window->setTopBarCompact(!window->topBarCompact());
				auto rect = window->topBarRect();
				rect.h = SdlTopBar::fixedHeight(window->pixelViewport(),
				                                window->topBarCompact());
				rect = SdlTopBar::clampToViewport(rect, window->pixelViewport(),
				                                  window->topBarCompact());
				window->setTopBarRect(rect);
				if (!redrawWindows())
					return false;
				break;
			}
			case SdlTopBarButton::Pin:
				_fullscreenState.barPinned = !_fullscreenState.barPinned;
				_fullscreenState.barVisible = true;
				if (!redrawWindows())
					return false;
				break;
			case SdlTopBarButton::Minimize:
				return setMinimized();
			case SdlTopBarButton::Restore:
				return toggleFullscreen(ev.windowID);
			case SdlTopBarButton::Close:
#ifdef WITH_SDL_LAUNCHER_BRIDGE
				if (auto bridge = SdlLauncher::active())
				{
					std::ignore = bridge->requestClose();
					return true;
				}
#endif
				return freerdp_abort_connect_context(context()) != FALSE;
			case SdlTopBarButton::None:
				break;
		}
		return true;
	}

	if (ev.type != SDL_EVENT_MOUSE_BUTTON_DOWN)
		return false;

	/* Do not hit-test the bar's geometry unless it is actually visible. */
	const auto settings = context()->settings;
	auto window = getWindowForId(ev.windowID);
	if (!window)
		return false;

	/* Keep gesture ownership bounded to buttons that have a capture slot. An
	 * unsupported button must not create an owner that can never be cleared by
	 * a matching release. */
	auto* buttonCapture = topBarCapture(ev.button);
	if (!buttonCapture)
		return false;

	const bool overVisibleBar =
	    _fullscreenState.barOwner && ev.windowID == _fullscreenState.barOwner &&
	    _fullscreenState.barVisible && topBarAllowed() && window->topBarContains(ev.x, ev.y);

	if (_topBarGestureOwner == TopBarGestureOwner::None)
		_topBarGestureOwner = overVisibleBar ? TopBarGestureOwner::Local
		                                    : TopBarGestureOwner::Remote;

	if (_topBarGestureOwner == TopBarGestureOwner::Local)
	{
		buttonCapture->active = true;
		buttonCapture->local = true;
		buttonCapture->windowId = ev.windowID;
		buttonCapture->button = overVisibleBar && ev.button == SDL_BUTTON_LEFT
		                             ? window->topBarButtonAt(ev.x, ev.y)
		                             : SdlTopBarButton::None;
		if (overVisibleBar)
		{
			_topBarPointer = screenToPixel(ev.windowID, { ev.x, ev.y });
			_topBarHoveredButton = window->topBarButtonAt(ev.x, ev.y);
		}
		/* mstsc-style move/resize gestures: left press on the title area
		 * drags the bar, press on an edge grip resizes its width. */
		if (overVisibleBar && ev.button == SDL_BUTTON_LEFT)
		{
			const auto grab = screenToPixel(ev.windowID, { ev.x, ev.y });
			const bool isResize = window->topBarResizeAt(ev.x, ev.y);
			const bool isMove = window->topBarMoveAt(ev.x, ev.y);
			WLog_Print(_log, WLOG_DEBUG,
			           "topbar press win=%u resize=%d move=%d at %.0f,%.0f",
			           (unsigned)ev.windowID, (int)isResize, (int)isMove, grab.x, grab.y);
			if (isResize)
			{
				_topBarDrag = TopBarDragMode::Resize;
				_topBarDragWindow = ev.windowID;
				_topBarDragGrab = grab;
				_topBarDragRect = window->topBarRect();
				_topBarDragLeftEdge =
				    grab.x < _topBarDragRect.x + _topBarDragRect.w / 2.0f;
				std::ignore = SDL_CaptureMouse(true);
			}
			else if (isMove)
			{
				_topBarDrag = TopBarDragMode::Move;
				_topBarDragWindow = ev.windowID;
				_topBarDragGrab = grab;
				_topBarDragRect = window->topBarRect();
				std::ignore = SDL_CaptureMouse(true);
			}
		}
		return true;
	}

	/* Record remote ownership for every tracked press, including presses just
	 * below a hidden bar and additional buttons pressed over the visible bar.
	 * Once a remote gesture starts, the overlay cannot claim another button. */
	buttonCapture->active = true;
	buttonCapture->local = false;
	buttonCapture->windowId = ev.windowID;
	buttonCapture->button = SdlTopBarButton::None;
	return false;
}

SdlContext::TopBarPointerCapture* SdlContext::topBarCapture(Uint8 button)
{
	if (button >= _topBarCaptures.size())
		return nullptr;
	return &_topBarCaptures[button];
}

const SdlContext::TopBarPointerCapture* SdlContext::topBarCapture(Uint8 button) const
{
	if (button >= _topBarCaptures.size())
		return nullptr;
	return &_topBarCaptures[button];
}

bool SdlContext::hasTopBarCapture(bool local) const
{
	return std::any_of(_topBarCaptures.cbegin(), _topBarCaptures.cend(),
	                   [local](const auto& capture) {
		                   return capture.active && capture.local == local;
	                   });
}

bool SdlContext::handleEvent(const SDL_TouchFingerEvent& ev)
{
	if (!getWindowForId(ev.windowID))
		return true;
	SDL_Event copy{};
	copy.tfinger = ev;
	if (!eventToPixelCoordinates(ev.windowID, copy))
		return true;
	removeLocalScaling(copy.tfinger.dx, copy.tfinger.dy);
	removeLocalScaling(copy.tfinger.x, copy.tfinger.y);
	applyMonitorOffset(copy.tfinger.windowID, copy.tfinger.x, copy.tfinger.y);
	return SdlTouch::handleEvent(this, copy.tfinger);
}

void SdlContext::updateDisplayOffsets()
{
	/* Update actual display rectangles:
	 *
	 * 1. Get logical display bounds
	 * 2. Use already known pixel width and height
	 * 3. Iterate over each display and update the x and y offsets by adding all monitor
	 * widths/heights from the primary
	 */
	_offsets.clear();
	for (auto& entry : _displays)
	{
		SDL_Rect bounds{};
		std::ignore = SDL_GetDisplayBounds(entry.first, &bounds);

		SDL_Rect pixel{};
		pixel.w = entry.second.width;
		pixel.h = entry.second.height;
		_offsets.emplace(entry.first, std::pair{ bounds, pixel });
	}

	/* 1. Find primary and update all neighbors
	 * 2. For each neighbor update all neighbors
	 * 3. repeat until all displays updated.
	 */
	if (_offsets.empty())
		return;
	auto primary = SDL_GetPrimaryDisplay();
	if (_offsets.count(primary) == 0)
		primary = _offsets.begin()->first;
	/* Align display offsets to the primary display origin. */
	auto it = _offsets.find(primary);
	it->second.second.x = it->second.first.x;
	it->second.second.y = it->second.first.y;
	std::vector<SDL_DisplayID> handled;
	handled.push_back(primary);

	auto neighbors = updateDisplayOffsetsForNeighbours(primary);
	while (!neighbors.empty())
	{
		auto neighbor = neighbors.front();
		neighbors.erase(neighbors.begin());

		if (std::find(handled.begin(), handled.end(), neighbor) != handled.end())
			continue;
		handled.push_back(neighbor);

		auto next = updateDisplayOffsetsForNeighbours(neighbor, handled);
		neighbors.insert(neighbors.end(), next.begin(), next.end());
	}
	updateMonitorDataFromOffsets();
}

bool SdlContext::eventToPixelCoordinates(SDL_WindowID id, SDL_Event& ev)
{
	auto w = getWindowForId(id);
	if (!w)
		return false;

	/* Ignore errors here, sometimes SDL has no renderer */
	auto renderer = w->renderer();
	if (!renderer)
		return true;
	return SDL_ConvertEventToRenderCoordinates(renderer, &ev);
}

SDL_FPoint SdlContext::applyLocalScaling(const SDL_FPoint& val) const
{
	if (!useLocalScale())
		return val;

	auto rval = val;
	rval.x *= _localScale.x;
	rval.y *= _localScale.y;
	return rval;
}

void SdlContext::removeLocalScaling(float& x, float& y) const
{
	if (!useLocalScale())
		return;
	x /= _localScale.x;
	y /= _localScale.y;
}

SDL_FPoint SdlContext::screenToPixel(SDL_WindowID id, const SDL_FPoint& pos)
{
	auto w = getWindowForId(id);
	if (!w)
	{
		/* RAIL windows are server-authoritative 1:1 (no local scaling) and not in _windows. */
		if (_rail.enabled() && _rail.ownsWindow(id))
			return pos;
		return {};
	}

	/* Ignore errors here, sometimes SDL has no renderer */
	auto renderer = w->renderer();
	if (!renderer)
		return pos;

	SDL_FPoint rpos{};
	if (!SDL_RenderCoordinatesFromWindow(renderer, pos.x, pos.y, &rpos.x, &rpos.y))
		return {};
	removeLocalScaling(rpos.x, rpos.y);
	return rpos;
}

bool SdlContext::screenToRdp(SDL_WindowID id, const SDL_FPoint& pos, SDL_FPoint& rpos)
{
	auto target = getWindowForId(id);
	if (!target)
		return false;

	auto local = pos;
	if (freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon))
	{
		const auto sourceBounds = target->bounds();
		if (!sdl_pointer_in_window(pos, sourceBounds, sourceBounds))
		{
			/* Use this event's position, not the latest global mouse state: queued
			 * events must not inherit a newer pointer position. Only map into RDP
			 * windows; outside them, retain the source mapping and button capture. */
			for (auto& entry : _windows)
			{
				if (const auto point =
				        sdl_pointer_in_window(pos, sourceBounds, entry.second.bounds()))
				{
					target = &entry.second;
					local = *point;
					break;
				}
			}
		}
	}

	rpos = local;
	if (auto renderer = target->renderer())
	{
		if (!SDL_RenderCoordinatesFromWindow(renderer, local.x, local.y, &rpos.x, &rpos.y))
			return false;
		removeLocalScaling(rpos.x, rpos.y);
	}
	applyMonitorOffset(target->id(), rpos.x, rpos.y);
	return true;
}

SDL_FPoint SdlContext::pixelToScreen(SDL_WindowID id, const SDL_FPoint& pos)
{
	auto w = getWindowForId(id);
	if (!w)
	{
		if (_rail.enabled() && _rail.ownsWindow(id))
			return pos;
		return {};
	}

	/* Ignore errors here, sometimes SDL has no renderer */
	auto renderer = w->renderer();
	if (!renderer)
		return pos;

	SDL_FPoint rpos{};
	if (!SDL_RenderCoordinatesToWindow(renderer, pos.x, pos.y, &rpos.x, &rpos.y))
		return {};
	return applyLocalScaling(rpos);
}

SDL_FRect SdlContext::pixelToScreen(SDL_WindowID id, const SDL_FRect& pos, bool round)
{
	const auto fpos = pixelToScreen(id, SDL_FPoint{ pos.x, pos.y });
	const auto size = pixelToScreen(id, SDL_FPoint{ pos.w, pos.h });
	SDL_FRect r{ fpos.x, fpos.y, size.x, size.y };
	if (round)
	{
		r.w = std::ceil(r.w);
		r.h = std::ceil(r.h);
		r.x = std::floor(r.x);
		r.y = std::floor(r.y);
	}
	return r;
}

bool SdlContext::handleEvent(const SDL_Event& ev)
{
	if ((ev.type >= SDL_EVENT_DISPLAY_FIRST) && (ev.type <= SDL_EVENT_DISPLAY_LAST))
	{
		const auto& dev = ev.display;
		return handleEvent(dev);
	}
	if ((ev.type >= SDL_EVENT_WINDOW_FIRST) && (ev.type <= SDL_EVENT_WINDOW_LAST))
	{
		const auto& wev = ev.window;
		return handleEvent(wev);
	}
	switch (ev.type)
	{
		case SDL_EVENT_RENDER_TARGETS_RESET:
		case SDL_EVENT_RENDER_DEVICE_RESET:
		case SDL_EVENT_WILL_ENTER_FOREGROUND:
			return redraw();
		default:
			break;
	}

	if (!isConnected())
		return true;

	switch (ev.type)
	{
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_MOTION:
		{
			const auto& cev = ev.tfinger;
			return handleEvent(cev);
		}
		case SDL_EVENT_MOUSE_MOTION:

		{
			const auto& cev = ev.motion;
			return handleEvent(cev);
		}
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			const auto& cev = ev.button;
			return handleEvent(cev);
		}
		case SDL_EVENT_MOUSE_WHEEL:
		{
			const auto& cev = ev.wheel;
			return handleEvent(cev);
		}
		case SDL_EVENT_CLIPBOARD_UPDATE:
		{
			const auto& cev = ev.clipboard;
			return getClipboardChannelContext().handleEvent(cev);
		}
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		{
			const auto& cev = ev.key;
			if (cev.scancode == SDL_SCANCODE_ESCAPE) {
				if (ev.type == SDL_EVENT_KEY_DOWN && (_refreshEscapeCaptured || refreshMenuOpen())) {
					_refreshEscapeCaptured = true;
					for (auto& entry : _windows) entry.second.closeRefreshMenu();
					std::ignore = redrawWindows(); return true;
				}
				if (ev.type == SDL_EVENT_KEY_UP && _refreshEscapeCaptured) { _refreshEscapeCaptured = false; return true; }
			}
			return getInputChannelContext().handleEvent(cev);
		}
		default:
			return true;
	}
}

COMMAND_LINE_ARGUMENT_A* SdlContext::args()
{
	return _args.data();
}

size_t SdlContext::argsCount() const
{
	if (_args.size() <= 1)
		return 0;
	return _args.size() - 1;
}

int SdlContext::argumentHandler(const COMMAND_LINE_ARGUMENT_A* arg, void* custom)
{
	auto sdl = static_cast<SdlContext*>(custom);
	if (!sdl)
		return -1;

	if (arg->Name)
	{
		if (strcmp(arg->Name, sdl_allow_screensaver) == 0)
		{
			if (arg->Value != nullptr)
			{
				if (!SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1"))
				{
					SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					             "SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER) failed with %s",
					             SDL_GetError());
					return -2;
				}
			}
		}
		else if (strcmp(arg->Name, sdl_connection_dialog) == 0)
			sdl->_connectionDialog = (arg->Value != nullptr);
		else if (strcmp(arg->Name, sdl_refresh_menu) == 0)
			SdlTopBar::setRefreshMenuEnabled(arg->Value != nullptr);
		else if (strcmp(arg->Name, sdl_monitor_scale) == 0)
		{
			if (!sdl->parseMonitorScaleOverrides(arg->Value))
				return COMMAND_LINE_ERROR_UNEXPECTED_VALUE;
		}
	}
	return 0;
}

bool SdlContext::parseMonitorScaleOverrides(const char* value)
{
	if (_monitorScaleOverridesConfigured)
	{
		WLog_Print(_log, WLOG_ERROR,
		           "/sdl-monitor-scale must not be specified more than once");
		return false;
	}

	SdlMonitorScaleOverrides overrides;
	std::string error;
	if (!sdl_parse_monitor_scale_overrides(value, overrides, error))
	{
		WLog_Print(getWLog(), WLOG_ERROR, "%s", error.c_str());
		return false;
	}

	_monitorScaleOverrides = std::move(overrides);
	_monitorScaleOverridesConfigured = true;
	return true;
}

bool SdlContext::validateMonitorScaleOverrides() const
{
	if (!_monitorScaleOverridesConfigured)
		return true;

	const auto settings = context()->settings;
	WINPR_ASSERT(settings);
	const auto mask = freerdp_settings_get_uint64(settings, FreeRDP_MonitorOverrideFlags);
	if ((mask & (FREERDP_MONITOR_OVERRIDE_DESKTOP_SCALE |
	             FREERDP_MONITOR_OVERRIDE_DEVICE_SCALE)) != 0)
	{
		WLog_Print(_log, WLOG_ERROR,
		           "/sdl-monitor-scale cannot be combined with global desktop/device scale "
		           "overrides");
		return false;
	}

	for (const auto& entry : _monitorScaleOverrides)
	{
		if (_displays.find(entry.first) == _displays.end())
		{
			WLog_Print(_log, WLOG_ERROR,
			           "Monitor scale override references unknown SDL monitor ID %" PRIu32
			           ". Use /list:monitor to list available monitor IDs.",
			           entry.first);
			return false;
		}
	}

	return true;
}

void SdlContext::applyMonitorScaleOverride(rdpMonitor& monitor) const
{
	const auto desktopScaleFactor = monitor.attributes.desktopScaleFactor;
	const auto deviceScaleFactor = monitor.attributes.deviceScaleFactor;
	if (!sdl_apply_monitor_scale_override(_monitorScaleOverrides, monitor))
		return;

	/* INFO, not DEBUG: a silent mismatch here (e.g. SDL display IDs renumbered
	 * between sessions) inverts the server's DPI map. The name pins the
	 * override to a physical display. */
	const char* name = SDL_GetDisplayName(monitor.orig_screen);
	WLog_Print(_log, WLOG_INFO,
	           "monitor %" PRIu32 " ('%s') scale override: desktopScaleFactor %" PRIu32
	           " -> %" PRIu32 ", deviceScaleFactor %" PRIu32 " -> %" PRIu32,
	           monitor.orig_screen, name ? name : "?", desktopScaleFactor,
	           monitor.attributes.desktopScaleFactor, deviceScaleFactor,
	           monitor.attributes.deviceScaleFactor);
}

CriticalSection& SdlContext::lock()
{
	return _critical;
}

std::vector<rdpPointer*>& SdlContext::pointers()
{
	return _valid_pointers;
}

bool SdlContext::contains(const rdpPointer* ptr) const
{
	for (const auto& cur : _valid_pointers)
	{
		if (cur == ptr)
			return true;
	}
	return false;
}

bool SdlContext::credentialsRead() const
{
	return _credentialsRead;
}

void SdlContext::setCredentialsRead()
{
	_credentialsRead = true;
}

bool SdlContext::resizeToScale(SdlWindow* window)
{
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	// Constructor sizing still translates the configured remote pixels once.
	// Thereafter preserve the user's logical window size, including display moves.
	if (auto bridge = SdlLauncher::active(); bridge && bridge->windowGeometryEnabled() &&
	    !freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon)) return true;
#endif
	if (freerdp_settings_get_bool(context()->settings, FreeRDP_SmartSizing))
		return true;
	if (!useLocalScale())
		return true;
	if (!window)
		return false;
	return window->resizeToScale();
}

bool SdlContext::useLocalScale() const
{
	const auto ssize = freerdp_settings_get_bool(context()->settings, FreeRDP_SmartSizing);
	if (ssize)
		return true;
	const auto dynResize =
	    freerdp_settings_get_bool(context()->settings, FreeRDP_DynamicResolutionUpdate);
	const auto fs = freerdp_settings_get_bool(context()->settings, FreeRDP_Fullscreen);
	const auto multimon = freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon);
	return !dynResize && !fs && !multimon;
}

bool SdlContext::drawToWindows(const std::vector<SDL_Rect>& rects, bool remoteContent)
{
	/* RAIL damage is per-window (_gfxDamage), not in the rects queue: repaint every tick. */
	if (_rail.enabled())
	{
		for (auto& window : _windows)
		{
			if ((SDL_GetWindowFlags(window.second.window()) & SDL_WINDOW_HIDDEN) == 0)
				SDL_HideWindow(window.second.window());
		}

		std::unique_lock lock(_critical);
		_rail.paint(_primary.get(), pixelFormat(), rects);
		return true;
	}

	/* Non-RAIL mode (e.g. Non-Monitored Desktop during UAC / Consent UI): show the desktop window.
	 * Before the RemoteApp starts, only when the logon needs the user. */
	if (freerdp_settings_get_bool(context()->settings, FreeRDP_RemoteApplicationMode) &&
	    !_rail.desktopMayShow())
		return true;
	bool firstShow = false;
	for (auto& window : _windows)
	{
		if (!window.second.revealed())
		{
			window.second.reveal();
			firstShow = true;
		}
	}

	if (rects.empty() && !firstShow)
		return true;
	if (!isConnected())
		return true;

	const auto settings = context()->settings;
	const bool multimon = freerdp_settings_get_bool(settings, FreeRDP_UseMultimon);
	const bool scaled = useLocalScale();

	for (auto& window : _windows)
	{
		auto& target = window.second;
		/* A newly shown window has no valid content yet: repaint it completely. */
		if (firstShow)
		{
			if (!drawToWindow(target))
				return false;
			continue;
		}
		if (scaled)
		{
			if (!drawToWindow(target, rects,remoteContent))
				return false;
			continue;
		}

		/* Recreate and clear the target before deciding that this window has no
		 * overlap. A newly created target requires a full source repaint even if
		 * the current damage belongs to another monitor. */
		if (!target.ensureRenderTarget())
			return false;
		/* Keep each framebuffer snapshot and its clipping/draw operation under
		 * the same recursive lock used by drawToWindow. This preserves the
		 * existing unlock between windows while avoiding stale surfaces during a
		 * resize or reconnect. */
		std::unique_lock lock(_critical);
		auto surface = _primary.get();
		if (!surface || (surface->w <= 0) || (surface->h <= 0))
			return false;
		const SDL_Rect sourceBounds = { 0, 0, surface->w, surface->h };
		if (target.needsFullRedraw(surface->w, surface->h))
		{
			if (!drawToWindow(target, {},remoteContent))
				return false;
			continue;
		}

		SDL_Point offset{ 0, 0 };
		if (multimon)
			offset = { target.offsetX(), target.offsetY() };
		const auto clipped = sdl::render::clipSourceRects(rects, sourceBounds, offset,
		                                                  target.pixelViewport());
		if (clipped.empty())
		{
			target.renderMetrics().notePresentSkip();
			continue;
		}
		if (!drawToWindow(target, clipped,remoteContent))
			return false;
	}

	flushQueueMetrics(false);
	return true;
}

BOOL SdlContext::desktopResize(rdpContext* context)
{
	rdpGdi* gdi = nullptr;
	rdpSettings* settings = nullptr;
	auto sdl = get_context(context);

	WINPR_ASSERT(sdl);
	WINPR_ASSERT(context);

	settings = context->settings;
	WINPR_ASSERT(settings);

	std::unique_lock lock(sdl->_critical);
	gdi = context->gdi;
	if (!gdi_resize(gdi, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
	                freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)))
		return FALSE;
	return sdl->createPrimary();
}

/* This function is called to output a System BEEP */
BOOL SdlContext::playSound(rdpContext* context, const PLAY_SOUND_UPDATE* play_sound)
{
	/* TODO: Implement */
	WINPR_UNUSED(context);
	WINPR_UNUSED(play_sound);
	return TRUE;
}

/* This function is called whenever a new frame starts.
 * It can be used to reset invalidated areas. */
BOOL SdlContext::beginPaint(rdpContext* context)
{
	auto gdi = context->gdi;
	WINPR_ASSERT(gdi);
	WINPR_ASSERT(gdi->primary);

	HGDI_DC hdc = gdi->primary->hdc;
	WINPR_ASSERT(hdc);
	if (!hdc->hwnd)
		return TRUE;

	HGDI_WND hwnd = hdc->hwnd;
	WINPR_ASSERT(hwnd->invalid);
	hwnd->invalid->null = TRUE;
	hwnd->ninvalid = 0;

	return TRUE;
}

bool SdlContext::redraw(bool suppress)
{
	std::lock_guard outputLock(_screenOutputMutex);
	if (!_connected)
		return true;

	/* RemoteApp desktop windows are placeholders, hiding one must not stop server output. */
	if (suppress && freerdp_settings_get_bool(context()->settings, FreeRDP_RemoteApplicationMode))
		return true;

	auto gdi = context()->gdi;
	if (!gdi) return true;
	_screenOutputSuppressed = suppress;
	return gdi_send_suppress_output(gdi, suppress);
}

void SdlContext::requestScreenRefreshModes(uint32_t modes)
{
    for (const auto mode : sdlRefreshModes)
        if ((modes & sdlRefreshBit(mode)) != 0) requestScreenRefresh(mode);
}

void SdlContext::requestScreenRefresh(SdlRefreshMode mode)
{
    if (!isConnected() || isReconnecting() || _stopRequested) return;
    WLog_Print(_log, WLOG_INFO, "Screen refresh requested: method=%s", sdlRefreshName(mode));
    const bool rebuild = mode == SdlRefreshMode::Textures || mode == SdlRefreshMode::Combined;
    bool localOK = true;
    std::unique_lock frameLock(_critical);
    if (!isConnected() || isReconnecting() || _stopRequested) return;
    for (auto& entry : _windows) {
        if (rebuild && !entry.second.rebuildDesktopTextures()) localOK = false;
        if (!drawToWindow(entry.second)) localOK = false;
    }
    frameLock.unlock();
    WLog_Print(_log, localOK ? WLOG_INFO : WLOG_WARN,
               "Screen refresh local repaint: method=%s result=%s",
               sdlRefreshName(mode), localOK ? "presented" : "failed");
    if (mode != SdlRefreshMode::Local && mode != SdlRefreshMode::Textures) {
        _screenRefreshModes.fetch_or(sdlRefreshBit(mode));
        _screenRefreshEvent.set();
    }
}

bool SdlContext::serviceScreenRefresh()
{
    if (!_screenRefreshEvent.isSet() && !_screenResumePending) return true;
    _screenRefreshEvent.clear();
    if (!isConnected() || isReconnecting() || _stopRequested) {
        _screenRefreshModes = 0; _screenResumePending = false; return true;
    }
    std::lock_guard outputLock(_screenOutputMutex);
    if (_screenOutputSuppressed) {
        _screenRefreshModes = 0; _screenResumePending = false; return true;
    }
    const auto settings = context()->settings;
    const auto width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const auto height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    if (width == 0 || height == 0 || width - 1 > UINT16_MAX || height - 1 > UINT16_MAX) {
        _screenRefreshModes = 0;
        // A pending resume must not be abandoned if dimensions are transiently invalid.
        if (_screenResumePending && ++_screenResumeRetries >= 3) return false;
        return true;
    }
    const RECTANGLE_16 area{0, 0, static_cast<UINT16>(width - 1), static_cast<UINT16>(height - 1)};
    const auto update = context()->update;
    const auto resume = [&]() {
        return update->SuppressOutput && update->SuppressOutput(context(), 1, &area);
    };
    if (_screenResumePending) {
        if (resume()) {
            _screenResumePending = false; _screenResumeRetries = 0;
            WLog_Print(_log, WLOG_INFO, "Screen refresh output resume retry succeeded");
        } else {
            WLog_Print(_log, WLOG_WARN, "Screen refresh output resume retry failed");
            return ++_screenResumeRetries < 3;
        }
    }
    const uint32_t pending = _screenRefreshModes.exchange(0);
    for (const auto mode : sdlRefreshModes) {
        if ((pending & sdlRefreshBit(mode)) == 0) continue;
        const bool restart = mode == SdlRefreshMode::Updates || mode == SdlRefreshMode::Combined;
        const bool server = mode == SdlRefreshMode::Server || mode == SdlRefreshMode::Combined;
        if (restart) {
            if (freerdp_settings_get_bool(settings, FreeRDP_SuppressOutput) && update->SuppressOutput) {
                const bool paused = update->SuppressOutput(context(), 0, nullptr) != FALSE;
                // Always attempt resume, including when the pause call failed.
                const bool resumed = resume();
                WLog_Print(_log, paused && resumed ? WLOG_INFO : WLOG_WARN,
                           "Screen refresh output restart: method=%s pause=%d resume=%d",
                           sdlRefreshName(mode), paused, resumed);
                if (!resumed) {
                    _screenResumePending = true; _screenResumeRetries = 0;
                    // Preserve subsequent methods until output is resumed.
                    const uint32_t remaining = pending & ~((sdlRefreshBit(mode) << 1) - 1);
                    _screenRefreshModes.fetch_or(remaining | (server ? sdlRefreshBit(SdlRefreshMode::Server) : 0));
                    return true;
                }
            } else WLog_Print(_log, WLOG_WARN, "Screen refresh output restart unsupported: method=%s", sdlRefreshName(mode));
        }
        if (server) {
            if (freerdp_settings_get_bool(settings, FreeRDP_RefreshRect) && update->RefreshRect) {
                const bool sent = update->RefreshRect(context(), 1, &area) != FALSE;
                WLog_Print(_log, sent ? WLOG_INFO : WLOG_WARN,
                           "Screen refresh server redraw: method=%s result=%s", sdlRefreshName(mode), sent ? "sent" : "failed");
            } else WLog_Print(_log, WLOG_WARN, "Screen refresh server redraw unsupported: method=%s", sdlRefreshName(mode));
        }
    }
    return true;
}

bool SdlContext::refreshMenuOpen() const
{
    const auto window = getWindowForId(_fullscreenState.barOwner);
    return _fullscreenState.barVisible && window && window->refreshMenuOpen();
}

void SdlContext::serviceRefreshHover()
{
    auto window = getWindowForId(_fullscreenState.barOwner);
    if (window && window->serviceTopBarHover(SDL_GetTicks())) {
        if (!_fullscreenState.barPinned && _topBarHoveredButton == SdlTopBarButton::None)
            _fullscreenState.barVisible = false;
        std::ignore = redrawWindows();
    }
}

void SdlContext::setConnected(bool val)
{
	_connected = val;
	if (!val) {
		_screenRefreshEvent.clear(); _screenRefreshModes = 0;
		_screenResumePending = false; _screenResumeRetries = 0; _screenOutputSuppressed = false;
	}
}

bool SdlContext::isConnected() const
{
	return _connected;
}

void SdlContext::setReconnecting(bool val)
{
	if (val) {
		_screenRefreshEvent.clear(); _screenRefreshModes = 0;
		_screenResumePending = false; _screenResumeRetries = 0;
	}
	if (_reconnecting.exchange(val) == val)
		return;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active())
		bridge->state(val ? "reconnecting" : "connected");
#endif
	/* Main thread presents the overlay / repaints; the RDP thread only
	 * flips the atomic and notifies. */
	std::ignore = sdl_push_user_event(SDL_EVENT_USER_RECONNECTING, val ? 1 : 0);
}

bool SdlContext::isReconnecting() const
{
	return _reconnecting;
}

bool SdlContext::redrawStalled()
{
	const int dots = stalledDots();
	const auto pointer = topBarPointerForDraw();
	for (auto& window : _windows)
	{
		window.second.closeRefreshMenu();
		if (!window.second.updateStalledSurface(dots, showTopBarOn(window.second),
		                                        _fullscreenState.barPinned, pointer))
			return false;
	}
	flushQueueMetrics(false);
	return true;
}

bool SdlContext::repaintAll()
{
	auto surface = _primary.get();
	if (!surface || (surface->w <= 0) || (surface->h <= 0))
		return true;
	const SDL_Rect full = { 0, 0, surface->w, surface->h };
	return drawToWindows({ full });
}

rdpContext* SdlContext::context() const
{
	WINPR_ASSERT(_context);
	return _context;
}

rdpClientContext* SdlContext::common() const
{
	return reinterpret_cast<rdpClientContext*>(context());
}

bool SdlContext::setCursor(CursorType type)
{
	_cursorType = type;
	return restoreCursor();
}

bool SdlContext::setCursor(const rdpPointer* cursor)
{
	std::unique_lock lock(_critical);
	if (!contains(cursor))
		return true;

	_cursor = { sdl_Pointer_Copy(cursor), sdl_PointerFreeCopyAll };
	return setCursor(CURSOR_IMAGE);
}

rdpPointer* SdlContext::cursor() const
{
	return _cursor.get();
}

bool SdlContext::localCursorOverride() const
{
	/* The server owns the cursor shape, and may have hidden it. Local UI (the
	 * reconnect overlay and the connection bar) must stay pointable anyway. */
	return isReconnecting() || _topBarPointerOver;
}

bool SdlContext::setTopBarPointerOver(bool over)
{
	if (_topBarPointerOver == over)
		return true;
	_topBarPointerOver = over;
	return restoreCursor();
}

bool SdlContext::restoreCursor()
{
	WLog_Print(getWLog(), WLOG_TRACE, "restore cursor: %d", _cursorType);
	if (localCursorOverride())
	{
		/* Keep hasCursor() tracking the server cursor: it selects relative mouse mode. */
		setHasCursor(_cursorType != CURSOR_NULL);
#ifdef WITH_SDL_LAUNCHER_BRIDGE
		if (auto bridge = SdlLauncher::active(); bridge && SdlHosted::enabled())
		{
			bridge->hostedCursor("default");
			return true;
		}
#endif
		if (!SDL_SetCursor(SDL_GetDefaultCursor()) || !SDL_ShowCursor())
		{
			WLog_Print(getWLog(), WLOG_ERROR, "Unable to show the local cursor");
			return false;
		}
		return true;
	}
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	if (auto bridge = SdlLauncher::active();
	    bridge && SdlHosted::enabled() && _cursorType != CURSOR_IMAGE)
	{
		bridge->hostedCursor(_cursorType == CURSOR_NULL ? "hidden" : "default");
		setHasCursor(_cursorType != CURSOR_NULL);
		return true;
	}
#endif
	switch (_cursorType)
	{
		case CURSOR_NULL:
			if (!SDL_HideCursor())
			{
				WLog_Print(getWLog(), WLOG_ERROR, "SDL_HideCursor failed");
				return false;
			}

			setHasCursor(false);
			return true;

		case CURSOR_DEFAULT:
		{
			auto def = SDL_GetDefaultCursor();
			if (!SDL_SetCursor(def))
			{
				WLog_Print(getWLog(), WLOG_ERROR, "SDL_SetCursor(default=%p) failed",
				           static_cast<void*>(def));
				return false;
			}
			if (!SDL_ShowCursor())
			{
				WLog_Print(getWLog(), WLOG_ERROR, "SDL_ShowCursor failed");
				return false;
			}
			setHasCursor(true);
			return true;
		}
		case CURSOR_IMAGE:
			setHasCursor(true);
			return sdl_Pointer_Set_Process(this);
		default:
			WLog_Print(getWLog(), WLOG_ERROR, "Unknown cursorType %s",
			           sdl::utils::toString(_cursorType).c_str());
			return false;
	}
}

void SdlContext::applyPrimaryMonitor(std::vector<rdpMonitor>& monitors) const
{
	if (monitors.empty())
		return;
	auto primary = monitors.end();
	// Explicit /monitors: order comes from the launcher's saved UUID list.
	// If its primary disappears, choose the first remaining selected display.
	if (freerdp_settings_get_uint32(context()->settings, FreeRDP_NumMonitorIds) > 0)
	{
		for (const auto id : _monitorIds)
		{
			primary = std::find_if(monitors.begin(), monitors.end(),
			                       [id](const rdpMonitor& m) { return m.orig_screen == id; });
			if (primary != monitors.end())
				break;
		}
	}
	if (primary == monitors.end())
		primary = std::find_if(monitors.begin(), monitors.end(),
		                       [](const rdpMonitor& m) { return m.is_primary != 0; });
	if (primary == monitors.end())
		primary = monitors.begin();
	for (auto& monitor : monitors)
		monitor.is_primary = (&monitor == &*primary);
}

void SdlContext::setMonitorIds(const std::vector<SDL_DisplayID>& ids)
{
	_monitorIds.clear();
	for (auto id : ids)
	{
		_monitorIds.push_back(id);
	}
}

const std::vector<SDL_DisplayID>& SdlContext::monitorIds() const
{
	return _monitorIds;
}

int64_t SdlContext::monitorId(uint32_t index) const
{
	if (index >= _monitorIds.size())
	{
		return -1;
	}
	return _monitorIds.at(index);
}

bool SdlContext::push(const std::vector<SDL_Rect>& rects)
{
	return _updates.push(rects, SDL_EVENT_USER_UPDATE);
}

std::vector<SDL_Rect> SdlContext::pop()
{
	return _updates.pop();
}

void SdlContext::noteMotionsCoalesced(uint64_t count)
{
	_queueAccum.motionsCoalesced += count;
	if(_performanceEnabled) _performanceQueue.motionsCoalesced+=count;
}

void SdlContext::noteUpdateReceived()
{
	++_queueAccum.updateReceived;
	if(_performanceEnabled) ++_performanceQueue.updateReceived;
}

void SdlContext::noteUpdateActed()
{
	++_queueAccum.updateActed;
	if(_performanceEnabled) ++_performanceQueue.updateActed;
}

void SdlContext::flushQueueMetrics(bool force)
{
	const auto snapshot = _updates.takeSnapshot();
    if(_performanceEnabled) {
        _performanceQueue.pops+=snapshot.pops;
        _performanceQueue.queueWaitNs+=snapshot.queueWaitNs;
        _performanceQueue.pushes+=snapshot.pushes;
        _performanceQueue.collapsedEvents+=snapshot.collapsedEvents;
    }
	_queueAccum.pushes += snapshot.pushes;
	_queueAccum.attemptedRects += snapshot.attemptedRects;
	_queueAccum.mergedRects += snapshot.mergedRects;
	_queueAccum.collapsedEvents += snapshot.collapsedEvents;
	_queueAccum.pops += snapshot.pops;
	_queueAccum.emptyPops += snapshot.emptyPops;
	_queueAccum.popRects += snapshot.popRects;
	_queueAccum.queueWaitNs += snapshot.queueWaitNs;

	/* Codec-side threadpool counters, polled here on the UI thread.
	 * Unsigned delta arithmetic absorbs 32-bit wraps. Offline replay
	 * drives decoding too, but without queue pops no record is written. */
	uint32_t tiles = 0;
	uint32_t created = 0;
	uint32_t reused = 0;
	yuv_pool_stats(&tiles, &created, &reused);
	_queueAccum.yuvTiles += tiles - _yuvLastTiles;
	_queueAccum.yuvWorkCreated += created - _yuvLastCreated;
	_queueAccum.yuvWorkReused += reused - _yuvLastReused;
	_yuvLastTiles = tiles;
	_yuvLastCreated = created;
	_yuvLastReused = reused;

	if (!_queueAccum.active())
		return;

	const auto now = SDL_GetTicksNS();
	if (_queueIntervalStartNs == 0)
		_queueIntervalStartNs = now;
	const uint64_t duration = now >= _queueIntervalStartNs ? now - _queueIntervalStartNs : 0;
	/* Offline replay drives drawToWindows without the event queue, so it
	 * never pops: with no pops there is nothing to attribute and no record
	 * is written, keeping replay metrics comparable. */
	if (!force && (duration < SdlRenderMetrics::intervalNs || _queueAccum.pops == 0))
		return;

	char line[1024];
	std::snprintf(line, sizeof(line),
	              "{\"schema\":\"freerdp.sdl_queue_metrics\",\"version\":1,"
	              "\"window_id\":0,\"monitor_id\":0,"
	              "\"interval_start_ns\":%llu,\"interval_duration_ns\":%llu,"
	              "\"pushes\":%llu,\"attempted_rects\":%llu,"
	              "\"merged_rects\":%llu,\"collapsed_events\":%llu,"
	              "\"pops\":%llu,\"empty_pops\":%llu,\"pop_rects\":%llu,"
	              "\"queue_wait_ns\":%llu,"
	              "\"update_events_received\":%llu,\"update_events_acted\":%llu,"
	              "\"motions_coalesced\":%llu,"
	              "\"yuv_tiles\":%llu,\"yuv_work_created\":%llu,\"yuv_work_reused\":%llu}",
	              static_cast<unsigned long long>(_queueIntervalStartNs),
	              static_cast<unsigned long long>(duration),
	              static_cast<unsigned long long>(_queueAccum.pushes),
	              static_cast<unsigned long long>(_queueAccum.attemptedRects),
	              static_cast<unsigned long long>(_queueAccum.mergedRects),
	              static_cast<unsigned long long>(_queueAccum.collapsedEvents),
	              static_cast<unsigned long long>(_queueAccum.pops),
	              static_cast<unsigned long long>(_queueAccum.emptyPops),
	              static_cast<unsigned long long>(_queueAccum.popRects),
	              static_cast<unsigned long long>(_queueAccum.queueWaitNs),
	              static_cast<unsigned long long>(_queueAccum.updateReceived),
	              static_cast<unsigned long long>(_queueAccum.updateActed),
	              static_cast<unsigned long long>(_queueAccum.motionsCoalesced),
	              static_cast<unsigned long long>(_queueAccum.yuvTiles),
	              static_cast<unsigned long long>(_queueAccum.yuvWorkCreated),
	              static_cast<unsigned long long>(_queueAccum.yuvWorkReused));
	line[sizeof(line) - 1] = '\0';
	SdlRenderMetrics::appendJsonLine(line);
	_queueAccum = QueueAccum{};
	_queueIntervalStartNs = now;
}

static void* fullscreenEventWindow(SDL_WindowID id)
{
	return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

bool SdlContext::setFullscreen(bool enter, bool forceOriginalDisplay)
{
	/* Window id 0 asks the main thread to make the session request there, so
	 * the fullscreen state is only ever touched on the SDL main thread. */
	if (!SDL_IsMainThread())
		return sdl_push_user_event(SDL_EVENT_USER_WINDOW_FULLSCREEN, fullscreenEventWindow(0),
		                           enter, forceOriginalDisplay);

	/* Only record the request. The bar and each window's state change when
	 * the windows report that they actually entered or left fullscreen. */
	_fullscreenState.request(enter, SDL_GetTicks());
	WLog_Print(getWLog(), WLOG_DEBUG, "Requesting fullscreen %s for %" PRIuz " window(s)",
	           enter ? "enter" : "leave", _windows.size());
	for (const auto& window : _windows)
	{
		if (!sdl_push_user_event(SDL_EVENT_USER_WINDOW_FULLSCREEN,
		                         fullscreenEventWindow(window.first), enter, forceOriginalDisplay))
			return false;
	}
	return true;
}

bool SdlContext::applyFullscreen(SDL_WindowID id, bool enter, bool forceOriginalDisplay)
{
	auto window = getWindowForId(id);
	if (!window || !window->window())
		return true;
	const auto result = window->fullscreen(enter, forceOriginalDisplay);
	const auto flags = SDL_GetWindowFlags(window->window());
	/* SDL sets the fullscreen flag of a shown window only when it sends the
	 * enter/leave event; a hidden window's flag is only what is pending. */
	const bool shown = (flags & SDL_WINDOW_HIDDEN) == 0;
	const bool actual = shown ? (flags & SDL_WINDOW_FULLSCREEN) != 0
	                          : _fullscreenState.windowFullscreen(id);
	switch (result)
	{
		case SdlWindow::FullscreenResult::Failed:
		{
			WLog_Print(getWLog(), WLOG_ERROR,
			           "Window %" PRIu32 " could not %s fullscreen: %s; it stays %s", id,
			           enter ? "enter" : "leave", SDL_GetError(),
			           actual ? "fullscreen" : "windowed");
			return presentFullscreen(id, actual, _fullscreenState.requestFailed(id, actual));
		}
		case SdlWindow::FullscreenResult::Unsynced:
			WLog_Print(getWLog(), WLOG_WARN,
			           "Window %" PRIu32 " fullscreen %s did not settle in time: %s", id,
			           enter ? "enter" : "leave", SDL_GetError());
			break;
		case SdlWindow::FullscreenResult::Applied:
		default:
			break;
	}
	/* SDL sends no event when the window already is in the requested state.
	 * A hidden window asked to stay windowed will not send one either. */
	if (actual == enter && (shown || !enter))
		return observeFullscreen(id, actual);
	return true;
}

bool SdlContext::observeFullscreen(SDL_WindowID id, bool fullscreen)
{
	return presentFullscreen(id, fullscreen, _fullscreenState.observe(id, fullscreen));
}

bool SdlContext::presentFullscreen(SDL_WindowID id, bool fullscreen,
                                   const SdlFullscreenState::Observation& observed)
{
	if (!observed.changed)
		return true;
	WLog_Print(getWLog(), WLOG_INFO, "Window %" PRIu32 " %s fullscreen (%s)%s", id,
	           fullscreen ? "entered" : "left", observed.requested ? "requested" : "native control",
	           observed.owner ? (fullscreen ? "; showing the connection bar"
	                                        : "; hiding the connection bar")
	                          : "");
	if (!observed.owner)
		return true;
	resetTopBarPointer();
	if (auto window = getWindowForId(id); window && !fullscreen)
		window->closeRefreshMenu();
	return redrawWindows();
}

const SdlFullscreenState& SdlContext::fullscreenState() const
{
	return _fullscreenState;
}

bool SdlContext::topBarAllowed() const
{
	return _fullscreenState.barAllowed(
	    freerdp_settings_get_bool(context()->settings, FreeRDP_UseMultimon));
}

/* The window of the session's primary display: the first selected display
 * that still has a window, otherwise the system primary, otherwise any. */
SDL_WindowID SdlContext::primaryWindowId() const
{
	std::vector<rdpMonitor> monitors;
	std::vector<SDL_WindowID> ids;
	for (const auto& entry : _windows)
	{
		monitors.push_back(entry.second.monitor(false));
		ids.push_back(entry.first);
	}
	applyPrimaryMonitor(monitors);
	for (size_t x = 0; x < monitors.size(); x++)
		if (monitors[x].is_primary)
			return ids[x];
	return ids.empty() ? 0 : ids.front();
}

/* Give the bar to `id` if it has none, or with `replace` in any case. */
void SdlContext::reassignTopBar(SDL_WindowID id, bool replace)
{
	const auto previous = _fullscreenState.barOwner;
	if (replace)
		_fullscreenState.setOwner(id, true);
	else
		_fullscreenState.ensureOwner(id);
	if (_fullscreenState.barOwner != previous)
	{
		WLog_Print(getWLog(), WLOG_DEBUG, "Connection bar moved to window %" PRIu32, id);
		resetTopBarPointer();
	}
}

void SdlContext::resetTopBarPointer()
{
	_topBarPointer = { -1.0f, -1.0f };
	_topBarHoveredButton = SdlTopBarButton::None;
	if (!setTopBarPointerOver(false))
		WLog_Print(_log, WLOG_WARN, "Unable to update the cursor over the SDL connection bar");
}

bool SdlContext::setMinimized()
{
	return sdl_push_user_event(SDL_EVENT_USER_WINDOW_MINIMIZE);
}

bool SdlContext::grabMouse() const
{
	return _grabMouse;
}

bool SdlContext::toggleGrabMouse()
{
	return setGrabMouse(!grabMouse());
}

bool SdlContext::setGrabMouse(bool enter)
{
	_grabMouse = enter;
	return true;
}

bool SdlContext::grabKeyboard() const
{
	return _grabKeyboard;
}

bool SdlContext::toggleGrabKeyboard()
{
	return setGrabKeyboard(!grabKeyboard());
}

bool SdlContext::setGrabKeyboard(bool enter)
{
	_grabKeyboard = enter;
	return true;
}

bool SdlContext::setResizeable(bool enable)
{
	const auto settings = context()->settings;
	const bool dyn = freerdp_settings_get_bool(settings, FreeRDP_DynamicResolutionUpdate);
	const bool smart = freerdp_settings_get_bool(settings, FreeRDP_SmartSizing);
	bool use = (dyn && enable) || smart;
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	// Remembering local geometry must not depend on the server negotiating
	// dynamic resolution. Fixed-resolution desktops keep their framebuffer.
	if (auto bridge = SdlLauncher::active(); bridge && bridge->windowGeometryEnabled() &&
	    !freerdp_settings_get_bool(settings, FreeRDP_UseMultimon)) use = true;
#endif

	for (auto& window : _windows)
	{
		if (SDL_IsMainThread())
			window.second.resizeable(use);
		else if (!sdl_push_user_event(SDL_EVENT_USER_WINDOW_RESIZEABLE, &window.second, use))
			return false;
	}
	_resizeable = use;

	return true;
}

bool SdlContext::resizeable() const
{
	return _resizeable;
}

bool SdlContext::toggleResizeable()
{
	return setResizeable(!resizeable());
}

bool SdlContext::fullscreen() const
{
	return _fullscreenState.sessionFullscreen(SDL_GetTicks());
}

bool SdlContext::toggleFullscreen(SDL_WindowID origin)
{
	return setFullscreen(_fullscreenState.toggleTarget(origin, SDL_GetTicks()));
}
