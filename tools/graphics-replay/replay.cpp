/* Offline diagnostic: actual FreeRDP GFX decoder and SDL monitor dispatch.
 * No transport connect, authentication, server, or remote input is used.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdl_context.hpp"
#include "sdl_types.hpp"

#include <freerdp/client/channels.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/codec/color.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>

// Cache allocation normally occurs during connection negotiation. The offline
// executable links these matching production cache objects to initialize GDI.
extern "C" rdpCache* cache_new(rdpContext*);
extern "C" void cache_free(rdpCache*);

namespace
{
using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> decodeErrors{0};
BOOL decodeLog(const wLogMessage* message)
{
	const char* text = message->TextString ? message->TextString : "";
	std::cerr << (message->PrefixString ? message->PrefixString : "") << text << '\n';
	// The production AVC handler intentionally ignores failed updates. Such a
	// run must never produce a successful replay benchmark or black-image pass.
	if (message->Level >= WLOG_ERROR || std::strstr(text, "ignoring update") ||
	    std::strstr(text, "decompress failed"))
		decodeErrors++;
	return TRUE;
}
void check(bool value, const std::string& what)
{
	if (!value)
		throw std::runtime_error(what + ": " + SDL_GetError());
}
uint64_t ns(Clock::duration elapsed)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}
double cpuSeconds()
{
	rusage usage{};
	check(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage");
	return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
	       (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
}
uint64_t readLE(const unsigned char* p, unsigned count)
{
	uint64_t value = 0;
	for (unsigned n = 0; n < count; n++)
		value |= uint64_t(p[n]) << (8 * n);
	return value;
}
uint64_t number(const std::string& text)
{
	check(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, "expected unsigned integer");
	return std::stoull(text);
}
uint64_t seconds(const std::string& text)
{
	const auto value = number(text);
	check(value <= UINT64_MAX / 1000000, "seconds overflow");
	return value * 1000000;
}
struct Message
{
	uint64_t us;
	std::vector<BYTE> bytes;
};
std::vector<Message> load(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	check(bool(input), "open replay");
	char magic[8]{};
	input.read(magic, 8);
	check(input && std::memcmp(magic, "FRGFXR01", 8) == 0, "replay magic");
	std::vector<Message> result;
	size_t total = 0;
	while (input.peek() != EOF)
	{
		unsigned char header[12]{};
		input.read(reinterpret_cast<char*>(header), sizeof(header));
		check(bool(input), "truncated replay header");
		const auto time = readLE(header, 8);
		const auto length = readLE(header + 8, 4);
		check(length > 0 && length <= 64 * 1024 * 1024, "replay record length");
		check(result.empty() || time >= result.back().us, "replay timestamps are not ordered");
		total += length;
		check(total <= 1024ULL * 1024 * 1024, "replay exceeds 1 GiB preload limit");
		result.push_back({ time, std::vector<BYTE>(length) });
		input.read(reinterpret_cast<char*>(result.back().bytes.data()), length);
		check(bool(input), "truncated replay payload");
	}
	check(!result.empty(), "empty replay");
	return result;
}

struct Host
{
	IDRDYNVC_ENTRY_POINTS entry{};
	IWTSVirtualChannelManager manager{};
	IWTSListener listener{};
	IWTSVirtualChannel channel{};
	IWTSPlugin* plugin = nullptr;
	IWTSListenerCallback* listenerCallback = nullptr;
	IWTSVirtualChannelCallback* callback = nullptr;
	rdpContext* context = nullptr;
	RdpgfxClientContext* gfx = nullptr;
	bool pipeline = false;
	static Host* active;

	explicit Host(rdpContext* ctx) : context(ctx)
	{
		active = this;
		entry.GetPlugin = [](IDRDYNVC_ENTRY_POINTS*, const char*) -> IWTSPlugin* { return nullptr; };
		entry.RegisterPlugin = [](IDRDYNVC_ENTRY_POINTS*, const char*, IWTSPlugin* plugin) -> UINT {
			active->plugin = plugin;
			return CHANNEL_RC_OK;
		};
		entry.GetRdpSettings = [](IDRDYNVC_ENTRY_POINTS*) { return active->context->settings; };
		entry.GetRdpContext = [](IDRDYNVC_ENTRY_POINTS*) { return active->context; };
		entry.GetPluginData = [](IDRDYNVC_ENTRY_POINTS*) -> const ADDIN_ARGV* { return nullptr; };
		manager.CreateListener = [](IWTSVirtualChannelManager*, const char*, ULONG,
		                            IWTSListenerCallback* cb, IWTSListener** out) -> UINT {
			active->listenerCallback = cb;
			*out = &active->listener;
			return CHANNEL_RC_OK;
		};
		manager.DestroyListener = [](IWTSVirtualChannelManager*, IWTSListener*) -> UINT { return CHANNEL_RC_OK; };
		channel.Write = [](IWTSVirtualChannel*, ULONG, const BYTE*, void*) -> UINT { return CHANNEL_RC_OK; };
		channel.Close = [](IWTSVirtualChannel*) -> UINT { return CHANNEL_RC_OK; };
	}
	void initialize()
	{
		auto entrypoint = reinterpret_cast<PDVC_PLUGIN_ENTRY>(freerdp_channels_load_static_addin_entry(
		    "rdpgfx", nullptr, nullptr, FREERDP_ADDIN_CHANNEL_DYNAMIC));
		check(entrypoint && entrypoint(&entry) == CHANNEL_RC_OK && plugin, "load graphics plugin");
		check(plugin->Initialize(plugin, &manager) == CHANNEL_RC_OK && listenerCallback, "initialize graphics plugin");
		gfx = static_cast<RdpgfxClientContext*>(plugin->pInterface);
		check(gfx && gdi_graphics_pipeline_init(context->gdi, gfx), "initialize graphics pipeline");
		pipeline = true;
		gfx->OnOpen = [](RdpgfxClientContext*, BOOL* caps, BOOL* acks) -> UINT {
			*caps = FALSE;
			*acks = FALSE;
			return CHANNEL_RC_OK;
		};
		BOOL accept = TRUE;
		check(listenerCallback->OnNewChannelConnection(listenerCallback, &channel, nullptr, &accept, &callback) == CHANNEL_RC_OK && callback, "open offline graphics channel");
		check(callback->OnOpen(callback) == CHANNEL_RC_OK, "graphics OnOpen");
	}
	~Host()
	{
		if (callback)
			(void)callback->OnClose(callback);
		if (pipeline)
			gdi_graphics_pipeline_uninit(context->gdi, gfx);
		if (plugin)
			(void)plugin->Terminated(plugin);
		active = nullptr;
	}
	void feed(const Message& message)
	{
		wStream stream{};
		auto s = Stream_StaticConstInit(&stream, message.bytes.data(), message.bytes.size());
		const UINT status = callback->OnDataReceived(callback, s);
		check(status == CHANNEL_RC_OK, "graphics message failed with code " + std::to_string(status));
	}
};
Host* Host::active = nullptr;

class ReplayWindow : public SdlWindow
{
  public:
	ReplayWindow(SDL_DisplayID display, const SDL_Rect& rect)
	    : SdlWindow(display, "FreeRDP offline graphics replay", rect, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN)
	{
	}
};
} // namespace

// Friend access is injected ONLY into the diagnostic's generated header overlay.
// Production SdlContext/SdlWindow objects and their rendering code are unmodified.
class SdlGraphicsReplay
{
  public:
	static SdlGraphicsReplay* active;
	freerdp* instance = nullptr;
	std::unique_ptr<SdlContext> sdl;
	std::unique_ptr<Host> host;
	pcRdpgfxResetGraphics originalReset = nullptr;
	std::vector<SDL_Rect> monitors;
	std::vector<uint64_t> renderSamples;
	uint64_t paintCount = 0, verified = 0, currentUs = 0;
	uint64_t verifyEvery = 0, startUs = 0, stopUs = UINT64_MAX;
	bool measured = false;
	bool visible = false;
	bool quitRequested = false;
	bool callbackFailed = false;
	std::string callbackError;
	std::string rendererName;
	std::string saveFinal;

	SdlGraphicsReplay()
	{
		active = this;
		instance = freerdp_new();
		check(instance != nullptr, "freerdp_new");
		instance->ContextSize = sizeof(sdl_rdp_context);
		check(freerdp_context_new(instance), "freerdp_context_new");
		auto context = instance->context;
		auto settings = context->settings;
		check(freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 64) &&
		      freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 64) &&
		      freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE) &&
		      freerdp_settings_set_bool(settings, FreeRDP_SmartSizing, FALSE) &&
		      freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE) &&
		      freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE) &&
		      freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE), "offline settings");
		sdl = std::make_unique<SdlContext>(context);
		reinterpret_cast<sdl_rdp_context*>(context)->sdl = sdl.get();
		sdl->_sdlPixelFormat = SDL_PIXELFORMAT_BGRA32;
		sdl->_topBarVisible = false;
		sdl->_connected = true;
		context->cache = cache_new(context);
		check(context->cache != nullptr, "offline graphics caches");
		context->codecs = freerdp_client_codecs_new(
		    freerdp_settings_get_uint32(settings, FreeRDP_ThreadingFlags));
		check(context->codecs && freerdp_client_codecs_prepare(
		    context->codecs, freerdp_settings_get_codecs_flags(settings), 64, 64), "offline codecs");
		check(gdi_init(instance, PIXEL_FORMAT_BGRA32), "gdi_init");
		check(sdl->createPrimary(), "initial primary surface");
		context->update->BeginPaint = SdlContext::beginPaint;
		context->update->EndPaint = [](rdpContext*) -> BOOL {
			try { active->paint(); return TRUE; }
			catch (const std::exception& error) {
				active->callbackFailed = true;
				active->callbackError = error.what();
				return FALSE;
			}
		};
		context->update->DesktopResize = [](rdpContext* ctx) -> BOOL {
			if (!gdi_resize(ctx->gdi, freerdp_settings_get_uint32(ctx->settings, FreeRDP_DesktopWidth),
			                freerdp_settings_get_uint32(ctx->settings, FreeRDP_DesktopHeight)))
				return FALSE;
			return active->sdl->createPrimary();
		};
		host = std::make_unique<Host>(context);
		host->initialize();
		originalReset = host->gfx->ResetGraphics;
		host->gfx->ResetGraphics = [](RdpgfxClientContext* ctx, const RDPGFX_RESET_GRAPHICS_PDU* pdu) -> UINT {
			try {
				check(pdu->width && pdu->height && uint64_t(pdu->width) * pdu->height <= 64 * 1024 * 1024, "desktop size limit");
				const auto status = active->originalReset(ctx, pdu);
				if (status != CHANNEL_RC_OK) return status;
				active->resetWindows(*pdu);
				return CHANNEL_RC_OK;
			} catch (const std::exception& error) {
				active->callbackFailed = true;
				active->callbackError = error.what();
				return ERROR_INVALID_DATA;
			}
		};
	}
	~SdlGraphicsReplay()
	{
		host.reset();
		if (sdl)
		{
			sdl->_connected = false;
			sdl->_windows.clear();
			sdl->_primary.reset();
		}
		if (instance)
			gdi_free(instance);
		sdl.reset();
		if (instance)
		{
			cache_free(instance->context->cache);
			instance->context->cache = nullptr;
			freerdp_context_free(instance);
			freerdp_free(instance);
		}
		active = nullptr;
	}
	void resetWindows(const RDPGFX_RESET_GRAPHICS_PDU& pdu)
	{
		check(pdu.monitorCount > 0 && pdu.monitorCount <= 16, "monitor count");
		sdl->_windows.clear();
		monitors.clear();
		int64_t minX = 0, minY = 0;
		int displayCount = 0;
		std::unique_ptr<SDL_DisplayID, decltype(&SDL_free)> displays(
		    visible ? SDL_GetDisplays(&displayCount) : nullptr, SDL_free);
		check(!visible || (displays && displayCount > 0), "list playback displays");
		std::vector<SDL_DisplayID> usedDisplays;
		for (UINT32 n = 0; n < pdu.monitorCount; n++)
		{
			minX = std::min(minX, int64_t(pdu.monitorDefArray[n].left));
			minY = std::min(minY, int64_t(pdu.monitorDefArray[n].top));
		}
		for (UINT32 n = 0; n < pdu.monitorCount; n++)
		{
			const auto& m = pdu.monitorDefArray[n];
			const int64_t x = m.left - minX, y = m.top - minY;
			const int64_t width = int64_t(m.right) - m.left + 1, height = int64_t(m.bottom) - m.top + 1;
			check(x >= 0 && y >= 0 && width > 0 && height > 0 &&
			      x + width <= pdu.width && y + height <= pdu.height, "monitor bounds");
			SDL_Rect rect{ int(x), int(y), int(width), int(height) };
			auto display = SDL_GetPrimaryDisplay();
			if (visible)
			{
				// Prefer a matching physical screen; keep the captured pixel size so
				// showing the picture does not change clipping or verification.
				for (int i = 0; i < displayCount; i++)
				{
					const auto candidate = displays.get()[i];
					const auto mode = SDL_GetCurrentDisplayMode(candidate);
					if (mode && int(mode->w * mode->pixel_density) == rect.w &&
					    int(mode->h * mode->pixel_density) == rect.h &&
					    std::find(usedDisplays.begin(), usedDisplays.end(), candidate) == usedDisplays.end())
					{
						display = candidate;
						break;
					}
				}
				usedDisplays.push_back(display);
			}
			const int position = visible ? SDL_WINDOWPOS_CENTERED_DISPLAY(display) : 0;
			ReplayWindow window(display, {position, position, rect.w, rect.h});
			check(window.window() && window.renderer(), "create replay window");
			if (visible)
			{
				const auto title = "Offline FreeRDP replay - monitor " + std::to_string(n + 1) +
				                   " (Esc to quit)";
				check(SDL_SetWindowTitle(window.window(), title.c_str()) &&
				      SDL_SetWindowResizable(window.window(), false), "configure playback window");
			}
			int w = 0, h = 0;
			check(SDL_GetWindowSizeInPixels(window.window(), &w, &h) && w == rect.w && h == rect.h, "replay window pixel size differs from capture");
			window.setOffsetX(-rect.x);
			window.setOffsetY(-rect.y);
			window.renderMetrics().setIdentity(window.id(), n + 1);
			monitors.push_back(rect);
			rendererName = SDL_GetRendererName(window.renderer());
			std::cerr << "Replay monitor " << n + 1 << ": " << rect.w << 'x' << rect.h << " +" << rect.x << '+' << rect.y << " renderer=" << rendererName << '\n';
			sdl->_windows.emplace(window.id(), std::move(window));
		}
		// Initialize every viewport, including the old renderer's uninitialized target.
		for (auto& entry : sdl->_windows)
		{
			check(sdl->drawToWindow(entry.second), "initial full repaint");
			if (visible)
			{
				check(SDL_ShowWindow(entry.second.window()) && SDL_SyncWindow(entry.second.window()), "show playback window");
				std::cerr << "Visible replay window " << entry.first << '\n';
			}
		}
		flush();
	}
	bool pumpEvents()
	{
		if (!visible)
		{
			SDL_PumpEvents();
			return true;
		}
		SDL_Event event{};
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT ||
			    (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) ||
			    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && sdl->_windows.count(event.window.windowID)))
			{
				quitRequested = true;
				return false;
			}
			if (event.type != SDL_EVENT_WINDOW_EXPOSED &&
			    event.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
				continue;
			auto found = sdl->_windows.find(event.window.windowID);
			if (found == sdl->_windows.end()) continue; // Events from a previous ResetGraphics.
			auto& window = found->second;
			if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
			{
				check(window.resizeToScale() && SDL_SyncWindow(window.window()), "restore recorded window size");
				check(sdl->drawToWindow(window), "repaint resized playback window");
			}
			else
				check(window.updateSurface(false), "repaint exposed playback window");
		}
		return true;
	}
	bool waitUntil(Clock::time_point deadline)
	{
		if (!visible)
		{
			std::this_thread::sleep_until(deadline);
			return true;
		}
		// Continue handling close/Escape even during long idle gaps in the capture.
		while (Clock::now() < deadline)
		{
			if (!pumpEvents()) return false;
			std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(8)));
		}
		return pumpEvents();
	}
	void flush()
	{
		for (auto& entry : sdl->_windows)
			entry.second.renderMetrics().flush();
	}
	void paint()
	{
		auto gdi = instance->context->gdi;
		if (sdl->_windows.empty() || !gdi || !gdi->primary)
			return;
		auto hwnd = gdi->primary->hdc->hwnd;
		if (!hwnd || hwnd->invalid->null || gdi->suppressOutput)
			return;
		std::vector<SDL_Rect> rects;
		for (int n = 0; n < hwnd->ninvalid; n++)
		{
			const auto& r = hwnd->cinvalid[n];
			rects.push_back({r.x, r.y, r.w, r.h});
		}
		if (rects.empty()) return;
		const auto begin = Clock::now();
		check(sdl->drawToWindows(rects), "production monitor rendering");
		const auto elapsed = ns(Clock::now() - begin);
		paintCount++;
		if (measured) renderSamples.push_back(elapsed);
		if (verifyEvery && paintCount % verifyEvery == 0) verify(false);
	}
	void verify(bool save)
	{
		auto source = sdl->_primary.get();
		check(source && !sdl->_windows.empty(), "no decoded desktop to verify");
		size_t index = 0;
		for (auto& entry : sdl->_windows)
		{
			auto& window = entry.second;
			auto renderer = window.renderer();
			auto previous = SDL_GetRenderTarget(renderer);
			check(SDL_SetRenderTarget(renderer, window._renderTarget), "select readback target");
			std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
			check(SDL_SetRenderTarget(renderer, previous), "restore readback target");
			check(image != nullptr, "readback");
			std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> converted(SDL_ConvertSurface(image.get(), SDL_PIXELFORMAT_BGRA32), SDL_DestroySurface);
			const auto& rect = monitors.at(index++);
			check(converted && converted->w == rect.w && converted->h == rect.h, "readback size");
			for (int y = 0; y < rect.h; y++)
			{
				const auto* got = static_cast<const BYTE*>(converted->pixels) + y * converted->pitch;
				const auto* want = static_cast<const BYTE*>(source->pixels) + (y + rect.y) * source->pitch + rect.x * 4;
				for (int x = 0; x < rect.w; x++)
					if (std::memcmp(got + x * 4, want + x * 4, 3) != 0)
					{
						if (!saveFinal.empty()) (void)SDL_SaveBMP(converted.get(), (saveFinal + "-mismatch.bmp").c_str());
						throw std::runtime_error("pixel mismatch, paint=" + std::to_string(paintCount) + " monitor=" + std::to_string(index) + " x=" + std::to_string(x) + " y=" + std::to_string(y));
					}
			}
			if (save && !saveFinal.empty())
				check(SDL_SaveBMP(converted.get(), (saveFinal + "-monitor-" + std::to_string(index) + ".bmp").c_str()), "save final image");
		}
		verified++;
	}
};
SdlGraphicsReplay* SdlGraphicsReplay::active = nullptr;

int main(int argc, char** argv)
{
	umask(0077);
	try
	{
		std::string file, backend = "software", output, metrics;
		uint64_t maxMessages = UINT64_MAX, verifyEvery = 0, startUs = 0, stopUs = UINT64_MAX;
		bool paced = false, visible = false;
		for (int n = 1; n < argc; n++)
		{
			std::string arg = argv[n];
			auto value = [&]() -> std::string { check(n + 1 < argc, "missing option value"); return argv[++n]; };
			if (arg == "--input") file = value();
			else if (arg == "--renderer") backend = value();
			else if (arg == "--metrics") metrics = value();
			else if (arg == "--save-final") output = value();
			else if (arg == "--max-messages") maxMessages = number(value());
			else if (arg == "--verify-every") verifyEvery = number(value());
			else if (arg == "--start-seconds") startUs = seconds(value());
			else if (arg == "--stop-seconds") stopUs = seconds(value());
			else if (arg == "--paced") paced = true;
			else if (arg == "--visible") visible = true;
			else if (arg == "--help")
			{
				std::cout << "--input session.gfx [--renderer software|metal] [--metrics output.jsonl]\n"
				             "[--start-seconds N] [--stop-seconds N] [--max-messages N] [--paced]\n"
				             "[--verify-every N] [--save-final private/image-prefix] [--visible]\n"
				             "--visible shows monitor windows; Escape or close stops playback.\n";
				return 0;
			}
			else throw std::runtime_error("unknown argument: " + arg);
		}
		check(!file.empty() && maxMessages > 0 && stopUs >= startUs, "input and valid measurement interval required");
		check(backend == "software" || backend == "metal", "supported renderers: software, metal");
		const auto messages = load(file); // Loading is outside every timed scope.
		for (const char* name : {"com.freerdp.codec", "com.freerdp.gdi"})
		{
			auto log = WLog_Get(name);
			wLogCallbacks callbacks{};
			callbacks.message = decodeLog;
			check(log && WLog_SetLogLevel(log, WLOG_INFO) &&
			      WLog_SetLogAppenderType(log, WLOG_APPENDER_CALLBACK) &&
			      WLog_ConfigureAppender(WLog_GetLogAppender(log), "callbacks", &callbacks), "decode log guard");
		}
		if (!metrics.empty())
		{
			check(!std::ifstream(metrics).good(), "metrics file already exists");
			check(setenv("FREERDP_SDL_RENDER_METRICS", metrics.c_str(), 1) == 0, "set metrics path");
		}
		if (backend == "software" && !visible) check(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"), "dummy driver");
		check(SDL_SetHint(SDL_HINT_RENDER_DRIVER, backend.c_str()), "renderer hint");
		check(SDL_Init(SDL_INIT_VIDEO), "SDL_Init");
		check(TTF_Init(), "TTF_Init");
		{
			SdlGraphicsReplay replay;
			replay.visible = visible;
			replay.verifyEvery = verifyEvery;
			replay.saveFinal = output;
			replay.startUs = startUs;
			replay.stopUs = stopUs;
			uint64_t fed = 0, measuredMessages = 0, feedWallNs = 0, measurementStartNs = 0;
			const auto playbackStart = Clock::now();
			double cpuStart = 0;
			for (const auto& message : messages)
			{
				if (fed >= maxMessages || message.us > stopUs) break;
				if (!replay.pumpEvents()) break;
				if (paced && !replay.waitUntil(playbackStart + std::chrono::microseconds(message.us))) break;
				if (!replay.measured && message.us >= startUs)
				{
					replay.flush();
					replay.measured = true;
					measurementStartNs = ns(Clock::now().time_since_epoch());
					cpuStart = cpuSeconds();
				}
				replay.currentUs = message.us;
				const auto begin = Clock::now();
				replay.host->feed(message);
				check(decodeErrors == 0, "decoder reported an error or ignored update; benchmark rejected");
				check(!replay.callbackFailed, replay.callbackError);
				if (replay.measured) { feedWallNs += ns(Clock::now() - begin); measuredMessages++; }
				fed++;
			}
			const auto cpu = replay.measured ? cpuSeconds() - cpuStart : 0;
			replay.flush();
			const auto measurementEndNs = ns(Clock::now().time_since_epoch());
			check(replay.quitRequested || !replay.renderSamples.empty(), "no measured decoded paints; benchmark rejected");
			check(replay.quitRequested || replay.rendererName == backend, "SDL renderer fallback; benchmark rejected");
			if ((verifyEvery || !output.empty()) && replay.paintCount) replay.verify(true);
			uint64_t sum = 0;
			for (auto n : replay.renderSamples) sum += n;
			std::sort(replay.renderSamples.begin(), replay.renderSamples.end());
			auto percentile = [&](size_t p) -> double {
				if (replay.renderSamples.empty()) return 0;
				return replay.renderSamples[(replay.renderSamples.size() * p + 99) / 100 - 1] / 1000000.0;
			};
			std::cout << std::fixed << std::setprecision(6)
			          << "{\"schema\":\"freerdp.graphics_replay_result\",\"version\":1,\"renderer\":\"" << replay.rendererName
			          << "\",\"messages\":" << fed << ",\"measured_messages\":" << measuredMessages
			          << ",\"measurement_start_ns\":" << measurementStartNs << ",\"measurement_end_ns\":" << measurementEndNs
			          << ",\"start_capture_us\":" << startUs << ",\"decode_errors\":" << decodeErrors.load()
			          << ",\"last_capture_us\":" << replay.currentUs << ",\"decode_and_render_wall_s\":" << feedWallNs / 1e9
			          << ",\"process_cpu_s\":" << cpu << ",\"render_wall_s\":" << sum / 1e9
			          << ",\"render_p50_ms\":" << percentile(50) << ",\"render_p95_ms\":" << percentile(95)
			          << ",\"render_p99_ms\":" << percentile(99) << ",\"paint_callbacks\":" << replay.paintCount
			          << ",\"measured_paints\":" << replay.renderSamples.size() << ",\"verified_checkpoints\":" << replay.verified
			          << ",\"paced\":" << (paced ? "true" : "false") << ",\"verification_enabled\":" << ((verifyEvery || !output.empty()) ? "true" : "false")
			          << ",\"visible\":" << (visible ? "true" : "false") << ",\"cancelled\":" << (replay.quitRequested ? "true" : "false")
			          << ",\"gfx_stats\":{";
			bool comma = false;
			for (size_t i = 0; i < rdpgfx_stats_max_index(); i++)
			{
				const auto value = rdpgfx_stats_value_for_index(replay.host->gfx, i);
				if (!value) continue;
				if (comma) std::cout << ',';
				comma = true;
				std::cout << '"' << rdpgfx_stats_name_for_index(i) << "\":" << value;
			}
			std::cout << "}}\n";
		}
		TTF_Quit();
		SDL_Quit();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Replay failed: " << error.what() << '\n';
		return 1;
	}
}
