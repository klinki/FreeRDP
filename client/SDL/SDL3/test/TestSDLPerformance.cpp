/** Performance accounting and remote-frame regressions. Apache-2.0. */
#include "sdl_window.hpp"
#include "dialogs/sdl_dialogs.hpp"
#include <freerdp/freerdp.h>
#include <freerdp/performance.h>
extern "C"
{
#include "../../../../libfreerdp/core/performance.h"
}
#include <winpr/sysinfo.h>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>
static bool expect(bool ok, const char* msg)
{
	if (!ok)
		fprintf(stderr, "FAIL: %s\n", msg);
	return ok;
}
static rdpPerformanceSnapshot snapshot(rdpContext* ctx)
{
	rdpPerformanceSnapshot s{};
	s.size = sizeof(s);
	s.version = 1;
	if (!freerdp_performance_get_snapshot(ctx, &s))
		fprintf(stderr, "snapshot failed\n");
	return s;
}
static bool run()
{
	std::unique_ptr<freerdp, decltype(&freerdp_free)> instance(freerdp_new(), freerdp_free);
	if (!instance || !freerdp_context_new(instance.get()))
		return false;
	struct Guard
	{
		freerdp* p;
		~Guard()
		{
			freerdp_context_free(p);
		}
	} guard{ instance.get() };
	auto ctx = instance->context;
	performance_account(ctx, FALSE, FALSE, 100);
	if (!expect(snapshot(ctx).tcpInBytes == 0, "disabled accounting stays off"))
		return false;
	if (!freerdp_performance_set_enabled(ctx, TRUE))
		return false;
	performance_account(ctx, FALSE, FALSE, 17);
	performance_account(ctx, FALSE, TRUE, 11);
	performance_account(ctx, TRUE, FALSE, 23);
	performance_account(ctx, TRUE, TRUE, 29);
	auto s = snapshot(ctx);
	if (!expect(s.tcpInBytes == 17 && s.tcpOutBytes == 11 && s.udpInBytes == 23 &&
	                s.udpOutBytes == 29,
	            "TCP and UDP bytes are separate"))
		return false;
	const auto epoch = s.epoch;
	std::vector<std::thread> workers;
	for (int i = 0; i < 4; i++)
		workers.emplace_back(
		    [ctx]
		    {
			    for (int j = 0; j < 1000; j++)
				    performance_account(ctx, FALSE, TRUE, 1);
		    });
	for (auto& worker : workers)
		worker.join();
	if (!expect(snapshot(ctx).tcpOutBytes == 4011, "concurrent accounting is exact"))
		return false;
	performance_udp_connected(ctx, TRUE);
	performance_rtt(ctx, TRUE, 42);
	s = snapshot(ctx);
	if (!expect(s.rttValid && s.rttMilliseconds == 42 &&
	                s.rttSource == FREERDP_PERFORMANCE_RTT_RDP &&
	                s.rttTransport == FREERDP_PERFORMANCE_UDP,
	            "UDP RTT source retained"))
		return false;
	performance_rtt_at(ctx, TRUE, 99, GetTickCount64() - 16000);
	if (!expect(!snapshot(ctx).rttValid, "stale RTT is unavailable"))
		return false;
	performance_rtt(ctx, FALSE, 13);
	if (!expect(snapshot(ctx).rttMilliseconds == 13 &&
	                snapshot(ctx).rttTransport == FREERDP_PERFORMANCE_TCP,
	            "fresh TCP report follows stale UDP"))
		return false;
	performance_udp_connected(ctx, FALSE);
	freerdp_performance_set_enabled(ctx, FALSE);
	freerdp_performance_set_enabled(ctx, TRUE);
	s = snapshot(ctx);
	if (!expect(s.epoch > epoch && s.tcpOutBytes == 0 && s.udpInBytes == 0,
	            "enable changes reset baselines"))
		return false;
	rdpPerformanceSnapshot invalid{};
	invalid.size = 1;
	invalid.version = 1;
	if (!expect(!freerdp_performance_get_snapshot(ctx, &invalid),
	            "sized API rejects short snapshots"))
		return false;
	int count = 0;
	auto ids = SDL_GetDisplays(&count);
	if (!ids || count < 1)
		return false;
	const auto id = ids[0];
	SDL_free(ids);
	auto window = SdlWindow::create(id, "Performance regression", SDL_WINDOW_HIDDEN, 320, 240);
	auto surface = SDL_CreateSurface(320, 240, SDL_PIXELFORMAT_BGRA32);
	if (!surface || !window.renderer())
		return false;
	SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGBA(surface, 30, 80, 140, 255));
	auto& live = window.liveMetrics();
	live.setMemoryEnabled(true);
	live.beginFrame(320 * 240);
	const bool rendered =
	    window.drawRects(surface, { 0, 0 }, {}) && window.updateSurface(false, false, { -1, -1 });
	live.endFrame();
	live.setMemoryEnabled(false);
	auto frame = window.takeLiveSnapshot(SDL_GetTicksNS());
	if (!expect(rendered && frame.frames == 1 && frame.uploadedBytes > 0,
	            "new remote pixels produce one update"))
		return false;
	window.setPerformanceOverlay(true, "Performance\nUpdates/s: 0 (idle)\nTraffic: 0 KiB/s\nRTT: "
	                                   "unavailable\n\nDetails                       Hide");
	window.updateSurface(false, false, { -1, -1 });
	if (!expect(window.takeLiveSnapshot(SDL_GetTicksNS()).frames == 0,
	            "overlay-only paint never raises updates/s"))
		return false;
	auto overlay = window.performanceOverlay();
	// Initial box is clamped to this small viewport; drag/header captures are local.
	SDL_MouseButtonEvent down{};
	down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	down.button = SDL_BUTTON_LEFT;
	SdlPerformanceOverlay::Action action;
	if (!expect(overlay->button(down, { 25, 75 }, action) && overlay->capturing(),
	            "overlay drag starts locally"))
		return false;
	overlay->motion({ 40, 90 });
	SDL_MouseButtonEvent up = down;
	up.type = SDL_EVENT_MOUSE_BUTTON_UP;
	if (!expect(overlay->button(up, { 40, 90 }, action) && !overlay->capturing(),
	            "overlay drag ends locally"))
		return false;
	SDL_DestroySurface(surface);
	return true;
}
static bool benchmark()
{
	int count = 0;
	auto ids = SDL_GetDisplays(&count);
	if (!ids || !count)
		return false;
	const auto id = ids[0];
	SDL_free(ids);
	auto window = SdlWindow::create(id, "Performance workload", SDL_WINDOW_HIDDEN, 1280, 720);
	auto surface = SDL_CreateSurface(1280, 720, SDL_PIXELFORMAT_BGRA32);
	if (!surface)
		return false;
	SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGBA(surface, 32, 64, 128, 255));
	for (const bool enabled : { false, true })
	{
		window.setPerformanceOverlay(enabled,
		                             "Performance\nUpdates/s: 60\nReceive: 1.5 MiB/s\nRTT (TCP "
		                             "estimate): 12 ms\n\nDetails                       Hide");
		std::vector<double> durations;
		for (int frame = 0; frame < 212; frame++)
		{
			std::vector<SDL_Rect> damage;
			// Repeat the same full / one-region / sparse-tile geometry used by the render fixtures.
			if (frame % 3 == 0)
				damage = { { 0, 0, 1280, 720 } };
			else if (frame % 3 == 1)
				damage = { { (frame * 17) % 1100, (frame * 13) % 600, 160, 100 } };
			else
				for (int tile = 0; tile < 8; tile++)
					damage.push_back({ (frame * 17 + tile * 127) % 1200,
					                   (frame * 11 + tile * 67) % 650, 64, 64 });
			auto& live = window.liveMetrics();
			live.setMemoryEnabled(enabled);
			live.beginFrame(1280 * 720);
			const auto begin = std::chrono::steady_clock::now();
			if (!window.drawRects(surface, { 0, 0 }, damage) ||
			    !window.updateSurface(false, false, { -1, -1 }))
				return false;
			live.endFrame();
			live.setMemoryEnabled(false);
			const double ms =
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
			        .count();
			if (frame >= 12)
				durations.push_back(ms);
			window.takeLiveSnapshot(SDL_GetTicksNS());
		}
		std::sort(durations.begin(), durations.end());
		printf("{\"backend\":\"SDL software "
		       "fixture\",\"monitoring\":%s,\"samples\":%zu,\"p50_ms\":%.6f,\"p95_ms\":%.6f}\n",
		       enabled ? "true" : "false", durations.size(), durations[durations.size() / 2],
		       durations[durations.size() * 95 / 100]);
	}
	SDL_DestroySurface(surface);
	return true;
}

int main(int argc, char**)
{
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1;
	sdl_dialogs_init();
	const bool ok = argc > 1 ? benchmark() : run();
	sdl_dialogs_uninit();
	SDL_Quit();
	if (ok)
		puts("PASS transport counters, RTT validity, epochs, remote-only update counts, and "
		     "overlay input");
	return ok ? 0 : 1;
}
