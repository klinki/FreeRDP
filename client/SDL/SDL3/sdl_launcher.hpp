/** Optional private protocol for the native macOS launcher. */
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <freerdp/freerdp.h>
#include <winpr/json.h>

class SdlLauncher
{
  public:
	using Json = std::unique_ptr<WINPR_JSON, decltype(&WINPR_JSON_Delete)>;
	SdlLauncher(int fd, std::string sessionId, rdpContext* context);
	~SdlLauncher();
	SdlLauncher(const SdlLauncher&) = delete;
	SdlLauncher& operator=(const SdlLauncher&) = delete;
	static SdlLauncher* active();
	// Pure discovery: no context, transport, SDL, display, or configuration access.
	static Json capabilities();
	static bool useAccessoryActivationPolicy(); // macOS main thread, after SDL_Init.
	bool prepare(std::vector<std::string>& arguments, std::string& error);
	bool authenticate(char** username, char** password, char** domain, rdp_auth_reason reason,
	                  bool rejected = false);
	DWORD certificate(const char* host, UINT16 port, const char* commonName, const char* subject,
	                  const char* issuer, const char* fingerprint, const char* oldSubject,
	                  const char* oldIssuer, const char* oldFingerprint, DWORD flags);
	SSIZE_T retry(size_t current, const char* module = "connection");
	void state(const char* phase);
	void ended(int exitCode, const std::string& detail);
	void setupFailed(const std::string& detail);
	void cancel();
	bool cancelled() const;
	bool hadConnected() const;
	void serviceFocus(); // SDL main thread only
	bool requestClose(); // SDL main thread: queues a decision; never waits.
	bool closeConfirmationEnabled() const;
	bool thumbnailsEnabled() const;
	// Composing RDP thread only, after the completed BGRA frame's writes finish.
	bool captureThumbnail(const BYTE* bgra, UINT32 width, UINT32 height, UINT32 stride);
	static bool isAuthenticationError(UINT32 error);

  private:
	Json message(const char* type);
	Json request(Json event, const char* responseType);
	bool send(Json event);
	void readLoop();
	void writeLoop();
	void thumbnailLoop();
	void yieldActivationToLauncher(); // SDL main thread, before a close request.
	void activateAccessorySession(); // SDL main thread, before raising its windows.
	bool receive(Json event);
	std::vector<std::pair<std::string, UINT32>> displays(WINPR_JSON* array);
	void terminal(const char* outcome, int code, const std::string& detail);
	int _fd;
	std::string _sessionId;
	rdpContext* _context;
	std::mutex _mutex;
	std::condition_variable _condition;
	std::deque<std::string> _outbound;
	Json _start{ nullptr, WINPR_JSON_Delete };
	struct Pending
	{
		std::string type;
		Json response{ nullptr, WINPR_JSON_Delete };
	};
	std::map<std::string, Pending> _pending;
	struct Thumbnail
	{
		std::vector<BYTE> pixels;
		UINT32 width = 0, height = 0;
	};
	Thumbnail _thumbnail;
	std::string _thumbnailRequest, _closeRequest;
	UINT64 _thumbnailEpoch = 0;
	std::chrono::steady_clock::time_point _lastThumbnailFrame{}, _lastThumbnailRequest{};
	std::thread _reader, _writer, _thumbnailWorker;
	std::atomic<bool> _confirmSessionClose{ false }, _thumbnailEnabled{ false }, _connected{ false };
	std::atomic<bool> _cancelled{ false }, _closing{ false }, _focus{ false };
	std::atomic<bool> _hadConnected{ false }, _terminal{ false };
	bool _credentialsRead = false;
	bool _writing = false;
	bool _started = false;
	UINT64 _nextRequest = 0;
	std::atomic<size_t> _retryCount{ 0 };
};
