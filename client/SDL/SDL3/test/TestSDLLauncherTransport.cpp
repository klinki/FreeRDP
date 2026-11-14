/** Actual private socket protocol regression tests; never contacts an RDP host. */
#include "../sdl_launcher.hpp"
#include "../sdl_hosted.hpp"
#include "../sdl_hosted_surface.hpp"
#include <mach/mach.h>
#include <set>
#include <freerdp/error.h>
#include <freerdp/client.h>
#include <freerdp/crypto/crypto.h>
#include <chrono>
#include <thread>
#include <winpr/crt.h>
#include <future>
#include <iostream>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef WITH_OPENSSL
#include <openssl/opensslv.h>
#endif

// The production implementation only posts a quit event; avoid initializing a UI in this test.
bool sdl_push_quit()
{
	return true;
}
namespace
{
	constexpr const char* session = "7A4499D4-6232-4775-9326-57961998CE02";
	void require(bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}
	std::string getString(WINPR_JSON* json, const char* key)
	{
		auto value = WINPR_JSON_GetStringValue(WINPR_JSON_GetObjectItemCaseSensitive(json, key));
		return value ? value : "";
	}
	struct Connection
	{
		int peer = -1;
		freerdp* rdp = freerdp_new();
		std::unique_ptr<SdlLauncher> bridge;
		std::string buffered;
		Connection()
		{
			require(rdp && freerdp_context_new(rdp), "Cannot allocate test RDP context");
			int sockets[2];
			require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
			peer = sockets[0];
			bridge = std::make_unique<SdlLauncher>(sockets[1], session, rdp->context);
		}
		~Connection()
		{
			bridge.reset();
			if (peer >= 0)
				close(peer);
			freerdp_context_free(rdp);
			freerdp_free(rdp);
		}
		SdlLauncher::Json next()
		{
			while (buffered.find('\n') == std::string::npos)
			{
				pollfd fd{ peer, POLLIN, 0 };
				require(poll(&fd, 1, 2000) > 0, "Protocol response timed out");
				char bytes[4096];
				const auto n = recv(peer, bytes, sizeof(bytes), 0);
				require(n > 0, "Socket closed before response");
				buffered.append(bytes, static_cast<size_t>(n));
			}
			const auto end = buffered.find('\n');
			SdlLauncher::Json result(WINPR_JSON_ParseWithLength(buffered.data(), end),
			                         WINPR_JSON_Delete);
			buffered.erase(0, end + 1);
			require(result != nullptr, "Invalid protocol output");
			require(getString(result.get(), "sessionId") == session, "Wrong output session ID");
			require(WINPR_JSON_GetNumberValue(
			            WINPR_JSON_GetObjectItemCaseSensitive(result.get(), "v")) == 1,
			        "Wrong output version");
			return result;
		}
		void write(const std::string& frame)
		{
			require(::send(peer, frame.data(), frame.size(), 0) ==
			            static_cast<ssize_t>(frame.size()),
			        "Cannot write test frame");
		}
		void command(const char* type, const std::string& fields = "")
		{
			write(std::string("{\"v\":1,\"type\":\"") + type + "\",\"sessionId\":\"" + session +
			      "\"" + fields + "}\n");
		}
	};
	// Every wait has a deadline, including reader/worker teardown with a silent parent.
	template <typename Predicate> void eventually(Predicate predicate, const char* message)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!predicate())
		{
			require(std::chrono::steady_clock::now() < deadline, message);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	void startOptions(Connection& c, bool confirm, bool thumbnails)
	{
		c.command("start", std::string(",\"arguments\":[],\"displaySelections\":[],\"confirmSessionClose\":") +
		                       (confirm ? "true" : "false") + ",\"thumbnailsEnabled\":" +
		                       (thumbnails ? "true" : "false"));
		// True options are observable atomics published by the receiver.
		if (confirm)
			eventually([&] { return c.bridge->closeConfirmationEnabled(); }, "Close option not applied");
		if (thumbnails)
			eventually([&] { return c.bridge->thumbnailsEnabled(); }, "Thumbnail option not applied");
	}
	void performanceOverlayStyles()
	{
		Connection c;
		c.command("start", ",\"arguments\":[],\"displaySelections\":[],\"performanceSupported\":true");
		eventually([&] { return c.bridge->performanceAvailable(); }, "Performance option not applied");
		c.command("performance_control", ",\"performanceEnabled\":true,\"performanceOverlayVisible\":true,\"performanceGeneration\":1,\"performanceOverlayStyle\":\"quake\"");
		eventually([&] { return c.bridge->performanceMask() == 7 && c.bridge->performanceGeneration() == 1; }, "Quake style not applied");
		c.bridge->requestPerformanceOverlay(false);
		std::ignore = c.next();
		require(c.bridge->performanceMask() == 5, "Hiding lost the Quake style");
		c.bridge->requestPerformanceOverlay(true);
		std::ignore = c.next();
		require(c.bridge->performanceMask() == 7, "Showing lost the Quake style");
		c.command("performance_control", ",\"performanceEnabled\":true,\"performanceOverlayVisible\":true,\"performanceGeneration\":2,\"performanceOverlayStyle\":\"panel\"");
		eventually([&] { return c.bridge->performanceMask() == 3 && c.bridge->performanceGeneration() == 2; }, "Panel style not restored");
		c.command("performance_control", ",\"performanceEnabled\":true,\"performanceOverlayVisible\":false,\"performanceGeneration\":3");
		eventually([&] { return c.bridge->performanceMask() == 1 && c.bridge->performanceGeneration() == 3; }, "Older control defaults to Panel");
		for (const auto field : {"\"invalid\"", "true", "null"})
		{
			Connection invalid;
			invalid.command("start", ",\"arguments\":[],\"displaySelections\":[],\"performanceSupported\":true");
			eventually([&] { return invalid.bridge->performanceAvailable(); }, "Performance option not applied");
			invalid.command("performance_control", std::string(",\"performanceEnabled\":false,\"performanceOverlayVisible\":false,\"performanceOverlayStyle\":") + field);
			eventually([&] { return invalid.bridge->cancelled(); }, "Invalid style did not reject the control");
		}
	}
	void windowGeometryOptions()
	{
		Connection enabled;
		enabled.command("start", ",\"arguments\":[],\"displaySelections\":[],\"rememberWindowSize\":true,\"windowSize\":{\"width\":900,\"height\":650}");
		eventually([&] { return enabled.bridge->windowGeometryEnabled(); }, "Window geometry option not applied");
		Connection legacy;
		startOptions(legacy, true, false);
		require(!legacy.bridge->windowGeometryEnabled(), "Older launcher unexpectedly enabled geometry reporting");
		for (const auto fields : {
		         ",\"rememberWindowSize\":\"true\"",
		         ",\"rememberWindowSize\":true,\"windowSize\":{\"width\":0,\"height\":600}",
		         ",\"rememberWindowSize\":true,\"windowSize\":{\"width\":900.5,\"height\":600}",
		         ",\"rememberWindowSize\":true,\"windowSize\":{\"width\":900,\"height\":999999}",
		         ",\"windowSize\":{\"width\":900,\"height\":600}" })
		{
			Connection invalid;
			invalid.command("start", std::string(",\"arguments\":[],\"displaySelections\":[]") + fields);
			eventually([&] { return invalid.bridge->cancelled(); }, "Invalid window geometry was accepted");
		}
	}
	void windowGeometryReporting()
	{
		require(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy") && SDL_Init(SDL_INIT_VIDEO), "Cannot initialize geometry fixture");
		{
			Connection c;
			c.command("start", ",\"arguments\":[],\"displaySelections\":[],\"rememberWindowSize\":true,\"windowSize\":{\"width\":900,\"height\":650}");
			eventually([&] { return c.bridge->windowGeometryEnabled(); }, "Window geometry option not applied");
			std::ignore = freerdp_settings_set_uint32(c.rdp->context->settings, FreeRDP_DesktopWidth, 800);
			std::ignore = freerdp_settings_set_uint32(c.rdp->context->settings, FreeRDP_DesktopHeight, 600);
			std::ignore = freerdp_settings_set_bool(c.rdp->context->settings, FreeRDP_DynamicResolutionUpdate, FALSE);
			auto window = SDL_CreateWindow("geometry transport fixture", 640, 480, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
			require(window != nullptr, "Cannot create geometry fixture window");
			c.bridge->configureWindowGeometry(window);
			require(SDL_ShowWindow(window) && SDL_SyncWindow(window), "Cannot show geometry fixture");
			c.bridge->state("connected"); std::ignore = c.next();
			c.bridge->observeWindowGeometry(window, SDL_EVENT_WINDOW_SHOWN);
			auto first = c.next();
			auto size = WINPR_JSON_GetObjectItemCaseSensitive(first.get(), "windowSize");
			require(getString(first.get(), "type") == "window_geometry" &&
			        WINPR_JSON_GetNumberValue(WINPR_JSON_GetObjectItemCaseSensitive(size, "width")) == 900,
			        "Restored normal geometry not reported");
			for (int width = 700; width <= 900; width++)
			{
				require(SDL_SetWindowSize(window, width, 600) && SDL_SyncWindow(window), "Cannot resize geometry fixture");
				c.bridge->observeWindowGeometry(window, SDL_EVENT_WINDOW_RESIZED);
			}
			require(freerdp_settings_get_uint32(c.rdp->context->settings, FreeRDP_DesktopWidth) == 800 &&
			        freerdp_settings_get_uint32(c.rdp->context->settings, FreeRDP_DesktopHeight) == 600,
			        "Local geometry changed fixed remote desktop dimensions");
			c.bridge->ended(0, "geometry fixture complete");
			int lastWidth = 0;
			for (;;)
			{
				auto event = c.next();
				if (getString(event.get(), "type") == "ended") break;
				size = WINPR_JSON_GetObjectItemCaseSensitive(event.get(), "windowSize");
				lastWidth = static_cast<int>(WINPR_JSON_GetNumberValue(WINPR_JSON_GetObjectItemCaseSensitive(size, "width")));
			}
			require(lastWidth == 900, "Latest coalesced geometry was not delivered before terminal event");
			SDL_DestroyWindow(window);
		}
		SDL_Quit();
	}
    void screenRefresh()
    {
        Connection c;
        c.command("start", ",\"arguments\":[],\"displaySelections\":[],\"performanceSupported\":true");
        eventually([&] { return c.bridge->performanceAvailable(); }, "Start not applied");
        unsigned generation = 0;
        const auto barrier = [&] {
            ++generation;
            c.command("performance_control", ",\"performanceEnabled\":false,\"performanceOverlayVisible\":false,\"performanceGeneration\":" + std::to_string(generation));
            eventually([&] { return c.bridge->performanceGeneration() == generation; }, "Reader barrier not applied");
        };
        c.command("refresh_screen"); barrier();
        require(!c.bridge->takeScreenRefreshRequest(), "Refresh accepted before connected");
        c.bridge->state("connected"); std::ignore = c.next();
        c.write("{\"v\":1,\"type\":\"refresh_screen\",\"sessionId\":\"stale\"}\n"); barrier();
        require(!c.bridge->takeScreenRefreshRequest(), "Stale session refresh accepted");
        c.command("refresh_screen"); c.command("refresh_screen"); barrier();
        require(c.bridge->takeScreenRefreshRequests() == sdlRefreshBit(SdlRefreshMode::Server) && !c.bridge->takeScreenRefreshRequests(), "Refresh requests did not coalesce");
        uint32_t expectedModes = 0;
        for (const auto mode : sdlRefreshModes) {
            c.command("refresh_screen", std::string(",\"refreshMode\":\"") + sdlRefreshName(mode) + "\"");
            expectedModes |= sdlRefreshBit(mode);
        }
        barrier();
        require(c.bridge->takeScreenRefreshRequests() == expectedModes, "Distinct refresh methods were lost");
        c.command("refresh_screen"); barrier();
        c.bridge->state("reconnecting"); std::ignore = c.next();
        require(!c.bridge->takeScreenRefreshRequest(), "Pending refresh survived reconnect");
        c.command("refresh_screen"); barrier();
        c.bridge->state("connected"); std::ignore = c.next();
        require(!c.bridge->takeScreenRefreshRequest(), "Reconnecting refresh replayed after recovery");
        c.command("refresh_screen"); barrier(); c.command("cancel");
        eventually([&] { return c.bridge->cancelled(); }, "Cancel not applied");
        require(!c.bridge->takeScreenRefreshRequest(), "Cancelled refresh was delivered");
        for (const auto value : {"\"unknown\"", "1", "null", "true"}) {
            Connection invalid;
            invalid.command("refresh_screen", std::string(",\"refreshMode\":") + value);
            eventually([&] { return invalid.bridge->cancelled(); }, "Invalid refresh mode was accepted");
        }
    }
	void closeConfirmation()
	{
		Connection c;
		startOptions(c, true, false);
		// A close request can interrupt authentication; it need not wait for connected.
		require(c.bridge->requestClose(), "Close confirmation was not queued");
		auto request = c.next();
		require(getString(request.get(), "type") == "close_request", "Missing close request");
		const auto first = getString(request.get(), "requestId");
		require(!first.empty() && !c.bridge->requestClose(), "Repeated close was not coalesced");
		c.command("close_response", ",\"requestId\":\"stale\",\"accepted\":true");
		c.command("close_response", ",\"requestId\":\"" + first + "\",\"accepted\":\"true\"");
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		require(!c.bridge->cancelled() && !c.bridge->requestClose(), "Stale/invalid close response applied");
		c.command("close_response", ",\"requestId\":\"" + first + "\",\"accepted\":false");
		eventually([&] { return c.bridge->requestClose(); }, "Rejecting close did not restore session");
		request = c.next();
		const auto second = getString(request.get(), "requestId");
		require(second != first && !c.bridge->cancelled(), "New close request reused a completed ID");
		c.command("close_response", ",\"requestId\":\"" + first + "\",\"accepted\":true");
		c.command("close_response", ",\"requestId\":\"" + second + "\",\"accepted\":true");
		eventually([&] { return c.bridge->cancelled(); }, "Confirming close did not cancel");
		c.bridge->ended(0, "Synthetic confirmed close");
		auto ended = c.next();
		require(getString(ended.get(), "outcome") == "cancelled", "Confirmed close misclassified");
	}
	void closeCancellationAndEOF()
	{
		{
			Connection c;
			require(!c.bridge->requestClose() && c.bridge->cancelled(), "Default close must remain immediate");
		}
		for (bool eof : { false, true })
		{
			Connection c;
			startOptions(c, true, true);
			require(c.bridge->requestClose(), "Missing pending close");
			std::ignore = c.next();
			c.bridge->state("connected");
			std::ignore = c.next();
			// No frame exists: the thumbnail worker is waiting as well.
			c.command("thumbnail_request", ",\"requestId\":\"pending\"");
			if (eof)
			{
				close(c.peer);
				c.peer = -1;
			}
			else
				c.command("cancel");
			eventually([&] { return c.bridge->cancelled(); }, "Cancel/EOF waited for close confirmation");
			const auto began = std::chrono::steady_clock::now();
			c.bridge.reset();
			require(std::chrono::steady_clock::now() - began < std::chrono::seconds(2),
			        "Pending close/thumbnail worker prevented bounded teardown");
		}
	}
	UINT32 bigEndian(const BYTE* value)
	{
		return (static_cast<UINT32>(value[0]) << 24) | (static_cast<UINT32>(value[1]) << 16) |
		       (static_cast<UINT32>(value[2]) << 8) | value[3];
	}
	std::vector<BYTE> syntheticFrame(UINT32 width, UINT32 height)
	{
		std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
		UINT32 noise = 123456789;
		for (auto& pixel : pixels)
		{
			noise ^= noise << 13;
			noise ^= noise >> 17;
			noise ^= noise << 5;
			pixel = static_cast<BYTE>(noise);
		}
		return pixels;
	}
	void thumbnailBoundsAndDisable()
	{
		Connection c;
		startOptions(c, false, true);
		c.bridge->state("connected");
		std::ignore = c.next();
		const auto pixels = syntheticFrame(800, 600);
		require(!c.bridge->captureThumbnail(nullptr, 800, 600, 3200) &&
		            !c.bridge->captureThumbnail(pixels.data(), 800, 600, 1) &&
		            !c.bridge->captureThumbnail(pixels.data(), 0, 600, 3200),
		        "Invalid thumbnail source accepted");
		require(c.bridge->captureThumbnail(pixels.data(), 800, 600, 3200), "Valid completed frame rejected");
		require(!c.bridge->captureThumbnail(pixels.data(), 800, 600, 3200), "Thumbnail frame rate unbounded");
		c.command("thumbnail_request", ",\"requestId\":\"frame-1\"");
		c.command("thumbnail_request", ",\"requestId\":\"flood-ignored\"");
		auto event = c.next();
		require(getString(event.get(), "type") == "thumbnail" &&
		            getString(event.get(), "requestId") == "frame-1", "Thumbnail request correlation lost");
		const auto encoded = getString(event.get(), "pngBase64");
		require(!encoded.empty() && encoded.size() <= 350000, "Unbounded thumbnail envelope");
		BYTE* decoded = nullptr;
		size_t size = 0;
		crypto_base64_decode(encoded.c_str(), encoded.size(), &decoded, &size);
		std::unique_ptr<BYTE, decltype(&free)> owned(decoded, free);
		const BYTE signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
		require(decoded && size >= 24 && size <= 262144 && memcmp(decoded, signature, 8) == 0 &&
		            memcmp(decoded + 12, "IHDR", 4) == 0, "Invalid or oversized thumbnail PNG");
		const auto width = bigEndian(decoded + 16), height = bigEndian(decoded + 20);
		require(width > 0 && width <= 320 && height > 0 && height <= 200 &&
		            WINPR_JSON_GetNumberValue(WINPR_JSON_GetObjectItemCaseSensitive(event.get(), "width")) == width &&
		            WINPR_JSON_GetNumberValue(WINPR_JSON_GetObjectItemCaseSensitive(event.get(), "height")) == height,
		        "PNG and advertised dimensions differ");
		c.command("thumbnail_request", ",\"thumbnailsEnabled\":false");
		eventually([&] { return !c.bridge->thumbnailsEnabled(); }, "Disable capture command ignored");
		require(!c.bridge->captureThumbnail(pixels.data(), 800, 600, 3200), "Disabled capture retained frame");
		c.command("cancel");
		eventually([&] { return c.bridge->cancelled(); }, "Thumbnail cancel ignored");
		c.bridge->ended(0, "Synthetic thumbnail cancellation");
		event = c.next();
		require(getString(event.get(), "type") == "ended", "Flood produced a second thumbnail");
	}
	void thumbnailBackpressureTeardown()
	{
		Connection c;
		startOptions(c, false, true);
		c.bridge->state("connected");
		std::ignore = c.next();
		// Keep output larger than this socket buffer to exercise a stalled parent.
		int size = 1024;
		std::ignore = setsockopt(c.peer, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
		const auto pixels = syntheticFrame(320, 200);
		require(c.bridge->captureThumbnail(pixels.data(), 320, 200, 1280), "Backpressure frame rejected");
		c.command("thumbnail_request", ",\"requestId\":\"unread\"");
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const auto began = std::chrono::steady_clock::now();
		c.bridge.reset();
		require(std::chrono::steady_clock::now() - began < std::chrono::seconds(2),
		        "Unread thumbnail prevented bounded writer/worker teardown");
	}

	void mouseWheelOptions()
	{
		for (const auto fields : { "", ",\"reverseMouseWheel\":false", ",\"reverseMouseWheel\":true" })
		{
			Connection c;
			c.command("start", std::string(",\"arguments\":[],\"displaySelections\":[]") + fields);
			std::vector<std::string> arguments;
			std::string error;
			require(c.bridge->prepare(arguments, error), "Valid wheel option rejected");
			require(c.bridge->reverseMouseWheel() == (strstr(fields, "true") != nullptr),
			        "Wrong per-session mouse wheel option");
			require(arguments.empty(), "Mouse wheel option leaked into ordinary arguments");
		}
		for (const auto fields : { ",\"reverseMouseWheel\":\"true\"", ",\"reverseMouseWheel\":1",
		                          ",\"reverseMouseWheel\":null" })
		{
			Connection c;
			c.command("start", std::string(",\"arguments\":[],\"displaySelections\":[]") + fields);
			eventually([&] { return c.bridge->cancelled(); }, "Malformed wheel option accepted");
			require(!c.bridge->reverseMouseWheel(), "Malformed wheel option changed scroll direction");
		}
	}
	void handshakeCapabilities()
	{
		Connection c;
		// prepare emits hello before consuming cancel. No UI or remote connection is needed.
		c.command("cancel");
		std::vector<std::string> arguments;
		std::string error;
		require(!c.bridge->prepare(arguments, error), "Discovery cancellation was ignored");
		auto hello = c.next();
		auto query = SdlLauncher::capabilities();
		require(getString(hello.get(), "type") == "hello" && query, "Missing capability discovery");
		require(getString(hello.get(), "engineVersion") == getString(query.get(), "engineVersion"),
		        "Query and handshake engine versions differ");
		auto helloCaps = WINPR_JSON_GetObjectItemCaseSensitive(hello.get(), "capabilities");
		auto queryCaps = WINPR_JSON_GetObjectItemCaseSensitive(query.get(), "capabilities");
		std::unique_ptr<char, decltype(&free)> helloEncoded(WINPR_JSON_PrintUnformatted(helloCaps), free);
		std::unique_ptr<char, decltype(&free)> queryEncoded(WINPR_JSON_PrintUnformatted(queryCaps), free);
		require(WINPR_JSON_IsArray(helloCaps) && WINPR_JSON_IsArray(queryCaps) &&
		            helloEncoded && queryEncoded && strcmp(helloEncoded.get(), queryEncoded.get()) == 0,
		        "Query and live handshake capabilities differ");
		require(WINPR_JSON_IsArray(WINPR_JSON_GetObjectItemCaseSensitive(hello.get(), "displays")),
		        "Handshake lost its display inventory");
		const std::string listed = queryEncoded.get();
#if defined(WITH_OPENSSL) && OPENSSL_VERSION_NUMBER >= 0x10101000L
		require(listed.find("\"tls_keylog\"") != std::string::npos, "TLS keylog support not advertised");
#else
		require(listed.find("\"tls_keylog\"") == std::string::npos, "Unavailable TLS keylog advertised");
#endif
	}
	void tlsSecretsOptions()
	{
		char directoryTemplate[] = "/private/tmp/freerdp-tls-keylog-XXXXXX";
		const char* created = mkdtemp(directoryTemplate);
		require(created != nullptr, "Cannot create private TLS test directory");
		const std::string directory = created;
		const std::string file = directory + "/secrets";
		const std::string alias = directory + "/alias";
		const std::string linkedDirectory = directory + "/linked";
		int fd = open(file.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
		require(fd >= 0, "Cannot create private TLS test file");
		close(fd);
		require(symlink(file.c_str(), alias.c_str()) == 0 &&
		            symlink(directory.c_str(), linkedDirectory.c_str()) == 0,
		        "Cannot create TLS symlink fixtures");
		auto run = [&](const std::string& fields, bool accepted, bool configured) {
			Connection c;
			const auto args = fields.find("\"arguments\"") == std::string::npos
			                      ? ",\"arguments\":[]" : "";
			c.command("start", args + std::string(",\"displaySelections\":[]") + fields);
			std::vector<std::string> arguments;
			std::string error;
			require(c.bridge->prepare(arguments, error) == accepted, "Wrong TLS start acceptance");
			if (accepted)
			{
				require(arguments.empty(), "TLS start changed ordinary arguments");
				require(c.bridge->applyTlsSecretsFile(c.rdp->context->settings, error),
				        "Cannot apply valid TLS secrets setting");
				const char* actual = freerdp_settings_get_string(c.rdp->context->settings,
				                                               FreeRDP_TlsSecretsFile);
				require((actual != nullptr) == configured, "TLS setting changed without request");
				if (configured)
					require(actual == file, "Wrong TLS secrets file setting");
			}
		};
		run("", true, false);
		run(",\"tlsSecretsFile\":\"" + file + "\"", true, true);
		run(",\"arguments\":[\"/tls:secrets-file:" + file + "\"]", false, false);
		run(",\"arguments\":[\"/tls-secrets-file:" + file + "\"]", false, false);
		run(",\"tlsSecretsFile\":null", false, false);
		run(",\"tlsSecretsFile\":\"\"", false, false);
		run(",\"tlsSecretsFile\":\"relative/path\"", false, false);
		run(",\"tlsSecretsFile\":\"" + alias + "\"", false, false);
		run(",\"tlsSecretsFile\":\"" + linkedDirectory + "/secrets\"", false, false);
		run(",\"tlsSecretsFile\":\"" + directory + "\"", false, false);
		run(",\"tlsSecretsFile\":\"" + directory + "/missing\"", false, false);
		run(",\"tlsSecretsFile\":\"" + file + "\\u0000suffix\"", false, false);
		require(chmod(file.c_str(), 0644) == 0, "Cannot change TLS fixture permissions");
		run(",\"tlsSecretsFile\":\"" + file + "\"", false, false);
		unlink(linkedDirectory.c_str());
		unlink(alias.c_str());
		unlink(file.c_str());
		rmdir(directory.c_str());
	}
	void promptAndStaleResponse()
	{
		Connection c;
		char* username = _strdup("old");
		char* domain = nullptr;
		char* password = nullptr;
		auto auth =
		    std::async(std::launch::async, [&]
		               { return c.bridge->authenticate(&username, &password, &domain, AUTH_NLA); });
		auto request = c.next();
		require(getString(request.get(), "type") == "auth_request", "Missing auth prompt");
		const auto id = getString(request.get(), "requestId");
		c.write(std::string("{\"v\":1,\"type\":\"cancel\",\"sessionId\":\"stale\"}\n"));
		c.command("future_command");
		c.command("auth_response", ",\"requestId\":\"stale\",\"accepted\":true,\"username\":"
		                           "\"wrong\",\"password\":\"synthetic\"");
		require(auth.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
		        "Stale response completed auth");
		const auto fields = ",\"requestId\":\"" + id +
		                    "\",\"accepted\":true,\"username\":\"updated\",\"domain\":\"test\","
		                    "\"password\":\"synthetic-only\"";
		const auto frame = std::string("{\"v\":1,\"type\":\"auth_response\",\"sessionId\":\"") +
		                   session + "\"" + fields + "}\n";
		c.write(frame.substr(0, 13));
		require(auth.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
		        "Partial frame completed auth");
		c.write(frame.substr(13));
		require(auth.get(), "Valid response rejected");
		require(std::string(username) == "updated" && std::string(domain) == "test" &&
		            std::string(password) == "synthetic-only",
		        "Credentials not applied");
		require(c.bridge->authenticate(&username, &password, &domain, AUTH_NLA),
		        "Credential was not reused for duplicate callback");
		auto again = std::async(
		    std::launch::async,
		    [&] { return c.bridge->authenticate(&username, &password, &domain, AUTH_NLA, true); });
		request = c.next();
		require(WINPR_JSON_IsTrue(WINPR_JSON_GetObjectItemCaseSensitive(request.get(), "rejected")),
		        "Rejected credentials not flagged");
		c.command("cancel");
		require(!again.get() && c.bridge->cancelled(), "Cancellation did not unblock auth");
		free(username);
		free(domain);
		SecureZeroMemory(password, strlen(password));
		free(password);
	}
	void concurrentPromptsAndCertificate()
	{
		Connection c;
		char* username = nullptr;
		char* domain = nullptr;
		char* password = nullptr;
		auto auth =
		    std::async(std::launch::async, [&]
		               { return c.bridge->authenticate(&username, &password, &domain, AUTH_NLA); });
		auto cert = std::async(std::launch::async,
		                       [&]
		                       {
			                       return c.bridge->certificate(
			                           "localhost", 3389, "test", "subject", "issuer", "new",
			                           "oldSubject", "oldIssuer", "old", 0);
		                       });
		auto first = c.next();
		auto second = c.next();
		auto* certRequest =
		    getString(first.get(), "type") == "certificate_request" ? first.get() : second.get();
		require(getString(certRequest, "kind") == "changed" &&
		            getString(certRequest, "oldFingerprint") == "old",
		        "Changed certificate details missing");
		c.command("certificate_response", ",\"requestId\":\"" +
		                                      getString(certRequest, "requestId") +
		                                      "\",\"decision\":\"accept_once\"");
		require(cert.get() == 2, "Temporary certificate decision mapped incorrectly");
		close(c.peer);
		c.peer = -1;
		require(!auth.get(), "Parent EOF did not unblock concurrent authentication");
		free(username);
		free(domain);
		free(password);
	}
	void malformedAndVersion()
	{
		for (const auto& invalid :
		     { std::string("{\"v\":2,\"sessionId\":\"") + session + "\",\"type\":\"focus\"}\n",
		       std::string("invalid\n"), std::string(1024 * 1024 + 1, 'x') })
		{
			Connection c;
			auto cert = std::async(std::launch::async,
			                       [&]
			                       {
				                       return c.bridge->certificate("localhost", 3389, "test",
				                                                    "subject", "issuer", "new",
				                                                    nullptr, nullptr, nullptr, 0);
			                       });
			std::ignore = c.next();
			c.write(invalid);
			require(cert.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
			            cert.get() == 0 && c.bridge->cancelled(),
			        "Malformed input did not unblock certificate decision");
		}
	}
	void lifecycle()
	{
		Connection c;
		c.bridge->state("connected");
		std::ignore = c.next();
		c.bridge->state("reconnecting");
		std::ignore = c.next();
		std::ignore = freerdp_settings_set_bool(c.rdp->context->settings,
		                                        FreeRDP_AutoReconnectionEnabled, TRUE);
		std::ignore = freerdp_settings_set_uint32(c.rdp->context->settings,
		                                          FreeRDP_AutoReconnectMaxRetries, 5);
		require(c.bridge->retry(4) >= 0 && c.bridge->retry(5) == -1, "Retry budget incorrect");
		auto retry = c.next();
		require(getString(retry.get(), "type") == "retry", "Retry event missing");
		c.bridge->ended(1, "Synthetic loss");
		auto ended = c.next();
		require(getString(ended.get(), "outcome") == "retries_exhausted" &&
		            WINPR_JSON_IsTrue(
		                WINPR_JSON_GetObjectItemCaseSensitive(ended.get(), "hadConnected")),
		        "Wrong terminal loss outcome");
	}
	void terminalDistinctions()
	{
		{
			Connection c;
			c.bridge->state("connected");
			std::ignore = c.next();
			c.bridge->ended(131, "Synthetic network loss with retries disabled");
			auto ended = c.next();
			require(getString(ended.get(), "outcome") == "disconnected",
			        "Network loss mislabeled client error");
			require(WINPR_JSON_GetNumberValue(WINPR_JSON_GetObjectItemCaseSensitive(ended.get(), "errorCode")) == FREERDP_ERROR_CONNECT_FAILED,
			        "Transport failure without server error information must not report success");
		}
		{
			Connection c;
			c.bridge->state("connected");
			std::ignore = c.next();
			c.bridge->ended(-1, "Synthetic local exception");
			auto ended = c.next();
			require(getString(ended.get(), "outcome") == "client_error",
			        "Local client exception mislabeled transport loss");
		}
		{
			Connection c;
			c.bridge->setupFailed("Synthetic missing display");
			auto ended = c.next();
			const auto code = WINPR_JSON_GetNumberValue(
			    WINPR_JSON_GetObjectItemCaseSensitive(ended.get(), "errorCode"));
			require(getString(ended.get(), "outcome") == "initial_failure" &&
			            code == FREERDP_ERROR_CONNECT_FAILED,
			        "Setup error is not a valid unsigned FreeRDP error code");
		}
		{
			Connection c;
			c.bridge->state("connected");
			std::ignore = c.next();
			std::ignore = freerdp_settings_set_bool(c.rdp->context->settings,
			                                        FreeRDP_AutoReconnectionEnabled, TRUE);
			std::ignore = c.bridge->retry(0);
			std::ignore = c.next();
			freerdp_set_last_error(c.rdp->context, FREERDP_ERROR_CONNECT_WRONG_PASSWORD);
			c.bridge->ended(131, "Synthetic authentication failure");
			auto ended = c.next();
			require(getString(ended.get(), "outcome") == "authentication_failed",
			        "Retry authentication failure mislabeled exhaustion");
		}
		{
			Connection c;
			c.bridge->state("connected");
			std::ignore = c.next();
			freerdp_set_error_info(c.rdp->context->rdp, ERRINFO_LOGOFF_BY_USER);
			c.bridge->ended(11, "Synthetic remote logoff with DISCONNECT_BY_USER exit mapping");
			auto ended = c.next();
			require(getString(ended.get(), "outcome") == "remote_logoff",
			        "Remote logoff mislabeled connection loss");
		}
		{
			Connection c;
			c.bridge->state("connected");
			std::ignore = c.next();
			c.bridge->cancel();
			c.bridge->ended(0, "Synthetic local close");
			auto ended = c.next();
			require(getString(ended.get(), "outcome") == "cancelled",
			        "Deliberate close mislabeled connection loss");
		}
	}

	void reconnectPolicyExclusions()
	{
		for (unsigned scenario = 0; scenario < 4; ++scenario)
		{
			Connection c;
			static unsigned calls = 0;
			calls = 0;
			c.rdp->RetryDialog = [](freerdp*, const char*, size_t, void*) -> SSIZE_T { ++calls; return -1; };
			std::ignore = freerdp_settings_set_bool(c.rdp->context->settings, FreeRDP_AutoReconnectionEnabled, scenario != 0);
			if (scenario == 1)
				freerdp_set_error_info(c.rdp->context->rdp, ERRINFO_LOGOFF_BY_USER);
			if (scenario == 2)
				freerdp_set_last_error(c.rdp->context, FREERDP_ERROR_CONNECT_WRONG_PASSWORD);
			if (scenario == 3)
				freerdp_set_last_error(c.rdp->context, FREERDP_ERROR_CONNECT_CANCELLED);
			require(!client_auto_reconnect_ex(c.rdp, nullptr) && calls == 0,
			        "Disabled retries, server logoff, rejected credentials, and cancellation must not attempt recovery");
		}
	}

	/* Hosted rendering messages (launcher-bridge.md, "Hosted rendering"). Leaves the process in
	 * hosted mode, so it runs last. */
	void hostedRendering()
	{
		using Case = std::pair<const char*, const char*>;
		const Case valid[] = {
			{ "layout", R"(,"windowId":1,"width":800,"height":500,"scale":2)" },
			{ "layout", R"(,"windowId":1,"width":800,"height":500,"scale":2,"fullscreen":true)" },
			{ "input", R"(,"windowId":1,"action":"key","keyCode":0,"down":true)" },
			{ "input", R"(,"windowId":1,"action":"mouse_move","x":1,"y":2)" },
			{ "focus_state", R"(,"windowId":1,"focused":false)" },
			{ "occlusion", R"(,"windowId":1,"visible":false)" },
		};
		const Case malformed[] = {
			{ "layout", R"(,"windowId":1,"width":800,"height":500)" },
			{ "layout", R"(,"windowId":0,"width":800,"height":500,"scale":2)" },
			{ "layout", R"(,"windowId":1,"width":800,"height":500,"scale":20)" },
			{ "layout", R"(,"windowId":1,"width":800,"height":500,"scale":2,"fullscreen":1)" },
			{ "input", R"(,"windowId":1,"action":"paste")" },
			{ "input", R"(,"windowId":1,"action":"key")" },
			{ "input", R"(,"windowId":1,"action":"mouse_button","x":1,"y":2,"button":9)" },
			{ "focus_state", R"(,"windowId":1)" },
			{ "occlusion", R"(,"windowId":1)" },
			{ "occlusion", R"(,"windowId":1,"visible":"no")" },
			{ "occlusion", R"(,"windowId":1.5,"visible":true)" },
			{ "occlusion", R"(,"visible":true)" },
		};
		const std::string plain = R"(,"arguments":[],"displaySelections":[])";
		{ /* A session with its own window accepts the messages and does nothing with them. */
			Connection c;
			c.command("start", plain + R"(,"performanceSupported":true)");
			eventually([&] { return c.bridge->performanceAvailable(); }, "Start not applied");
			for (const auto& [type, fields] : valid)
				c.command(type, fields);
			c.command("performance_control",
			          R"(,"performanceEnabled":false,"performanceOverlayVisible":false,"performanceGeneration":7)");
			eventually([&] { return c.bridge->performanceGeneration() == 7; }, "Reader barrier not applied");
			require(!c.bridge->cancelled(), "A valid hosted message ended a window session");
		}
		for (const auto& [type, fields] : malformed)
		{
			Connection c;
			c.command(type, fields);
			eventually([&] { return c.bridge->cancelled(); }, fields);
		}
		for (const auto start : { R"(,"renderMode":5)", R"(,"renderMode":"layer")", R"(,"hostedScale":2)",
		                          R"(,"renderMode":"window","hostedScale":2)",
		                          R"(,"renderMode":"hosted","hostedScale":0.1)" })
		{
			Connection c;
			c.command("start", plain + start);
			std::vector<std::string> arguments;
			std::string error;
			require(!c.bridge->prepare(arguments, error) && error == "Invalid start command", start);
			require(!SdlHosted::enabled(), "An invalid start turned hosted mode on");
		}
		{ /* Hosted without the launcher's port fails by name instead of opening a window. The
			 * slot is inherited, so whatever started this test may have left a port in it. */
			mach_port_t none = MACH_PORT_NULL;
			require(mach_ports_register(mach_task_self(), &none, 1) == KERN_SUCCESS,
			        "Cannot clear the registered ports");
			Connection c;
			c.command("start", plain + R"(,"renderMode":"hosted")");
			std::vector<std::string> arguments;
			std::string error;
			require(!c.bridge->prepare(arguments, error) &&
			            error.find("surface port is missing") != std::string::npos,
			        "Hosted start without a port was not refused");
			require(!SdlHosted::enabled(), "Hosted mode is on without a port");
		}

		/* The launcher's side of the handshake: a port in the registered slot. */
		mach_port_t port = MACH_PORT_NULL;
		require(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS &&
		            mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) ==
		                KERN_SUCCESS &&
		            mach_ports_register(mach_task_self(), &port, 1) == KERN_SUCCESS,
		        "Cannot register a surface port");
		Connection c;
		c.command("start", plain + R"(,"renderMode":"hosted","hostedScale":2)");
		std::vector<std::string> arguments;
		std::string error;
		require(c.bridge->prepare(arguments, error), "Hosted start with a port was refused");
		require(SdlHosted::enabled() && SdlHostedSurfaces::instance().connected() &&
		            SdlHosted::initialScale(1.0f) == 2.0f,
		        "Hosted start did not turn hosted mode on");
		require(getString(c.next().get(), "type") == "hello", "Missing hello");

		require(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy") && SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
		auto window = SDL_CreateWindow("hosted transport", 640, 480, SDL_WINDOW_HIDDEN);
		auto renderer = window ? SDL_CreateRenderer(window, "software") : nullptr;
		require(renderer != nullptr, SDL_GetError());
		const auto id = SDL_GetWindowID(window);
		const auto idField = ",\"windowId\":" + std::to_string(id);
		require(SdlHosted::setLayout(id, { 1280, 960, 2.0f }), "Cannot lay out the test window");
		std::set<Uint32> seen;
		std::vector<SDL_Event> keys;
		const auto service = [&] {
			c.bridge->serviceHosted();
			SDL_Event event = {};
			while (SDL_PollEvent(&event))
			{
				if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST &&
				    event.window.windowID == id)
					seen.insert(event.type);
				if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
					keys.push_back(event);
			}
		};
		const auto number = [](WINPR_JSON* json, const char* key) {
			return WINPR_JSON_GetNumberValue(WINPR_JSON_GetObjectItemCaseSensitive(json, key));
		};
		const auto expectWindow = [&](int width, int height) {
			auto event = c.next();
			require(getString(event.get(), "type") == "hosted_window" &&
			            number(event.get(), "windowId") == id && number(event.get(), "width") == width &&
			            number(event.get(), "height") == height && number(event.get(), "scale") == 2,
			        "Wrong hosted_window event");
		};
		service();
		expectWindow(1280, 960);

		/* Layout in points becomes pixels; the fullscreen state arrives as the window event. */
		c.command("layout", idField + R"(,"width":800,"height":500,"scale":2,"fullscreen":true)");
		eventually([&] { service(); return SdlHosted::layout(id)->width == 1600; }, "Layout not applied");
		expectWindow(1600, 1000);
		eventually([&] { service(); return seen.count(SDL_EVENT_WINDOW_ENTER_FULLSCREEN) == 1; },
		           "Layout did not enter fullscreen");
		require(seen.count(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) == 1, "Layout did not resize");
		seen.clear();
		c.command("layout", idField + R"(,"width":800,"height":500,"scale":2)");
		c.command("layout", idField + R"(,"width":640,"height":400,"scale":2,"fullscreen":false)");
		c.command("layout", idField + R"(,"width":700,"height":400,"scale":2)");
		eventually([&] { service(); return SdlHosted::layout(id)->width == 1400; }, "Layouts not applied");
		eventually([&] { service(); return seen.count(SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) == 1; },
		           "A replaced layout lost its fullscreen state");
		require(seen.count(SDL_EVENT_WINDOW_ENTER_FULLSCREEN) == 0, "Fullscreen entered twice");

		/* Occlusion: the events a shown window gets, once per change, known windows only. */
		seen.clear();
		c.command("occlusion", R"(,"windowId":999999,"visible":false)");
		c.command("occlusion", idField + R"(,"visible":false)");
		c.command("occlusion", idField + R"(,"visible":false)");
		eventually([&] { service(); return !SdlHosted::visible(id); }, "Occlusion not applied");
		eventually([&] { service(); return seen.count(SDL_EVENT_WINDOW_HIDDEN) == 1; }, "No hidden event");
		require(SdlHosted::allHidden() && SdlHosted::visible(999999), "Wrong hidden windows");
		c.command("occlusion", idField + R"(,"visible":true)");
		eventually([&] { service(); return SdlHosted::visible(id); }, "Window not visible again");
		eventually([&] { service(); return seen.count(SDL_EVENT_WINDOW_EXPOSED) == 1; }, "No exposed event");
		require(!SdlHosted::allHidden(), "Still all hidden");

		/* Input becomes SDL events; losing focus releases what is held. */
		c.command("input", idField + R"(,"action":"key","keyCode":0,"down":true)");
		eventually([&] { service(); return keys.size() == 1; }, "Key not delivered");
		require(keys[0].type == SDL_EVENT_KEY_DOWN && keys[0].key.scancode == SDL_SCANCODE_A &&
		            keys[0].key.windowID == id,
		        "Wrong key event");
		c.command("focus_state", idField + R"(,"focused":false)");
		eventually([&] { service(); return keys.size() == 2; }, "Held key not released");
		require(keys[1].type == SDL_EVENT_KEY_UP && keys[1].key.scancode == SDL_SCANCODE_A,
		        "Wrong release event");

		/* What only the launcher can do to its window is asked for, never done. */
		const auto pending = [&] {
			pollfd fd{ c.peer, POLLIN, 0 };
			return c.buffered.find('\n') != std::string::npos || poll(&fd, 1, 50) > 0;
		};
		while (pending())
			std::ignore = c.next(); // hosted_window events of the layouts above
		const auto expectRequest = [&](const char* action) {
			auto event = c.next();
			require(getString(event.get(), "type") == "hosted_request" &&
			            number(event.get(), "windowId") == id && getString(event.get(), "action") == action,
			        action);
		};
		require(SdlHosted::minimize(window), "Minimize request failed");
		expectRequest("minimize");
		require(SdlHosted::fullscreen(window, true) && SdlHosted::fullscreen(window, false),
		        "Fullscreen request failed");
		expectRequest("enter_fullscreen");
		expectRequest("leave_fullscreen");
		require((SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) == 0 &&
		            (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0,
		        "A request changed the hidden window");

		/* A window that goes away is reported, and its state does not outlive it. */
		c.command("occlusion", idField + R"(,"visible":false)");
		eventually([&] { service(); return !SdlHosted::visible(id); }, "Occlusion not applied");
		SdlHosted::forget(id);
		service();
		auto closed = c.next();
		require(getString(closed.get(), "type") == "hosted_window_closed" &&
		            number(closed.get(), "windowId") == id,
		        "Missing hosted_window_closed");
		require(SdlHosted::visible(id) && !SdlHosted::allHidden(), "Visibility outlived the window");
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
	}
}
int main()
{
	try
	{
		handshakeCapabilities();
		mouseWheelOptions();
		tlsSecretsOptions();
		promptAndStaleResponse();
		concurrentPromptsAndCertificate();
		malformedAndVersion();
		lifecycle();
		terminalDistinctions();
		reconnectPolicyExclusions();
		performanceOverlayStyles();
		windowGeometryOptions();
		screenRefresh();
		closeConfirmation();
		closeCancellationAndEOF();
		thumbnailBoundsAndDisable();
		thumbnailBackpressureTeardown();
		windowGeometryReporting();
		hostedRendering();
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
	std::cout
	    << "Launcher transport prompt, framing, EOF, cancellation, lifecycle, close confirmation, and thumbnail tests passed\n";
	return 0;
}
