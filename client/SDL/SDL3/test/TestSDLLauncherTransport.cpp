/** Actual private socket protocol regression tests; never contacts an RDP host. */
#include "../sdl_launcher.hpp"
#include <freerdp/error.h>
#include <winpr/crt.h>
#include <future>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
			c.bridge->ended(2, "Synthetic remote logoff");
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

}
int main()
{
	try
	{
		promptAndStaleResponse();
		concurrentPromptsAndCertificate();
		malformedAndVersion();
		lifecycle();
		terminalDistinctions();
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
	std::cout
	    << "Launcher transport prompt, framing, EOF, cancellation, and lifecycle tests passed\n";
	return 0;
}
