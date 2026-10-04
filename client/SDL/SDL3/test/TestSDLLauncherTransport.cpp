/** Actual private socket protocol regression tests; never contacts an RDP host. */
#include "../sdl_launcher.hpp"
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
		closeConfirmation();
		closeCancellationAndEOF();
		thumbnailBoundsAndDisable();
		thumbnailBackpressureTeardown();
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
