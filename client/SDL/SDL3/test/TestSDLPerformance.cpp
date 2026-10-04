/** Performance accounting and remote-frame regressions. Apache-2.0. */
#include "sdl_window.hpp"
#include "dialogs/sdl_dialogs.hpp"
#include <freerdp/freerdp.h>
#include <freerdp/performance.h>
extern "C"
{
#include "../../../../libfreerdp/core/performance.h"
#include "../../../../libfreerdp/core/rdpeudp.h"
}
#include <winpr/sysinfo.h>
#include <cstdio>
#include <climits>
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
	s.version = FREERDP_PERFORMANCE_VERSION;
	if (!freerdp_performance_get_snapshot(ctx, &s))
		fprintf(stderr, "snapshot failed\n");
	return s;
}
static bool diagnosticMetrics(rdpContext* ctx)
{
    const auto epoch=snapshot(ctx).epoch;
    for (UINT64 i=1;i<=20;i++) performance_decode_record(ctx,epoch,i*1000000,i!=20,"Fixture codec");
    auto s=snapshot(ctx);
    if (!expect(s.decodeCalls==20 && s.decodeFailures==1 && s.decodeWallNs==210000000 &&
                s.decodeRecentSampleCount==20 && s.decodeRecentP95Milliseconds==19,
                "decode durations, failures, and recent p95 are measured independently")) return false;
    rdpPerformanceSnapshot v2{}; v2.version=2; v2.size=FREERDP_PERFORMANCE_V2_SIZE;
    v2.udpSentDatagrams=123456;
    if (!expect(freerdp_performance_get_snapshot(ctx,&v2) && v2.udpSentDatagrams==123456 &&
                freerdp_performance_get_snapshot(ctx,&v2) && v2.udpSentDatagrams==123456,
                "version 2 snapshots never overwrite the new extension")) return false;
    for (UINT64 i=0;i<300;i++) performance_decode_record(ctx,epoch,123000,TRUE,"Fixture codec");
    s=snapshot(ctx);
    if (!expect(s.decodeCalls==320 && s.decodeRecentSampleCount==256 &&
                s.decodeRecentP95Milliseconds==0.123,
                "decode percentile storage remains bounded")) return false;
    auto old=performance_decode_begin(ctx);
    freerdp_performance_set_enabled(ctx,FALSE);
    if (!expect(!performance_decode_begin(ctx).startedNs,"disabled decode instrumentation reads no timer")) return false;
    freerdp_performance_set_enabled(ctx,TRUE);
    performance_decode_end(ctx,old,TRUE,"Old generation");
    if (!expect(snapshot(ctx).decodeCalls==0,"in-flight decode timing cannot cross enable epochs")) return false;
    const auto current=snapshot(ctx).epoch;
    std::vector<std::thread> workers;
    for(int i=0;i<4;i++) workers.emplace_back([ctx,current] {
        for(int j=0;j<1000;j++) performance_decode_record(ctx,current,1000,TRUE,"Concurrent");
    });
    for(auto& worker:workers) worker.join();
    s=snapshot(ctx);
    if (!expect(s.decodeCalls+s.decodeSamplesSkipped==4000 && s.decodeRecentSampleCount<=256,
                "contended timing drops observations instead of blocking decoding")) return false;
    std::unique_ptr<rdpUdpTransport,decltype(&rdpeudp_test_free)> udp(rdpeudp_test_new(),rdpeudp_test_free);
    if(!udp) return false;
    rdpeudp_test_set_context(udp.get(),ctx);
    if(!rdpeudp_test_seed_sent(udp.get(),100,100) || !rdpeudp_test_seed_sent(udp.get(),101,101)) return false;
    const BOOL received[4]={FALSE,FALSE,FALSE,FALSE};
    wStream* ack=rdpeudp_build_ackvec(100,received,4,FALSE,0,0);
    if(!ack) return false;
    RdpUdp2Layout layout{};
    layout.flags=RDPUDP2_FLAG_ACKVEC | RDPUDP2_FLAG_AOA; layout.logWindow=5; layout.hasAckvec=TRUE;
    layout.hasAoa=TRUE; layout.aoa=1; // Keep this control layout above the short-packet padding threshold.
    layout.ackvec=Stream_Buffer(ack); layout.ackvecLen=Stream_Length(ack);
    wStream* wire=rdpeudp2_encode_layout(&layout);
    Stream_Release(ack);
    if(!wire || !Stream_SetPosition(wire,Stream_Length(wire)) || !rdpeudp2_protect(wire,FALSE)) return false;
    const bool fed=rdpeudp_test_feed(udp.get(),Stream_Buffer(wire),Stream_Length(wire)) &&
                   rdpeudp_test_feed(udp.get(),Stream_Buffer(wire),Stream_Length(wire));
    Stream_Release(wire);
    RdpUdpStats stats{};
    const auto haveStats=rdpeudp_test_get_stats(udp.get(),&stats);
    const auto loss=snapshot(ctx).udpLossReports;
    if (!fed || !haveStats || stats.lostDetected!=2 || loss!=2)
        fprintf(stderr,"UDP fixture: fed=%d stats=%d lost=%llu collected=%llu received=%llu\n",
                fed,haveStats,(unsigned long long)stats.lostDetected,(unsigned long long)loss,
                (unsigned long long)stats.recvPackets);
    if (!expect(fed && haveStats && stats.lostDetected==2 && loss==2,
                "repeated negative ACK vectors count each outstanding transmission once")) return false;
    performance_udp_stats(ctx,10,15,3,0);
    s=snapshot(ctx);
    if (!expect(s.udpSentDatagrams==10 && s.udpReceivedDatagrams==15 && s.udpRetransmissions==3,
                "successful datagrams and retransmissions retain separate counters")) return false;
    freerdp_performance_set_enabled(ctx,FALSE);
    freerdp_performance_set_enabled(ctx,TRUE);
    if (!expect(snapshot(ctx).udpRetransmissions==0 && snapshot(ctx).decodeCalls==0,
                "additional counters reset with collection epochs")) return false;
    return true;
}
static bool pacingMetrics()
{
    struct Clock { UINT64 now=1; static UINT64 read(void* p) noexcept { return static_cast<Clock*>(p)->now; } } clock;
    SdlRenderMetrics live(1,1,&Clock::read,&clock,true);
    live.setMemoryEnabled(true);
    live.beginFrame(1); live.notePresent(1); live.endFrame();
    clock.now+=16000000;
    live.beginFrame(1); live.notePresent(1); live.endFrame();
    clock.now+=1000000000;
    live.beginFrame(1); live.notePresent(1); live.endFrame();
    auto s=live.takeMemorySnapshot(clock.now);
    if (!expect(s.frameIntervalCount==2 && s.frameIntervalAverageMs==508 &&
                s.frameIntervalP95Ms==1000 && s.frameIntervalMaxMs==1000 && s.renderStalls==0,
                "idle presentation gaps are visible but do not count as render stalls")) return false;
    live.resetMemoryBaseline(clock.now);
    clock.now+=2000000000;
    live.beginFrame(1); live.notePresent(1); live.endFrame();
    s=live.takeMemorySnapshot(clock.now);
    if (!expect(s.frameIntervalCount==0,"resume baseline excludes paused gaps")) return false;
    live.beginFrame(1); clock.now+=100000000; live.notePresent(1); live.endFrame();
    live.notePresentSkip();
    s=live.takeMemorySnapshot(clock.now);
    return expect(s.renderStalls==1 && s.presentFailures==1 && s.presentCalls==1,
                  "slow redraws and failed presents remain distinct");
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
	if (!expect(!snapshot(ctx).bandwidthValid, "missing bandwidth remains unavailable"))
		return false;
	performance_bandwidth_measurement(ctx, FALSE, 100, 1000000);
	s = snapshot(ctx);
	if (!expect(s.bandwidthValid && !s.bandwidthStale &&
				s.bandwidthKilobitsPerSecond == 80000.0 &&
				s.bandwidthSource == FREERDP_PERFORMANCE_BANDWIDTH_RDP_MEASUREMENT &&
				s.bandwidthTransport == FREERDP_PERFORMANCE_TCP,
				"measurement bytes and milliseconds convert to decimal kilobits/s"))
		return false;
	performance_bandwidth_measurement(ctx, FALSE, 0, 1000000);
	performance_bandwidth_measurement(ctx, FALSE, 100, 0);
	if (!expect(snapshot(ctx).bandwidthKilobitsPerSecond == 80000.0,
				"empty and zero-duration measurements do not overwrite estimates"))
		return false;
	performance_bandwidth_measurement(ctx, FALSE, 1, UINT32_MAX);
	if (!expect(snapshot(ctx).bandwidthKilobitsPerSecond == (double)UINT32_MAX * 8.0,
				"large bandwidth measurements do not overflow"))
		return false;
	performance_bandwidth(ctx, FALSE, 80000);
	performance_udp_connected(ctx, TRUE);
	performance_rtt(ctx, TRUE, 42);
	s = snapshot(ctx);
	if (!expect(s.rttValid && s.rttMilliseconds == 42 &&
	                s.rttSource == FREERDP_PERFORMANCE_RTT_RDP &&
	                s.rttTransport == FREERDP_PERFORMANCE_UDP,
	            "UDP RTT source retained"))
		return false;
	performance_bandwidth(ctx, TRUE, 42000);
	s = snapshot(ctx);
	if (!expect(s.bandwidthKilobitsPerSecond == 42000 && !s.bandwidthStale &&
				s.bandwidthSource == FREERDP_PERFORMANCE_BANDWIDTH_RDP_ESTIMATE &&
				s.bandwidthTransport == FREERDP_PERFORMANCE_UDP,
				"active UDP bandwidth report takes priority"))
		return false;
	performance_bandwidth_at(ctx, TRUE, 42000, FREERDP_PERFORMANCE_BANDWIDTH_RDP_ESTIMATE,
							 GetTickCount64() - 16000);
	if (!expect(snapshot(ctx).bandwidthKilobitsPerSecond == 80000 &&
				snapshot(ctx).bandwidthTransport == FREERDP_PERFORMANCE_TCP,
				"fresh TCP estimate follows stale UDP"))
		return false;
	performance_bandwidth_at(ctx, FALSE, 80000, FREERDP_PERFORMANCE_BANDWIDTH_RDP_ESTIMATE,
							 GetTickCount64() - 17000);
	s = snapshot(ctx);
	if (!expect(s.bandwidthValid && s.bandwidthStale && s.bandwidthAgeMilliseconds >= 16000 &&
				s.bandwidthTransport == FREERDP_PERFORMANCE_UDP,
				"last-known bandwidth retains its age and explicit stale status"))
		return false;
	rdpPerformanceSnapshot legacy{};
	legacy.size = FREERDP_PERFORMANCE_V1_SIZE;
	legacy.version = 1;
	legacy.bandwidthKilobitsPerSecond = 1234.0;
	if (!expect(freerdp_performance_get_snapshot(ctx, &legacy) &&
				legacy.size == FREERDP_PERFORMANCE_V1_SIZE && legacy.bandwidthKilobitsPerSecond == 1234.0 &&
				freerdp_performance_get_snapshot(ctx, &legacy) && legacy.bandwidthKilobitsPerSecond == 1234.0,
				"version 1 snapshots preserve extension bytes across repeated calls"))
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
	if (!expect(snapshot(ctx).bandwidthTransport == FREERDP_PERFORMANCE_TCP,
				"removed UDP path cannot supply bandwidth"))
		return false;
	performance_tcp_socket(ctx, INT_MAX);
	if (!expect(!snapshot(ctx).bandwidthValid, "reconnect clears previous bandwidth estimates"))
		return false;
	performance_tcp_socket(ctx, -1);
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
	if (!diagnosticMetrics(ctx) || !pacingMetrics()) return false;
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
	window.setPerformanceOverlay(true, "Performance\nFPS (remote): 0 (idle)\nTraffic: 0 KiB/s\nRTT: "
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
		                             "Performance\nFPS (remote): 60\nReceive: 1.5 MiB/s\nRTT (TCP "
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
