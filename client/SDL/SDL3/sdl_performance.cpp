/** Live metrics orchestration, UI thread only. Licensed under Apache-2.0. */
#include "sdl_context.hpp"
#include "sdl_launcher.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
namespace
{
	void n(WINPR_JSON* o, const char* key, double v)
	{
		if (std::isfinite(v))
			WINPR_JSON_AddNumberToObject(o, key, v);
		else
			WINPR_JSON_AddNullToObject(o, key);
	}
	void txt(WINPR_JSON* o, const char* key, const char* v)
	{
		WINPR_JSON_AddStringToObject(o, key, v ? v : "");
	}
	std::string rate(double value)
	{
		char out[64];
		if (!std::isfinite(value))
			return "unavailable";
		if (value >= 1024 * 1024)
			std::snprintf(out, sizeof(out), "%.2f MiB/s", value / 1024 / 1024);
		else
			std::snprintf(out, sizeof(out), "%.1f KiB/s", value / 1024);
		return out;
	}
}
void SdlContext::servicePerformance()
{
#ifdef WITH_SDL_LAUNCHER_BRIDGE
	auto bridge = SdlLauncher::active();
	if (!bridge || !bridge->performanceAvailable())
		return;
	const auto now = SDL_GetTicksNS();
	const auto mask = bridge->performanceMask();
	const auto generation = bridge->performanceGeneration();
	if (mask != _performanceAppliedMask || generation != _performanceAppliedGeneration)
	{
		const bool wasEnabled = _performanceEnabled;
		_performanceAppliedMask = mask;
		_performanceAppliedGeneration = generation;
		_performanceEnabled = (mask & 1) != 0;
		_performanceOverlayVisible = (mask & 2) != 0;
		std::ignore = freerdp_performance_set_enabled(context(), _performanceEnabled);
		if (wasEnabled != _performanceEnabled)
		{
			_performanceLastNs = now;
			_performancePrevious = {};
			_performancePrevious.size = sizeof(_performancePrevious);
			_performancePrevious.version = FREERDP_PERFORMANCE_VERSION;
			std::ignore = freerdp_performance_get_snapshot(context(), &_performancePrevious);
			_performanceQueue = {};
			for (auto& entry : _windows)
				std::ignore = entry.second.takeLiveSnapshot(now);
		}
		for (auto& entry : _windows)
			entry.second.setPerformanceOverlay(
			    _performanceOverlayVisible,
			    "Performance\nStarting…\n\n\nDetails                       Hide");
		std::ignore = redrawWindows();
	}
	if (!_performanceEnabled || now - _performanceLastNs < 1000000000ULL)
		return;
	const double elapsed = static_cast<double>(now - _performanceLastNs) / 1e9;
	rdpPerformanceSnapshot network{};
	network.size = sizeof(network);
	network.version = FREERDP_PERFORMANCE_VERSION;
	if (!freerdp_performance_get_snapshot(context(), &network))
		return;
	const bool reset = network.epoch != _performancePrevious.epoch;
	const bool connected = isConnected();
	auto delta = [&](UINT64 value, UINT64 previous)
	{
		return !reset && connected && value >= previous
		           ? static_cast<double>(value - previous) / elapsed
		           : std::numeric_limits<double>::quiet_NaN();
	};
	const double tcpRx = delta(network.tcpInBytes, _performancePrevious.tcpInBytes),
	             tcpTx = delta(network.tcpOutBytes, _performancePrevious.tcpOutBytes);
	const double udpRx = delta(network.udpInBytes, _performancePrevious.udpInBytes),
	             udpTx = delta(network.udpOutBytes, _performancePrevious.udpOutBytes);
	auto event = bridge->performanceMessage("performance_sample");
	auto data = WINPR_JSON_AddObjectToObject(event.get(), "performance");
	n(data, "version", 1);
	n(data, "generation", generation);
	n(data, "sequence", ++_performanceSequence);
	n(data, "epoch", network.epoch);
	n(data, "timestamp", static_cast<double>(now) / 1e9);
	n(data, "intervalSeconds", elapsed);
	WINPR_JSON_AddBoolToObject(data, "connected", connected);
	WINPR_JSON_AddBoolToObject(data, "reset", reset);
	txt(data, "transport",
	    !connected                                                ? "Disconnected"
	    : (network.connectedTransports & FREERDP_PERFORMANCE_UDP) ? "TCP + UDP"
	                                                              : "TCP");
	auto traffic = WINPR_JSON_AddObjectToObject(data, "traffic");
	n(traffic, "tcpReceive", tcpRx);
	n(traffic, "tcpSend", tcpTx);
	n(traffic, "udpReceive", udpRx);
	n(traffic, "udpSend", udpTx);
	auto rtt = WINPR_JSON_AddObjectToObject(data, "rtt");
	n(rtt, "milliseconds",
	  connected && network.rttValid ? network.rttMilliseconds
	                                : std::numeric_limits<double>::quiet_NaN());
	txt(rtt, "source",
	    network.rttValid
	        ? (network.rttSource == FREERDP_PERFORMANCE_RTT_RDP ? "rdp" : "tcp_estimate")
	        : "unavailable");
	txt(rtt, "transport",
	    network.rttValid ? (network.rttTransport == FREERDP_PERFORMANCE_UDP ? "udp" : "tcp")
	                     : "none");
	n(rtt, "ageSeconds",
	  network.rttValid && network.rttSource == FREERDP_PERFORMANCE_RTT_RDP
	      ? network.rttAgeMilliseconds / 1000.0
	      : std::numeric_limits<double>::quiet_NaN());
	auto displays = WINPR_JSON_AddArrayToObject(data, "displays");
	for (auto& entry : _windows)
	{
		auto& window = entry.second;
		auto frame = window.takeLiveSnapshot(now);
		auto pixels = window.pixelViewport();
		if (pixels.w <= 0 || pixels.h <= 0 ||
		    !SDL_GetDisplayName(window.monitor(false).orig_screen))
			continue;
		auto display = WINPR_JSON_CreateObject();
		n(display, "id", window.monitor(false).orig_screen);
		n(display, "windowID", window.id());
		txt(display, "name", SDL_GetDisplayName(window.monitor(false).orig_screen));
		n(display, "width", pixels.w);
		n(display, "height", pixels.h);
		n(display, "updatesPerSecond",
		  connected && !reset ? frame.frames / elapsed : std::numeric_limits<double>::quiet_NaN());
		auto render = WINPR_JSON_AddObjectToObject(display, "render");
		n(render, "redrawAverageMs", frame.redrawAverageMs);
		n(render, "redrawP95Ms", frame.redrawP95Ms);
		n(render, "uploadAverageMs", frame.uploadAverageMs);
		n(render, "drawAverageMs", frame.drawAverageMs);
		n(render, "presentAverageMs", frame.presentAverageMs);
		n(render, "uploadedBytes", frame.uploadedBytes);
		n(render, "presentCount", frame.frames);
		WINPR_JSON_AddItemToArray(displays, display);
		char update[64], latency[96];
		if (!connected)
			std::snprintf(update, sizeof(update), "Disconnected");
		else if (reset)
			std::snprintf(update, sizeof(update), "Starting…");
		else
			std::snprintf(update, sizeof(update), "Updates/s: %.1f%s", frame.frames / elapsed,
			              frame.frames ? "" : " (idle)");
		if (network.rttValid && connected)
			std::snprintf(latency, sizeof(latency), "RTT%s: %u ms",
			              network.rttSource == FREERDP_PERFORMANCE_RTT_TCP_ESTIMATE
			                  ? " (TCP estimate)"
			              : network.rttTransport == FREERDP_PERFORMANCE_UDP ? " (RDP UDP)"
			                                                                : " (RDP TCP)",
			              network.rttMilliseconds);
		else
			std::snprintf(latency, sizeof(latency), "RTT: unavailable");
		window.setPerformanceOverlay(_performanceOverlayVisible,
		                             std::string("Performance — drag to move\n") + update + "\n↓ " +
		                                 rate(tcpRx + udpRx) + "   ↑ " + rate(tcpTx + udpTx) +
		                                 "\n" + latency + "\n\nDetails                       Hide");
	}
	flushQueueMetrics(false);
	auto queue = WINPR_JSON_AddObjectToObject(data, "queue");
	n(queue, "averageWaitMs",
	  _performanceQueue.pops
	      ? static_cast<double>(_performanceQueue.queueWaitNs) / _performanceQueue.pops / 1e6
	      : std::numeric_limits<double>::quiet_NaN());
	n(queue, "updatesReceived", _performanceQueue.updateReceived);
	n(queue, "updatesActed", _performanceQueue.updateActed);
	n(queue, "motionsCoalesced", _performanceQueue.motionsCoalesced);
	n(queue, "collapsedEvents", _performanceQueue.collapsedEvents);
	_performanceQueue = {};
	_performancePrevious = network;
	_performanceLastNs = now;
	std::ignore = bridge->sendPerformance(std::move(event));
	// This only copies/presents retained pixels; it is not a remote frame.
	std::ignore = redrawWindows();
#endif
}
bool SdlContext::performanceButton(const SDL_MouseButtonEvent& event)
{
#ifdef WITH_SDL_LAUNCHER_BRIDGE
    if (_performanceCapture && !getWindowForId(_performanceCapture)) {
        _performanceCapture=0; _performanceOrphanCapture=true;
    }
    if (_performanceOrphanCapture) {
        if (event.type==SDL_EVENT_MOUSE_BUTTON_UP && event.button==SDL_BUTTON_LEFT) _performanceOrphanCapture=false;
        return true;
    }
	if (_topBarGestureOwner == TopBarGestureOwner::Remote)
		return false;
	const auto id = _performanceCapture ? _performanceCapture : event.windowID;
	auto window = getWindowForId(id);
	if (!window || !window->performanceOverlay())
	{
		_performanceCapture = 0;
		return false;
	}
	auto overlay = window->performanceOverlay();
	SdlPerformanceOverlay::Action action;
	if (!overlay->button(event, screenToPixel(id, { event.x, event.y }), action))
		return false;
	_performanceCapture = overlay->capturing() ? id : 0;
	if (auto bridge = SdlLauncher::active())
	{
		if (action == SdlPerformanceOverlay::Details)
			bridge->requestPerformanceDetails();
		if (action == SdlPerformanceOverlay::Hide)
			bridge->requestPerformanceOverlay(false);
	}
	std::ignore = redrawWindows();
	return true;
#else
	return false;
#endif
}
bool SdlContext::performanceMotion(const SDL_MouseMotionEvent& event)
{
#ifdef WITH_SDL_LAUNCHER_BRIDGE
    if (_performanceOrphanCapture) return true;
    if (_performanceCapture && !getWindowForId(_performanceCapture)) {
        _performanceCapture=0; _performanceOrphanCapture=true; return true;
    }
	if (!_performanceCapture)
		return false;
	auto window = getWindowForId(_performanceCapture);
	if (!window || !window->performanceOverlay())
	{
		_performanceCapture = 0;
		return false;
	}
	const bool handled = window->performanceOverlay()->motion(
	    screenToPixel(_performanceCapture, { event.x, event.y }));
	if (handled)
		std::ignore = redrawWindows();
	return handled;
#else
	return false;
#endif
}
