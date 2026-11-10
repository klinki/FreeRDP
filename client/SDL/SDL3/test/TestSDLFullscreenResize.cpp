/**
 * Regression coverage for fullscreen drawable size and delayed display-channel activation.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <algorithm>
#include <cstdio>
#include <memory>
#include <winpr/sysinfo.h>
#include <vector>

#include "sdl_context.hpp"
#include "sdl_types.hpp"
#include "dialogs/sdl_dialogs.hpp"

static std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> sentLayouts;
static unsigned layoutCalls = 0;

static bool expect(bool condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

static UINT captureLayout(DispClientContext*, UINT32 count,
                          DISPLAY_CONTROL_MONITOR_LAYOUT* layouts)
{
	if (!expect(SDL_IsMainThread(), "Display Control sends stay on SDL main thread"))
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;
	layoutCalls++;
	sentLayouts.assign(layouts, layouts + count);
	return CHANNEL_RC_OK;
}

static bool run()
{
	std::unique_ptr<freerdp, decltype(&freerdp_free)> instance(freerdp_new(), freerdp_free);
	if (!instance)
		return false;
	instance->ContextSize = sizeof(sdl_rdp_context);
	if (!freerdp_context_new(instance.get()))
		return false;
	struct ContextGuard
	{
		freerdp* instance;
		~ContextGuard() { freerdp_context_free(instance); }
	} contextGuard{ instance.get() };
	SdlContext sdl(instance->context);
	reinterpret_cast<sdl_rdp_context*>(instance->context)->sdl = &sdl;
	auto settings = instance->context->settings;
	if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "fullscreen-test.invalid") ||
	    !freerdp_settings_set_bool(settings, FreeRDP_Fullscreen, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE) ||
	    !sdl.detectDisplays())
		return false;
	const auto displays = sdl.getDisplayIds();
	if (displays.empty())
		return false;
	// Prefer the laptop for native runs even when external displays are attached.
	auto display = displays.front();
	for (const auto id : displays)
	{
		const auto name = SDL_GetDisplayName(id);
		if (name && SDL_strstr(name, "Built-in"))
			display = id;
	}
	printf("Fullscreen display: %s\n", SDL_GetDisplayName(display));
	if (!sdl.addDisplayWindow(display))
		return false;
	auto window = sdl.getFirstWindow();
	if (!window || !window->window())
		return false;
	std::ignore = SDL_SyncWindow(window->window());
	const auto pixels = window->rect();
	printf("Fullscreen drawable: %dx%d pixels\n", pixels.w, pixels.h);
	if (!expect(pixels.w > 0 && pixels.h > 0, "drawable pixel dimensions available"))
		return false;

	// The initial full-display probe includes space that the actual fullscreen
	// content can exclude on a notched Mac. Seed an oversized cached definition.
	auto monitor = window->monitor(true);
	monitor.width = pixels.w + 64;
	monitor.height = pixels.h + 66;
	monitor.attributes.desktopScaleFactor = 175;
	monitor.attributes.deviceScaleFactor = 140;
	window->setMonitor(monitor);
	if (!sdl.updateWindowList())
		return false;

	DispClientContext channel{};
	channel.SendMonitorLayout = captureLayout;
	auto& disp = sdl.getDisplayChannelContext();
	if (!disp.init(&channel))
		return false;
	SDL_WindowEvent resized{};
	resized.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
	resized.windowID = window->id();
	if (!expect(disp.handleEvent(resized), "fullscreen pixel-size event handled"))
		return false;
	monitor = window->monitor(true);
	if (!expect(monitor.width == pixels.w && monitor.height == pixels.h,
	            "fullscreen uses drawable pixels instead of initial full-display bounds") ||
	    !expect(monitor.attributes.desktopScaleFactor == 175 &&
	            monitor.attributes.deviceScaleFactor == 140,
	            "fullscreen resize preserves scale overrides") ||
	    !expect(sentLayouts.empty(), "no layout sent before display-channel capabilities"))
		return false;

	if (!expect(channel.DisplayControlCaps != nullptr &&
	            channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "delayed fullscreen display-channel activation succeeds"))
		return false;
	// Windows ignores a layout sent right after the capabilities.
	const UINT64 activated = GetTickCount64();
	disp.service(activated);
	if (!expect(sentLayouts.empty(), "activation waits the minimum resize delay"))
		return false;
	disp.service(activated + 500);
	if (!expect(sentLayouts.size() == 1 && sentLayouts.front().Width == static_cast<UINT32>(pixels.w) &&
	            sentLayouts.front().Height == static_cast<UINT32>(pixels.h),
	            "activation sends the corrected fullscreen layout without another resize") ||
	    !expect(sentLayouts.front().Left == 0 && sentLayouts.front().Top == 0 &&
	            sentLayouts.front().Flags == DISPLAY_CONTROL_MONITOR_PRIMARY &&
	            sentLayouts.front().DesktopScaleFactor == 175 &&
	            sentLayouts.front().DeviceScaleFactor == 140,
	            "corrected layout retains primary origin and effective scaling"))
		return false;
	if (!expect(disp.handleEvent(resized) && layoutCalls == 1,
	            "repeated same-size events do not resend the layout"))
		return false;

	// A removed display's window may already have migrated to the laptop. Do not
	// replace its physical ownership/geometry until topology reconciliation.
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE))
		return false;
	const auto physical = window->monitor(false).orig_screen;
	constexpr SDL_DisplayID removed = 0xfffffff0;
	monitor.width = pixels.w + 64;
	monitor.height = pixels.h + 66;
	monitor.orig_screen = removed;
	window->setMonitor(monitor);
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(false).orig_screen == removed &&
	            window->monitor(false).width == monitor.width &&
	            window->monitor(false).height == monitor.height,
	            "migrated multimon window retains removed display ownership and geometry"))
		return false;

	// Two windows can coexist on the remaining display during a removal burst.
	// The live display's own window must still refresh while the migrated one waits.
	if (!sdl.addDisplayWindow(physical))
		return false;
	int count = 0;
	auto windows = SDL_GetWindows(&count);
	SDL_WindowID second = 0;
	for (int x = 0; x < count; x++)
	{
		const auto id = SDL_GetWindowID(windows[x]);
		if (id != window->id() && sdl.getWindowForId(id))
			second = id;
	}
	SDL_free(windows);
	if (!expect(second != 0, "second multimon window created"))
		return false;
	monitor.orig_screen = physical;
	monitor.x = -pixels.w;
	monitor.y = 120;
	window->setMonitor(monitor);
	auto secondMonitor = sdl.getWindowForId(second)->monitor(false);
	secondMonitor.orig_screen = removed;
	sdl.getWindowForId(second)->setMonitor(secondMonitor);
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(false).width == window->rect().w &&
	            window->monitor(false).height == window->rect().h &&
	            window->monitor(false).x == monitor.x &&
	            window->monitor(false).y == monitor.y &&
	            window->monitor(false).attributes.desktopScaleFactor == 175 &&
	            window->monitor(false).attributes.deviceScaleFactor == 140,
	            "owning multimon window uses drawable pixels while retaining origin and scaling") ||
	    !expect(sdl.removeDisplayWindow(removed) && sdl.getWindowForId(second) == nullptr,
	            "removed display window released before fallback sizing"))
		return false;

	// Recreate another owned window (also works on the one-display dummy driver).
	// Distinct origins and DPI catch accidentally promoting each window to primary.
	monitor = window->monitor(false);
	monitor.orig_screen = removed;
	window->setMonitor(monitor);
	if (!sdl.addDisplayWindow(physical))
		return false;
	windows = SDL_GetWindows(&count);
	for (int x = 0; x < count; x++)
	{
		const auto id = SDL_GetWindowID(windows[x]);
		if (id != window->id() && sdl.getWindowForId(id))
			second = id;
	}
	SDL_free(windows);
	auto other = sdl.getWindowForId(second);
	if (!other)
		return false;
	std::ignore = SDL_SyncWindow(other->window());
	monitor.orig_screen = physical;
	monitor.is_primary = true;
	window->setMonitor(monitor);
	secondMonitor = other->monitor(false);
	secondMonitor.x = 0;
	secondMonitor.y = 0;
	secondMonitor.is_primary = false;
	secondMonitor.width = other->rect().w + 32;
	secondMonitor.height = other->rect().h + 66;
	secondMonitor.attributes.desktopScaleFactor = 100;
	secondMonitor.attributes.deviceScaleFactor = 100;
	other->setMonitor(secondMonitor);
	resized.windowID = other->id();
	const auto beforeMultimon = layoutCalls;
	if (!disp.handleEvent(resized))
		return false;
	disp.service(activated + 1000);
	if (!expect(layoutCalls == beforeMultimon + 1 && sentLayouts.size() == 2,
	            "pixel-size event sends both multimon layout entries") ||
	    !expect(window->monitor(false).x == monitor.x && window->monitor(false).y == monitor.y &&
	                other->monitor(false).x == secondMonitor.x && other->monitor(false).y == secondMonitor.y &&
	                other->monitor(false).width == other->rect().w &&
	                other->monitor(false).height == other->rect().h,
	            "each owned fullscreen window retains its origin and uses its own drawable"))
		return false;
	for (const auto& layout : sentLayouts)
	{
		const bool primary = layout.Flags == DISPLAY_CONTROL_MONITOR_PRIMARY;
		if (!expect(layout.Width == static_cast<UINT32>(primary ? window->rect().w : other->rect().w) &&
		                layout.Height == static_cast<UINT32>(primary ? window->rect().h : other->rect().h) &&
		                layout.DesktopScaleFactor == (primary ? 175u : 100u) &&
		                layout.DeviceScaleFactor == (primary ? 140u : 100u) &&
		                layout.Left == (primary ? 0 : pixels.w) &&
		                layout.Top == (primary ? 0 : -120),
		            "wire layout preserves relative monitor positions, primary and per-monitor DPI"))
			return false;
	}
	if (!disp.handleEvent(resized))
		return false;
	disp.service(activated + 2000);
	if (!expect(layoutCalls == beforeMultimon + 1, "duplicate multimon size events are deduplicated"))
		return false;

	// A display below a fullscreen window shorter than its display (the notch) must stay
	// adjacent to it; the origins still come from the full display bounds.
	auto top = window->monitor(false);
	top.x = 0;
	top.y = 0;
	top.is_primary = true;
	top.height = window->rect().h + 66;
	window->setMonitor(top);
	auto below = other->monitor(false);
	below.x = 0;
	below.y = top.height;
	below.is_primary = false;
	other->setMonitor(below);
	resized.windowID = window->id();
	const auto beforeStacked = layoutCalls;
	if (!disp.handleEvent(resized))
		return false;
	disp.service(activated + 3000);
	if (!expect(layoutCalls == beforeStacked + 1 && sentLayouts.size() == 2,
	            "shortening a stacked multimon window sends a layout") ||
	    !expect(window->monitor(false).height == window->rect().h &&
	                other->monitor(false).y == below.y,
	            "windows keep the display origins while the layout is compacted"))
		return false;
	for (const auto& layout : sentLayouts)
	{
		const bool primary = layout.Flags == DISPLAY_CONTROL_MONITOR_PRIMARY;
		if (!expect(layout.Left == 0 &&
		                layout.Top == (primary ? 0 : window->rect().h) &&
		                layout.Height == static_cast<UINT32>(primary ? window->rect().h : other->rect().h),
		            "the display below the shortened one stays adjacent in the wire layout"))
			return false;
	}
	secondMonitor = other->monitor(false);
	secondMonitor.orig_screen = removed;
	other->setMonitor(secondMonitor);
	if (!sdl.removeDisplayWindow(removed))
		return false;

	std::ignore = SDL_SyncWindow(window->window());
	const auto fallbackPixels = window->rect();
	if (!expect(sdl.updateWindow(window->id()) &&
	            window->monitor(false).width == fallbackPixels.w &&
	            window->monitor(false).height == fallbackPixels.h &&
	            window->monitor(false).attributes.physicalWidth == monitor.attributes.physicalWidth &&
	            window->monitor(false).attributes.physicalHeight == monitor.attributes.physicalHeight &&
	            window->monitor(false).attributes.desktopScaleFactor == 175 &&
	            window->monitor(false).attributes.deviceScaleFactor == 140,
	            "single remaining fullscreen multimon window uses drawable pixels and retains DPI"))
		return false;
	// Replug/layout changes refresh cached multimon dimensions and restore DPI.
	SdlMonitorScaleOverrides scales{ { physical, { physical, 175, 180 } } };
	if (!sdl.setHotplugMonitorSelection({ physical }, scales))
		return false;
	SDL_DisplayEvent changed{};
	changed.type = SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED;
	changed.displayID = physical;
	const auto before = layoutCalls;
	// Continue the simulated clock after the activation send.
	const UINT64 now = activated + 4000;
	if (!disp.handleEvent(changed))
		return false;
	disp.service(now);
	if (!expect(layoutCalls == before, "dock event bursts wait for stable geometry"))
		return false;
	disp.service(now + 500);
	const auto fresh = window->rect();
	if (!expect(layoutCalls == before + 1 &&
	                sentLayouts.front().Width == static_cast<UINT32>(fresh.w) &&
	                sentLayouts.front().Height == static_cast<UINT32>(fresh.h) &&
	                sentLayouts.front().DesktopScaleFactor == 175 &&
	                sentLayouts.front().DeviceScaleFactor == 180,
	            "settled hotplug sends fullscreen fallback drawable dimensions with saved scaling"))
		return false;
	disp.service(now + 1500);
	if (!expect(layoutCalls == before + 1, "settling retries deduplicate unchanged layouts"))
		return false;
	changed.type = SDL_EVENT_DISPLAY_ADDED;
	if (!disp.handleEvent(changed))
		return false;
	disp.service(now + 1600);
	disp.service(now + 2100);
	if (!expect(layoutCalls == before + 2,
	            "reconnection triggers rescaling even when final layout matches the previous one"))
		return false;

#if SDL_VERSION_ATLEAST(3, 3, 2)
	// Focus/work-area notifications must refresh inventory without reconfiguring
	// the server when the effective remote layout has not changed.
	SDL_DisplayEvent bounds{};
	bounds.type = SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED;
	bounds.displayID = physical;
	const auto beforeBounds = layoutCalls;
	if (!disp.handleEvent(bounds))
		return false;
	disp.service(now + 2200);
	if (!disp.handleEvent(bounds))
		return false;
	disp.service(now + 2300);
	disp.service(now + 2799);
	if (!expect(layoutCalls == beforeBounds, "usable-bounds bursts retain the settling delay"))
		return false;
	for (UINT64 offset = 2800; offset <= 7800; offset += 1000)
	{
		disp.service(now + offset);
		if (!expect(layoutCalls == beforeBounds,
		            "unchanged usable bounds and settling retries do not resend the layout"))
			return false;
	}

	// A real effective-layout change must still be discovered by this event.
	const SdlMonitorScaleOverrides newScales{ { physical, { physical, 150, 140 } } };
	if (!sdl.setHotplugMonitorSelection({ physical }, newScales) || !disp.handleEvent(bounds))
		return false;
	disp.service(now + 8000);
	if (!expect(layoutCalls == beforeBounds, "changed usable bounds still wait for settling"))
		return false;
	disp.service(now + 8500);
	if (!expect(layoutCalls == beforeBounds + 1 && sentLayouts.size() == 1 &&
	                sentLayouts.front().Width == static_cast<UINT32>(fresh.w) &&
	                sentLayouts.front().Height == static_cast<UINT32>(fresh.h) &&
	                sentLayouts.front().DesktopScaleFactor == 150 &&
	                sentLayouts.front().DeviceScaleFactor == 140,
	            "usable-bounds refresh sends changed effective scaling with drawable dimensions"))
		return false;
	disp.service(now + 9500);
	if (!expect(layoutCalls == beforeBounds + 1, "changed usable bounds send only one layout"))
		return false;

	// Test both queue orders, and a bounds notification after the force request
	// has already been consumed by service. Neither may cancel reconnect rescaling.
	UINT64 burstTime = now + 10000;
	for (const bool reconnectFirst : { false, true })
	{
		const auto beforeBurst = layoutCalls;
		if (!disp.handleEvent(reconnectFirst ? changed : bounds) ||
		    !disp.handleEvent(reconnectFirst ? bounds : changed))
			return false;
		disp.service(burstTime);
		if (!disp.handleEvent(bounds))
			return false;
		disp.service(burstTime + 100);
		disp.service(burstTime + 599);
		if (!expect(layoutCalls == beforeBurst, "mixed reconnect/bounds bursts wait for settling"))
			return false;
		disp.service(burstTime + 600);
		if (!expect(layoutCalls == beforeBurst + 1 &&
		                sentLayouts.front().DesktopScaleFactor == 150 &&
		                sentLayouts.front().DeviceScaleFactor == 140,
		            "usable-bounds events preserve forced same-layout reconnect rescaling"))
			return false;
		disp.service(burstTime + 1600);
		if (!expect(layoutCalls == beforeBurst + 1, "mixed-burst settling retries deduplicate layouts"))
			return false;
		burstTime += 3000;
	}
	const auto beforeActivation = layoutCalls;
	if (!expect(channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK &&
	                disp.handleEvent(bounds),
	            "display-channel reactivation can coalesce with a work-area notification"))
		return false;
	disp.service(burstTime);
	disp.service(burstTime + 500);
	if (!expect(layoutCalls == beforeActivation + 1,
	            "usable-bounds events preserve forced same-layout channel activation"))
		return false;

	// With complete connect-time monitor data, the first channel of a connection treats that
	// layout as sent: Windows applies it itself, and a late identical PDU would only cause a
	// second display reconfiguration (sheltie-app/sheltie-macos#24).
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasMonitorAttributes, TRUE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_NegotiationFlags, 0x01))
		return false;
	// The monitor block is only written for more than one monitor, so add a second one. The
	// server desktop is the bounding box of both.
	const auto setTwoMonitors = [&]()
	{
		const auto first = static_cast<const rdpMonitor*>(
		    freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 0));
		if (!first)
			return false;
		rdpMonitor pair[2] = { *first, *first };
		pair[0].x = 0;
		pair[0].y = 0;
		pair[0].is_primary = TRUE;
		pair[1].x = pair[0].width;
		pair[1].y = 0;
		pair[1].width = 800;
		pair[1].height = 600;
		pair[1].is_primary = FALSE;
		pair[1].attributes.desktopScaleFactor = 100;
		pair[1].attributes.deviceScaleFactor = 100;
		return freerdp_settings_set_monitor_def_array_sorted(settings, pair, 2) &&
		       freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
		                                   static_cast<UINT32>(pair[0].width + 800)) &&
		       freerdp_settings_set_uint32(
		           settings, FreeRDP_DesktopHeight,
		           static_cast<UINT32>(std::max<INT32>(pair[0].height, 600)));
	};
	const auto connectState = [&]()
	{
		StateChangedEventArgs state = {};
		EventArgsInit(&state, "TestSDLFullscreenResize");
		state.oldState = CONNECTION_STATE_NEGO;
		state.newState = CONNECTION_STATE_MCS_CREATE_REQUEST;
		return PubSub_OnStateChanged(instance->context->pubSub, instance->context, &state) >= 0;
	};
	// Let pending topology refreshes from the previous steps finish first: they rebuild the
	// monitor definitions from the windows.
	burstTime += 3000;
	for (UINT64 offset = 0; offset <= 10000; offset += 1000)
		disp.service(burstTime + offset);
	burstTime += 13000;
	const auto beforeSeed = layoutCalls;
	if (!expect(setTwoMonitors() && connectState() &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "activation after a connect with monitor data succeeds"))
		return false;
	for (const UINT64 offset : { 0, 500, 1500, 2500 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeSeed,
	            "an unchanged connect-time layout is not sent again after activation"))
		return false;

	// The server may accept another desktop than the client data asked for. The connect-time
	// layout then is not what it applied, so the current one is sent.
	burstTime += 3000;
	const auto beforeNegotiatedMulti = layoutCalls;
	if (!expect(connectState() &&
	                freerdp_settings_set_uint32(
	                    settings, FreeRDP_DesktopWidth,
	                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) - 200) &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "multimon activation after the server chose a smaller desktop succeeds"))
		return false;
	for (const UINT64 offset : { 0, 600, 1600 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeNegotiatedMulti + 1 && sentLayouts.size() == 2,
	            "a multimon layout is sent when the server desktop differs from it"))
		return false;
	if (!setTwoMonitors())
		return false;

	burstTime += 3000;
	const auto beforeChanged = layoutCalls;
	if (!expect(connectState(), "second connect records its monitor data"))
		return false;
	// Change the effective scaling after the connect, the way a display change would.
	const SdlMonitorScaleOverrides laterScales{ { physical, { physical, 175, 140 } } };
	if (!sdl.setHotplugMonitorSelection({ physical }, laterScales) || !disp.handleEvent(bounds) ||
	    !expect(channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "activation after the layout changed since connect succeeds"))
		return false;
	for (const UINT64 offset : { 0, 600, 1600 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeChanged + 1 && sentLayouts.front().DesktopScaleFactor == 175,
	            "a layout that changed since connect is still sent, once"))
		return false;

	burstTime += 3000;
	const auto beforeReopen = layoutCalls;
	if (!expect(channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "channel reopened without a new connect succeeds"))
		return false;
	disp.service(burstTime);
	disp.service(burstTime + 500);
	if (!expect(layoutCalls == beforeReopen + 1,
	            "a reopened channel without new connect data forces the current layout"))
		return false;

	// Without multimon, the client core data describes the single monitor. A layout equal to it
	// is not sent after activation; one that differs from it (for example a fullscreen window
	// shorter than the display on a notched Mac) still is.
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE) || !sdl.updateWindowList())
		return false;
	const auto single = static_cast<const rdpMonitor*>(
	    freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 0));
	if (!expect(single && freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) == 1,
	            "single-monitor session has one monitor definition"))
		return false;
	const auto singleWidth = static_cast<UINT32>(single->width);
	const auto singleHeight = static_cast<UINT32>(single->height);
	const auto setCoreData = [&](UINT32 width, UINT32 height)
	{
		return freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) &&
		       freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height) &&
		       freerdp_settings_set_uint32(settings, FreeRDP_DesktopPhysicalWidth,
		                                   single->attributes.physicalWidth) &&
		       freerdp_settings_set_uint32(settings, FreeRDP_DesktopPhysicalHeight,
		                                   single->attributes.physicalHeight) &&
		       freerdp_settings_set_uint16(settings, FreeRDP_DesktopOrientation,
		                                   ORIENTATION_LANDSCAPE) &&
		       freerdp_settings_set_uint32(settings, FreeRDP_DesktopScaleFactor,
		                                   single->attributes.desktopScaleFactor) &&
		       freerdp_settings_set_uint32(settings, FreeRDP_DeviceScaleFactor,
		                                   single->attributes.deviceScaleFactor);
	};

	burstTime += 3000;
	const auto beforeSingle = layoutCalls;
	if (!expect(setCoreData(singleWidth, singleHeight) && connectState() &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "single-monitor activation after connect succeeds"))
		return false;
	for (const UINT64 offset : { 0, 500, 1500, 2500 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeSingle,
	            "an unchanged single-monitor desktop is not sent again after activation"))
		return false;

	burstTime += 3000;
	const auto beforeSingleChanged = layoutCalls;
	if (!expect(setCoreData(singleWidth, singleHeight + 66) && connectState() &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "single-monitor activation after a larger connect-time desktop succeeds"))
		return false;
	for (const UINT64 offset : { 0, 600, 1600 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeSingleChanged + 1 && sentLayouts.size() == 1 &&
	                sentLayouts.front().Height == singleHeight,
	            "a single-monitor desktop that differs from the connect data is sent, once"))
		return false;

	// The server may accept another desktop than the core data asked for
	// (rdp_apply_bitmap_capability_set()). The requested one must not count as sent then.
	burstTime += 3000;
	const auto beforeNegotiated = layoutCalls;
	if (!expect(setCoreData(singleWidth, singleHeight) && connectState() &&
	                freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 800) &&
	                freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 600) &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "single-monitor activation after the server chose another desktop succeeds"))
		return false;
	for (const UINT64 offset : { 0, 600, 1600 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeNegotiated + 1 && sentLayouts.size() == 1 &&
	                sentLayouts.front().Width == singleWidth &&
	                sentLayouts.front().Height == singleHeight,
	            "the drawable is sent when the server desktop differs from the request"))
		return false;

	// Multimon with a single monitor writes no monitor block: the core data carries the
	// geometry, so that is what counts as sent.
	if (!freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE))
		return false;
	burstTime += 3000;
	const auto beforeOneMulti = layoutCalls;
	if (!expect(setCoreData(singleWidth, singleHeight) && connectState() &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "one-monitor multimon activation succeeds"))
		return false;
	for (const UINT64 offset : { 0, 500, 1500, 2500 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeOneMulti,
	            "a one-monitor multimon layout equal to the core data is not sent again"))
		return false;

	burstTime += 3000;
	const auto beforeOneMultiChanged = layoutCalls;
	// Same size, so only the source of the connect-time data decides: the monitor definition
	// was never written, the core data's scale was.
	const auto monitorScale = single->attributes.desktopScaleFactor;
	if (!expect(setCoreData(singleWidth, singleHeight) &&
	                freerdp_settings_set_uint32(settings, FreeRDP_DesktopScaleFactor,
	                                            monitorScale + 25) &&
	                connectState() &&
	                channel.DisplayControlCaps(&channel, 1, 8192, 8192) == CHANNEL_RC_OK,
	            "one-monitor multimon activation with other core data succeeds"))
		return false;
	for (const UINT64 offset : { 0, 600, 1600 })
		disp.service(burstTime + offset);
	if (!expect(layoutCalls == beforeOneMultiChanged + 1 && sentLayouts.size() == 1 &&
	                sentLayouts.front().DesktopScaleFactor == monitorScale,
	            "a one-monitor multimon monitor that differs from the core data is sent"))
		return false;
#endif
	return disp.uninit(&channel);
}

int main(int argc, char**)
{
	if (argc == 1)
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1;
	sdl_dialogs_init();
	const bool success = run();
	sdl_dialogs_uninit();
	SDL_Quit();
	if (success)
		puts("PASS fullscreen drawable dimensions, delayed activation, per-window multimon layouts, stacked adjacency, fallback, scaling, and migration geometry");
	return success ? 0 : 1;
}
