/**
 * FreeRDP SDL3 private native launcher bridge.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include "sdl_launcher.hpp"
#include "sdl_utils.hpp"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ColorSync/ColorSync.h>
#include <ImageIO/ImageIO.h>
#include <freerdp/crypto/crypto.h>
#include <cctype>
#include <winpr/crt.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <freerdp/error.h>
#include <freerdp/crypto/certificate.h>

namespace
{
	SdlLauncher* launcher = nullptr;
	constexpr unsigned bridgeProtocolVersion = 1;
	constexpr const char* bridgeCapabilities[] = {
		"auth", "certificate", "focus", "retry", "display_uuid", "per_monitor_scaling",
		"dynamic_resolution", "multimon", "close_confirmation", "session_thumbnail",
		"dock_accessory"
	};
	// The nonconnecting query and the live handshake must advertise the same features.
	bool addCapabilities(WINPR_JSON* message)
	{
		auto array = WINPR_JSON_AddArrayToObject(message, "capabilities");
		if (!array)
			return false;
		for (const auto capability : bridgeCapabilities)
		{
			SdlLauncher::Json item(WINPR_JSON_CreateString(capability), WINPR_JSON_Delete);
			if (!item || !WINPR_JSON_AddItemToArray(array, item.get()))
				return false;
			std::ignore = item.release();
		}
		return true;
	}
	constexpr size_t maxFrame = 1024 * 1024;
	constexpr size_t maxThumbnailPNG = 262144;
	// Only owned, already downsampled BGRA pixels reach the encoding worker.
	std::string thumbnailPNG(const std::vector<BYTE>& pixels, UINT32 width, UINT32 height)
	{
		std::string result;
		auto color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		auto provider = CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size(), nullptr);
		auto image = color && provider
		                 ? CGImageCreate(width, height, 8, 32, width * 4, color,
		                                 kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
		                                 provider, nullptr, false, kCGRenderingIntentDefault)
		                 : nullptr;
		auto data = CFDataCreateMutable(kCFAllocatorDefault, 0);
		auto destination = image && data
		                       ? CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr)
		                       : nullptr;
		if (destination)
		{
			CGImageDestinationAddImage(destination, image, nullptr);
			if (CGImageDestinationFinalize(destination))
			{
				const auto size = CFDataGetLength(data);
				if (size > 0 && static_cast<size_t>(size) <= maxThumbnailPNG)
				{
					std::unique_ptr<char, decltype(&free)> encoded(
					    crypto_base64_encode(CFDataGetBytePtr(data), static_cast<size_t>(size)), free);
					if (encoded && strlen(encoded.get()) <= 350000)
						result = encoded.get();
				}
			}
			CFRelease(destination);
		}
		if (data) CFRelease(data);
		if (image) CGImageRelease(image);
		if (provider) CGDataProviderRelease(provider);
		if (color) CGColorSpaceRelease(color);
		return result;
	}
	WINPR_JSON* item(WINPR_JSON* obj, const char* key)
	{
		return WINPR_JSON_GetObjectItemCaseSensitive(obj, key);
	}
	std::string str(WINPR_JSON* obj, const char* key)
	{
		const auto value = WINPR_JSON_GetStringValue(item(obj, key));
		return value ? value : "";
	}
	void text(WINPR_JSON* obj, const char* key, const char* value)
	{
		std::ignore = WINPR_JSON_AddStringToObject(obj, key, value ? value : "");
	}
	void number(WINPR_JSON* obj, const char* key, double value)
	{
		std::ignore = WINPR_JSON_AddNumberToObject(obj, key, value);
	}
	void boolean(WINPR_JSON* obj, const char* key, bool value)
	{
		std::ignore = WINPR_JSON_AddBoolToObject(obj, key, value);
	}
	std::string thumbprint(const char* value, DWORD flags)
	{
		if (!value)
			return {};
		if (!(flags & VERIFY_CERT_FLAG_FP_IS_PEM))
			return value;
		auto cert = freerdp_certificate_new_from_pem(value);
		if (!cert)
			return {};
		char* fingerprint = freerdp_certificate_get_fingerprint(cert);
		const std::string result = fingerprint ? fingerprint : "";
		free(fingerprint);
		freerdp_certificate_free(cert);
		return result;
	}
	// Password and bridge overrides are never accepted as FreeRDP arguments in managed mode.
	bool allowedArgument(const std::string& value)
	{
		if (value.empty() || (value.front() != '/' && value.front() != '+' && value.front() != '-'))
			return false; // No .rdp files, response files, positional paths, or embedded
			              // credentials.
		const auto begin = value.find_first_not_of("/+-");
		const auto separator = value.find_first_of(":=", begin);
		if (begin == std::string::npos)
			return false;
		auto key = value.substr(begin, separator - begin);
		std::transform(key.begin(), key.end(), key.begin(),
		               [](unsigned char c) { return std::tolower(c); });
		const char* blocked[] = { "p",
			                      "password",
			                      "gp",
			                      "gateway",
			                      "g",
			                      "pth",
			                      "from-stdin",
			                      "args-from",
			                      "credentials-delegation",
			                      "reconnect-cookie",
			                      "assistance",
			                      "gat",
			                      "proxy",
			                      "auth-only",
			                      "smartcard",
			                      "smartcard-logon",
			                      "aad",
			                      "azure",
			                      "remote-app",
			                      "app",
			                      "load-balance-info",
			                      "tune",
			                      "cert",
			                      "cert-ignore",
			                      "cert-tofu",
			                      "cert-name",
			                      "cert-deny",
			                      "sec-ext",
			                      "restricted-admin",
			                      "restrictedadmin",
			                      "remote-credential-guard",
			                      "authentication",
			                      "force-console-callbacks",
			                      "list",
			                      "help",
			                      "version",
			                      "buildconfig",
			                      "monitors",
			                      "sdl-monitor-scale",
			                      "launcher-fd",
			                      "launcher-session",
			                      "log-level",
			                      "log-filters" };
		for (const auto entry : blocked)
			if (key == entry)
				return false;
		std::string options = separator == std::string::npos ? "" : value.substr(separator + 1);
		std::transform(options.begin(), options.end(), options.begin(),
		               [](unsigned char c) { return std::tolower(c); });
		if ((key == "sec" && (options.find("ext") != std::string::npos ||
		                      options.find("aad") != std::string::npos)) ||
		    (key == "tls" && options.find("secrets") != std::string::npos))
			return false;
		return key.rfind("launcher", 0) != 0;
	}
}

SdlLauncher::SdlLauncher(int fd, std::string sessionId, rdpContext* context)
    : _fd(fd), _sessionId(std::move(sessionId)), _context(context)
{
	launcher = this;
	int yes = 1;
	std::ignore = setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
	_reader = std::thread(&SdlLauncher::readLoop, this);
	_writer = std::thread(&SdlLauncher::writeLoop, this);
	_thumbnailWorker = std::thread(&SdlLauncher::thumbnailLoop, this);
}
SdlLauncher::~SdlLauncher()
{
	if (!_terminal)
		terminal(_hadConnected ? "client_error" : "initial_failure", -1,
		         "Client did not complete the session lifecycle");
	{
		std::unique_lock lock(_mutex);
		_condition.wait_for(lock, std::chrono::milliseconds(750),
		                    [&] { return _outbound.empty() && !_writing; });
	}
	_closing = true;
	_condition.notify_all();
	shutdown(_fd, SHUT_RDWR);
	if (_reader.joinable())
		_reader.join();
	if (_writer.joinable())
		_writer.join();
	if (_thumbnailWorker.joinable())
		_thumbnailWorker.join();
	close(_fd);
	launcher = nullptr;
}
SdlLauncher* SdlLauncher::active()
{
	return launcher;
}
SdlLauncher::Json SdlLauncher::capabilities()
{
	Json result(WINPR_JSON_CreateObject(), WINPR_JSON_Delete);
	if (!result || !WINPR_JSON_AddIntegerToObject(result.get(), "schemaVersion", 1) ||
	    !WINPR_JSON_AddStringToObject(result.get(), "client", "sdl3") ||
	    !WINPR_JSON_AddStringToObject(result.get(), "engineVersion", freerdp_get_version_string()) ||
	    !WINPR_JSON_AddIntegerToObject(result.get(), "bridgeProtocolVersion", bridgeProtocolVersion) ||
	    !addCapabilities(result.get()))
		return { nullptr, WINPR_JSON_Delete };
	return result;
}
SdlLauncher::Json SdlLauncher::message(const char* type)
{
	Json result(WINPR_JSON_CreateObject(), WINPR_JSON_Delete);
	number(result.get(), "v", bridgeProtocolVersion);
	text(result.get(), "type", type);
	text(result.get(), "sessionId", _sessionId.c_str());
	return result;
}
bool SdlLauncher::send(Json event)
{
	std::unique_ptr<char, decltype(&free)> encoded(WINPR_JSON_PrintUnformatted(event.get()), free);
	if (!encoded)
		return false;
	std::lock_guard lock(_mutex);
	if (_closing || _outbound.size() >= 128)
		return false;
	_outbound.emplace_back(std::string(encoded.get()) + "\n");
	_condition.notify_all();
	return true;
}
void SdlLauncher::writeLoop()
{
	while (!_closing)
	{
		std::string frame;
		{
			std::unique_lock lock(_mutex);
			_condition.wait(lock, [&] { return _closing || !_outbound.empty(); });
			if (_closing)
				break;
			frame = std::move(_outbound.front());
			_outbound.pop_front();
			_writing = true;
		}
		size_t offset = 0;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!_closing && offset < frame.size())
		{
			pollfd fd{ _fd, POLLOUT, 0 };
			const int ready = poll(&fd, 1, 100);
			if (ready < 0 && errno == EINTR)
				continue;
			if (ready < 0 || std::chrono::steady_clock::now() > deadline)
			{
				cancel();
				break;
			}
			if (ready == 0)
				continue;
			const auto n = ::send(_fd, frame.data() + offset, frame.size() - offset, MSG_DONTWAIT);
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (n <= 0)
			{
				cancel();
				break;
			}
			offset += static_cast<size_t>(n);
		}
		{
			std::lock_guard lock(_mutex);
			_writing = false;
		}
		_condition.notify_all();
	}
}
void SdlLauncher::readLoop()
{
	std::string buffer;
	char bytes[4096];
	while (!_closing && !_cancelled)
	{
		const auto n = recv(_fd, bytes, sizeof(bytes), 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		buffer.append(bytes, static_cast<size_t>(n));
		memset(bytes, 0, sizeof(bytes));
		if (buffer.size() > maxFrame && buffer.find('\n') == std::string::npos)
			break;
		size_t end = 0;
		while ((end = buffer.find('\n')) != std::string::npos)
		{
			if (end == 0 || end > maxFrame || buffer.find('\0', 0) < end)
			{
				cancel();
				return;
			}
			Json event(WINPR_JSON_ParseWithLength(buffer.data(), end), WINPR_JSON_Delete);
			std::fill(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(end), '\0');
			buffer.erase(0, end + 1);
			if (!receive(std::move(event)))
			{
				cancel();
				return;
			}
		}
	}
	std::fill(buffer.begin(), buffer.end(), '\0');
	if (!_closing)
		cancel();
}
bool SdlLauncher::receive(Json event)
{
	if (!event || !WINPR_JSON_IsObject(event.get()) ||
	    WINPR_JSON_GetNumberValue(item(event.get(), "v")) != 1)
		return false;
	if (str(event.get(), "sessionId") != _sessionId)
		return true;
	const auto type = str(event.get(), "type");
	if (type == "cancel")
	{
		cancel();
		return true;
	}
	if (type == "focus")
	{
		_focus = true;
		return true;
	}
	if (type == "close_response")
	{
		bool accepted = false;
		{
			std::lock_guard lock(_mutex);
			const auto value = item(event.get(), "accepted");
			if (_closeRequest.empty() || str(event.get(), "requestId") != _closeRequest ||
			    !WINPR_JSON_IsBool(value))
				return true;
			accepted = WINPR_JSON_IsTrue(value);
			_closeRequest.clear();
		}
		if (accepted) cancel();
		return true;
	}
	if (type == "thumbnail_request")
	{
		std::lock_guard lock(_mutex);
		const auto enabled = item(event.get(), "thumbnailsEnabled");
		if (WINPR_JSON_IsFalse(enabled))
		{
			_thumbnailEnabled = false;
			++_thumbnailEpoch;
			_thumbnail = {};
			_thumbnailRequest.clear();
			_outbound.erase(std::remove_if(_outbound.begin(), _outbound.end(), [](const auto& frame) {
				return frame.find("\"type\":\"thumbnail\"") != std::string::npos;
			}), _outbound.end());
			_condition.notify_all();
			return true;
		}
		const auto id = str(event.get(), "requestId");
		const auto now = std::chrono::steady_clock::now();
		if (!_connected || _cancelled || _terminal || id.empty() || id.size() > 128 ||
		    now - _lastThumbnailRequest < std::chrono::seconds(2))
			return true;
		_lastThumbnailRequest = now;
		_thumbnailEnabled = true;
		_thumbnailRequest = id; // At most one pending request and one owned frame.
		_condition.notify_all();
		return true;
	}
	std::lock_guard lock(_mutex);
	if (type == "start")
	{
		if (_started)
			return false;
		const auto confirm = item(event.get(), "confirmSessionClose");
		const auto thumbnails = item(event.get(), "thumbnailsEnabled");
		if ((confirm && !WINPR_JSON_IsBool(confirm)) ||
		    (thumbnails && !WINPR_JSON_IsBool(thumbnails)))
			return false;
		_confirmSessionClose = WINPR_JSON_IsTrue(confirm);
		_thumbnailEnabled = WINPR_JSON_IsTrue(thumbnails);
		_started = true;
		_start = std::move(event);
	}
	else if (type == "auth_response" || type == "certificate_response")
	{
		const auto found = _pending.find(str(event.get(), "requestId"));
		if (found == _pending.end())
			return true; // Ignore late/duplicate decisions.
		if (found->second.type != type || found->second.response)
			return true;
		found->second.response = std::move(event);
	}
	else
		return true; // Future/unknown commands cannot alter the current session.
	_condition.notify_all();
	return true;
}
SdlLauncher::Json SdlLauncher::request(Json event, const char* responseType)
{
	if (_cancelled)
		return Json(nullptr, WINPR_JSON_Delete);
	std::string id;
	{
		std::lock_guard lock(_mutex);
		id = _sessionId + ":" + std::to_string(++_nextRequest);
		_pending.emplace(id, Pending{ responseType, Json(nullptr, WINPR_JSON_Delete) });
	}
	text(event.get(), "requestId", id.c_str());
	if (!send(std::move(event)))
		cancel();
	std::unique_lock lock(_mutex);
	_condition.wait(lock, [&] { return _cancelled || _pending.at(id).response != nullptr; });
	auto result = std::move(_pending.at(id).response);
	_pending.erase(id);
	return result;
}
void SdlLauncher::cancel()
{
	if (_cancelled.exchange(true))
		return;
	{
		std::lock_guard lock(_mutex);
		_thumbnailEnabled = false;
		++_thumbnailEpoch;
		_thumbnail = {};
		_thumbnailRequest.clear();
		_closeRequest.clear();
		_outbound.erase(std::remove_if(_outbound.begin(), _outbound.end(), [](const auto& frame) {
			return frame.find("\"type\":\"thumbnail\"") != std::string::npos;
		}), _outbound.end());
	}
	_condition.notify_all();
	std::ignore = freerdp_abort_connect_context(_context);
	std::ignore = sdl_push_quit();
}
bool SdlLauncher::closeConfirmationEnabled() const
{
	return _confirmSessionClose;
}
bool SdlLauncher::thumbnailsEnabled() const
{
	return _thumbnailEnabled;
}
bool SdlLauncher::requestClose()
{
	if (!_confirmSessionClose || _cancelled || _terminal)
	{
		cancel();
		return false;
	}
	std::string id;
	{
		std::lock_guard lock(_mutex);
		if (!_closeRequest.empty())
			return false;
		id = _sessionId + ":close:" + std::to_string(++_nextRequest);
		_closeRequest = id;
	}
	yieldActivationToLauncher();
	auto event = message("close_request");
	text(event.get(), "requestId", id.c_str());
	if (!send(std::move(event))) cancel();
	return true;
}
bool SdlLauncher::captureThumbnail(const BYTE* bgra, UINT32 width, UINT32 height, UINT32 stride)
{
	if (!bgra || width == 0 || height == 0 || width > 65536 || height > 65536 ||
	    stride < static_cast<UINT64>(width) * 4 || !_thumbnailEnabled || !_connected ||
	    _cancelled || _terminal)
		return false;
	UINT64 epoch;
	{
		std::lock_guard lock(_mutex);
		const auto now = std::chrono::steady_clock::now();
		if (now - _lastThumbnailFrame < std::chrono::seconds(1))
			return false;
		_lastThumbnailFrame = now;
		epoch = _thumbnailEpoch;
	}
	const double scale = std::min({ 1.0, 320.0 / width, 200.0 / height });
	Thumbnail frame;
	frame.width = std::max(1u, static_cast<UINT32>(width * scale));
	frame.height = std::max(1u, static_cast<UINT32>(height * scale));
	frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4);
	for (UINT32 y = 0; y < frame.height; y++)
	{
		const auto sourceY = static_cast<UINT64>(y) * height / frame.height;
		for (UINT32 x = 0; x < frame.width; x++)
		{
			const auto sourceX = static_cast<UINT64>(x) * width / frame.width;
			const auto source = bgra + sourceY * stride + sourceX * 4;
			auto target = frame.pixels.data() + (static_cast<size_t>(y) * frame.width + x) * 4;
			memcpy(target, source, 3);
			target[3] = 255;
		}
	}
	{
		std::lock_guard lock(_mutex);
		if (!_thumbnailEnabled || _cancelled || _terminal || epoch != _thumbnailEpoch)
			return false;
		_thumbnail = std::move(frame);
	}
	_condition.notify_all();
	return true;
}
void SdlLauncher::thumbnailLoop()
{
	while (!_closing)
	{
		Thumbnail frame;
		std::string id;
		UINT64 epoch;
		{
			std::unique_lock lock(_mutex);
			_condition.wait(lock, [&] {
				return _closing || (_thumbnailEnabled && _connected && !_cancelled && !_terminal &&
				                    !_thumbnailRequest.empty() && !_thumbnail.pixels.empty());
			});
			if (_closing) break;
			frame = _thumbnail;
			id = std::move(_thumbnailRequest);
			_thumbnailRequest.clear();
			epoch = _thumbnailEpoch;
		}
		const auto png = thumbnailPNG(frame.pixels, frame.width, frame.height);
		if (png.empty()) continue;
		auto event = message("thumbnail");
		text(event.get(), "requestId", id.c_str());
		text(event.get(), "pngBase64", png.c_str());
		number(event.get(), "width", frame.width);
		number(event.get(), "height", frame.height);
		std::unique_ptr<char, decltype(&free)> encoded(WINPR_JSON_PrintUnformatted(event.get()), free);
		if (!encoded) continue;
		{
			std::lock_guard lock(_mutex);
			if (_closing || _cancelled || _terminal || !_thumbnailEnabled || !_connected ||
			    epoch != _thumbnailEpoch || _outbound.size() >= 128)
				continue;
			_outbound.emplace_back(std::string(encoded.get()) + "\n");
		}
		_condition.notify_all();
	}
}
bool SdlLauncher::cancelled() const
{
	return _cancelled;
}
bool SdlLauncher::hadConnected() const
{
	return _hadConnected;
}

std::vector<std::pair<std::string, UINT32>> SdlLauncher::displays(WINPR_JSON* array)
{
	std::vector<std::pair<std::string, UINT32>> result;
	CGDirectDisplayID cgIds[64];
	uint32_t count = 0;
	if (CGGetActiveDisplayList(64, cgIds, &count) != kCGErrorSuccess)
		return result;
	int n = 0;
	auto ids = SDL_GetDisplays(&n);
	for (int i = 0; ids && i < n; i++)
	{
		SDL_Rect bounds{};
		if (!SDL_GetDisplayBounds(ids[i], &bounds))
			continue;
		CGDirectDisplayID matched = 0;
		size_t matches = 0;
		for (uint32_t j = 0; j < count; j++)
		{
			const auto rect = CGDisplayBounds(cgIds[j]);
			if (std::abs(rect.origin.x - bounds.x) < 0.5 &&
			    std::abs(rect.origin.y - bounds.y) < 0.5 &&
			    std::abs(rect.size.width - bounds.w) < 0.5 &&
			    std::abs(rect.size.height - bounds.h) < 0.5)
			{
				matched = cgIds[j];
				matches++;
			}
		}
		std::string uuid;
		if (matches == 1)
		{
			auto ref = CGDisplayCreateUUIDFromDisplayID(matched);
			auto value = ref ? CFUUIDCreateString(kCFAllocatorDefault, ref) : nullptr;
			char chars[128]{};
			if (value && CFStringGetCString(value, chars, sizeof(chars), kCFStringEncodingUTF8))
				uuid = chars;
			if (value)
				CFRelease(value);
			if (ref)
				CFRelease(ref);
		}
		result.emplace_back(uuid, ids[i]);
		if (array)
		{
			auto obj = WINPR_JSON_CreateObject();
			number(obj, "id", ids[i]);
			text(obj, "uuid", uuid.c_str());
			text(obj, "name", SDL_GetDisplayName(ids[i]));
			number(obj, "x", bounds.x);
			number(obj, "y", bounds.y);
			number(obj, "width", bounds.w);
			number(obj, "height", bounds.h);
			const auto mode = SDL_GetCurrentDisplayMode(ids[i]);
			const double density = mode ? mode->pixel_density : SDL_GetDisplayContentScale(ids[i]);
			number(obj, "pixelWidth", std::round(bounds.w * density));
			number(obj, "pixelHeight", std::round(bounds.h * density));
			number(obj, "scale", SDL_GetDisplayContentScale(ids[i]));
			std::ignore = WINPR_JSON_AddItemToArray(array, obj);
		}
	}
	SDL_free(ids);
	return result;
}
bool SdlLauncher::prepare(std::vector<std::string>& arguments, std::string& error)
{
	auto hello = message("hello");
	text(hello.get(), "engineVersion", freerdp_get_version_string());
	if (!addCapabilities(hello.get()))
	{
		error = "Cannot encode launcher capabilities";
		return false;
	}
	std::ignore = displays(WINPR_JSON_AddArrayToObject(hello.get(), "displays"));
	if (!send(std::move(hello)))
	{
		error = "Cannot send launcher handshake";
		return false;
	}
	Json start(nullptr, WINPR_JSON_Delete);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!_cancelled)
	{
		{
			std::unique_lock lock(_mutex);
			if (_start)
			{
				start = std::move(_start);
				break;
			}
			_condition.wait_for(lock, std::chrono::milliseconds(20));
		}
		SDL_PumpEvents(); // Initialization and all display APIs stay on the SDL main thread.
		if (std::chrono::steady_clock::now() > deadline)
		{
			error = "Launcher did not send a start command";
			return false;
		}
	}
	if (!start)
		return false;
	auto args = item(start.get(), "arguments");
	auto selections = item(start.get(), "displaySelections");
	if (!WINPR_JSON_IsArray(args) || !WINPR_JSON_IsArray(selections) ||
	    WINPR_JSON_GetArraySize(args) > 256)
	{
		error = "Invalid start command";
		return false;
	}
	for (size_t i = 0; i < WINPR_JSON_GetArraySize(args); i++)
	{
		auto value = WINPR_JSON_GetStringValue(WINPR_JSON_GetArrayItem(args, i));
		if (!value || strlen(value) > 8192 || !allowedArgument(value))
		{
			error = "Unsupported or unsafe launcher argument";
			return false;
		}
		arguments.emplace_back(value);
	}
	const auto current = displays(nullptr); // Resolve again immediately before connection.
	std::vector<UINT32> selected;
	std::string monitorArg, scaleArg;
	for (size_t i = 0; i < WINPR_JSON_GetArraySize(selections); i++)
	{
		auto selection = WINPR_JSON_GetArrayItem(selections, i);
		const auto uuid = str(selection, "uuid");
		auto match = current.end();
		size_t count = 0;
		for (auto it = current.begin(); it != current.end(); ++it)
			if (!uuid.empty() && strcasecmp(it->first.c_str(), uuid.c_str()) == 0)
			{
				match = it;
				count++;
			}
		if (count != 1 ||
		    std::find(selected.begin(), selected.end(), match->second) != selected.end())
		{
			error =
			    "Selected display is unavailable or ambiguous. Reselect displays in the launcher.";
			return false;
		}
		const auto id = match->second;
		selected.push_back(id);
		if (!monitorArg.empty())
			monitorArg += ",";
		monitorArg += std::to_string(id);
		auto desktop = item(selection, "desktopScale"), device = item(selection, "deviceScale");
		if (desktop || device)
		{
			const double d = desktop ? WINPR_JSON_GetNumberValue(desktop) : 100;
			const double v = device ? WINPR_JSON_GetNumberValue(device) : 100;
			if (d < 100 || d > 500 || std::floor(d) != d || (v != 100 && v != 140 && v != 180))
			{
				error = "Unsupported per-monitor scale";
				return false;
			}
			if (!scaleArg.empty())
				scaleArg += ",";
			scaleArg += std::to_string(id) + "=" + std::to_string(static_cast<int>(d)) + "/" +
			            std::to_string(static_cast<int>(v));
		}
	}
	if (!monitorArg.empty())
		arguments.push_back("/monitors:" + monitorArg);
	if (!scaleArg.empty())
	{
		for (const auto& arg : arguments)
		{
			std::string option = arg;
			std::transform(option.begin(), option.end(), option.begin(),
			               [](unsigned char c) { return std::tolower(c); });
			const auto begin = option.find_first_not_of("/+-");
			if (begin != std::string::npos && option.compare(begin, 5, "scale") == 0)
			{
				error = "Global and per-monitor scaling cannot be combined";
				return false;
			}
		}
		arguments.push_back("/sdl-monitor-scale:" + scaleArg);
	}
	return true;
}
bool SdlLauncher::authenticate(char** username, char** password, char** domain,
                               rdp_auth_reason reason, bool rejected)
{
	if (_cancelled)
		return false;
	if (!rejected && _credentialsRead && *username && *password)
		return true;
	auto event = message("auth_request");
	number(event.get(), "reason", reason);
	boolean(event.get(), "rejected", rejected);
	text(event.get(), "target", freerdp_settings_get_server_name(_context->settings));
	text(event.get(), "username", *username);
	text(event.get(), "domain", *domain);
	auto response = request(std::move(event), "auth_response");
	if (!response || !WINPR_JSON_IsTrue(item(response.get(), "accepted")))
	{
		cancel();
		return false;
	}
	const auto u = str(response.get(), "username"), d = str(response.get(), "domain");
	auto secret = WINPR_JSON_GetStringValue(item(response.get(), "password"));
	if (!secret || u.empty())
		return false;
	auto nextU = _strdup(u.c_str()), nextD = _strdup(d.c_str()), nextP = _strdup(secret);
	if (!nextU || !nextD || !nextP)
	{
		free(nextU);
		free(nextD);
		free(nextP);
		return false;
	}
	if (*password)
		SecureZeroMemory(*password, strlen(*password));
	free(*username);
	free(*domain);
	free(*password);
	*username = nextU;
	*domain = nextD;
	*password = nextP;
	SecureZeroMemory(const_cast<char*>(secret), strlen(secret));
	_credentialsRead = true;
	return true;
}
DWORD SdlLauncher::certificate(const char* host, UINT16 port, const char* commonName,
                               const char* subject, const char* issuer, const char* fingerprint,
                               const char* oldSubject, const char* oldIssuer,
                               const char* oldFingerprint, DWORD flags)
{
	auto event = message("certificate_request");
	text(event.get(), "kind", oldFingerprint ? "changed" : "unknown");
	text(event.get(), "host", host);
	number(event.get(), "port", port);
	text(event.get(), "commonName", commonName);
	text(event.get(), "subject", subject);
	text(event.get(), "issuer", issuer);
	text(event.get(), "fingerprint", thumbprint(fingerprint, flags).c_str());
	if (oldFingerprint)
	{
		text(event.get(), "oldSubject", oldSubject);
		text(event.get(), "oldIssuer", oldIssuer);
		text(event.get(), "oldFingerprint", thumbprint(oldFingerprint, flags).c_str());
	}
	auto response = request(std::move(event), "certificate_response");
	if (!response)
		return 0;
	const auto decision = str(response.get(), "decision");
	if (decision == "accept_permanently")
		return 1;
	if (decision == "accept_once")
		return 2;
	return 0;
}
SSIZE_T SdlLauncher::retry(size_t current, const char* module)
{
	const auto settings = _context->settings;
	const auto max = freerdp_settings_get_uint32(settings, FreeRDP_AutoReconnectMaxRetries);
	if (_cancelled || !freerdp_settings_get_bool(settings, FreeRDP_AutoReconnectionEnabled) ||
	    (max > 0 && current >= max))
		return -1;
	const auto delay = freerdp_settings_get_uint32(settings, FreeRDP_TcpConnectTimeout);
	_retryCount = current + 1;
	auto event = message("retry");
	text(event.get(), "module", module);
	number(event.get(), "attempt", current + 1);
	number(event.get(), "maxAttempts", max);
	number(event.get(), "delayMs", delay);
	std::ignore = send(std::move(event));
	return static_cast<SSIZE_T>(delay);
}
void SdlLauncher::state(const char* phase)
{
	_connected = strcmp(phase, "connected") == 0;
	if (strcmp(phase, "connected") == 0)
	{
		_hadConnected = true;
		_retryCount = 0;
	}
	auto event = message("state");
	text(event.get(), "phase", phase);
	std::ignore = send(std::move(event));
}
bool SdlLauncher::isAuthenticationError(UINT32 error)
{
	switch (error)
	{
		case FREERDP_ERROR_AUTHENTICATION_FAILED:
		case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
		case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
		case FREERDP_ERROR_CONNECT_ACCESS_DENIED:
		case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
		case FREERDP_ERROR_CONNECT_CLIENT_REVOKED:
		case FREERDP_ERROR_CONNECT_ACCOUNT_RESTRICTION:
		case FREERDP_ERROR_CONNECT_ACCOUNT_LOCKED_OUT:
		case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
			return true;
		default:
			return false;
	}
}
void SdlLauncher::terminal(const char* outcome, int code, const std::string& detail)
{
	if (_terminal.exchange(true))
		return;
	auto event = message("ended");
	text(event.get(), "outcome", outcome);
	const auto lastError = freerdp_get_last_error(_context);
	number(event.get(), "errorCode",
	       lastError != FREERDP_ERROR_SUCCESS
	           ? lastError
	           : (code < 0 ? FREERDP_ERROR_CONNECT_FAILED : FREERDP_ERROR_SUCCESS));
	number(event.get(), "errorInfo", freerdp_error_info(_context->instance));
	boolean(event.get(), "hadConnected", _hadConnected);
	text(event.get(), "detail", detail.c_str());
	std::ignore = send(std::move(event));
}
void SdlLauncher::setupFailed(const std::string& detail)
{
	terminal("initial_failure", -1, detail);
}
void SdlLauncher::ended(int exitCode, const std::string& detail)
{
	const char* outcome = "client_error";
	const auto info = freerdp_error_info(_context->instance);
	if (_cancelled || exitCode == sdl::error::CONNECT_CANCELLED ||
	    exitCode == sdl::error::DISCONNECT_BY_USER)
		outcome = "cancelled";
	else if (exitCode == sdl::error::LOGOFF || info == ERRINFO_LOGOFF_BY_USER)
		outcome = "remote_logoff";
	else if (!_hadConnected)
		outcome = isAuthenticationError(freerdp_get_last_error(_context)) ? "authentication_failed"
		                                                                  : "initial_failure";
	else if (isAuthenticationError(freerdp_get_last_error(_context)))
		outcome = "authentication_failed";
	else if (_retryCount > 0)
		outcome = "retries_exhausted";
	else if (exitCode >= 0)
		outcome = "disconnected";
	terminal(outcome, exitCode, detail);
}
void SdlLauncher::serviceFocus()
{
	if (!_focus.exchange(false))
		return;
	activateAccessorySession();
	int count = 0;
	auto windows = SDL_GetWindows(&count);
	for (int i = 0; windows && i < count; i++)
	{
		std::ignore = SDL_RestoreWindow(windows[i]);
		std::ignore = SDL_RaiseWindow(windows[i]);
	}
	SDL_free(windows);
}
