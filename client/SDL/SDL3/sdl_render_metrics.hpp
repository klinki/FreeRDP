/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL client rendering metrics
 *
 * Copyright 2026 FreeRDP contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

/*
 * This recorder is deliberately independent of SDL.  A renderer can pass
 * the clipped area and the bytes submitted to SDL without making this helper
 * know about SDL_Rect, pixel formats, or a particular SDL version.
 *
 * Set FREERDP_SDL_RENDER_METRICS to a writable JSONL path to enable it.  An
 * unset, empty, or unopenable path disables the recorder.  Disabled calls
 * only test a cached bool and do not read the environment, take a lock, or
 * read the clock.
 *
 * All *_wall_ns values are elapsed wall-clock durations measured with a
 * monotonic clock.  They are not CPU time.  The caller owns one recorder per
 * window and must call it from the UI/render thread only; no internal
 * cross-thread aggregation is performed.
 */

class SdlRenderMetrics final
{
  public:
	using ClockFunction = uint64_t (*)(void* opaque) noexcept;

	static constexpr uint64_t intervalNs = 1000000000ULL;
	static constexpr size_t maxSamples = 256;
	/* Version 2 adds per-window skip/churn counters. All version 1 keys
	 * are unchanged so identical-input A/B comparisons stay comparable. */
	static constexpr unsigned schemaVersion = 2;

	class Timer final
	{
	  public:
		Timer() = default;
		Timer(const Timer&) = delete;
		Timer& operator=(const Timer&) = delete;

		Timer(Timer&& other) noexcept
		    : _owner(other._owner), _kind(other._kind), _pixels(other._pixels), _bytes(other._bytes),
		      _started(other._started)
		{
			other._owner = nullptr;
		}

		Timer& operator=(Timer&& other) noexcept
		{
			if (this != &other)
			{
				stop();
				_owner = other._owner;
				_kind = other._kind;
				_pixels = other._pixels;
				_bytes = other._bytes;
				_started = other._started;
				other._owner = nullptr;
			}
			return *this;
		}

		~Timer() { stop(); }

		void stop() noexcept
		{
			if (!_owner)
				return;

			const auto now = _owner->nowNs();
			const auto elapsed = now >= _started ? now - _started : 0;
			switch (_kind)
			{
				case Kind::Upload:
					_owner->noteUpload(_pixels, _bytes, elapsed);
					break;
				case Kind::Draw:
					_owner->noteDraw(elapsed);
					break;
				case Kind::Present:
					_owner->notePresent(elapsed);
					break;
			}
			_owner = nullptr;
		}

  private:
		enum class Kind : uint8_t
		{
			Upload,
			Draw,
			Present
		};

		friend class SdlRenderMetrics;
		Timer(SdlRenderMetrics* owner, Kind kind, uint64_t pixels, uint64_t bytes) noexcept
		    : _owner(owner), _kind(kind), _pixels(pixels), _bytes(bytes),
		      _started(owner ? owner->nowNs() : 0)
		{
		}

		SdlRenderMetrics* _owner = nullptr;
		Kind _kind = Kind::Draw;
		uint64_t _pixels = 0;
		uint64_t _bytes = 0;
		uint64_t _started = 0;
	};

	/* clock and opaque are intended for deterministic unit tests.  Production
	 * callers can use the two-argument constructor. */
	explicit SdlRenderMetrics(uint64_t windowId = 0, uint64_t monitorId = 0,
	                          ClockFunction clock = nullptr, void* opaque = nullptr) noexcept
	    : _windowId(windowId), _monitorId(monitorId), _clock(clock ? clock : &steadyNowNs),
	      _clockOpaque(opaque)
	{
		const auto* path = std::getenv("FREERDP_SDL_RENDER_METRICS");
		if (!path || path[0] == '\0')
			return;

		/* O_APPEND allows multiple per-window recorders to use one path.  The
		 * process-local mutex below prevents their stdio writes from interleaving. */
		_file = std::fopen(path, "ab");
		_enabled = _file != nullptr;
	}

	SdlRenderMetrics(const SdlRenderMetrics&) = delete;
	SdlRenderMetrics& operator=(const SdlRenderMetrics&) = delete;

	SdlRenderMetrics(SdlRenderMetrics&& other) noexcept
	    : _windowId(other._windowId), _monitorId(other._monitorId), _clock(other._clock),
	      _clockOpaque(other._clockOpaque), _file(other._file), _enabled(other._enabled),
	      _intervalActive(other._intervalActive), _intervalStartNs(other._intervalStartNs),
	      _frameActive(other._frameActive), _frameStartNs(other._frameStartNs),
	      _lastPresentValid(other._lastPresentValid), _lastPresentNs(other._lastPresentNs),
	      _frames(other._frames), _attemptedDirtyPixels(other._attemptedDirtyPixels),
	      _uploadPixels(other._uploadPixels), _uploadBytes(other._uploadBytes),
	      _uploadCalls(other._uploadCalls), _uploadWallNs(other._uploadWallNs),
	      _drawCalls(other._drawCalls), _drawWallNs(other._drawWallNs),
	      _presentCalls(other._presentCalls), _presentWallNs(other._presentWallNs),
	      _presentSkips(other._presentSkips), _targetRecreates(other._targetRecreates),
	      _gdiRecreates(other._gdiRecreates), _topBarDraws(other._topBarDraws),
	      _stalledPresents(other._stalledPresents),
	      _redrawSamples(other._redrawSamples), _frameIntervalSamples(other._frameIntervalSamples)
	{
		other._file = nullptr;
		other._enabled = false;
		other._intervalActive = false;
		other._frameActive = false;
	}

	SdlRenderMetrics& operator=(SdlRenderMetrics&& other) noexcept
	{
		if (this != &other)
		{
			close();
			_windowId = other._windowId;
			_monitorId = other._monitorId;
			_clock = other._clock;
			_clockOpaque = other._clockOpaque;
			_file = other._file;
			_enabled = other._enabled;
			_intervalActive = other._intervalActive;
			_intervalStartNs = other._intervalStartNs;
			_frameActive = other._frameActive;
			_frameStartNs = other._frameStartNs;
			_lastPresentValid = other._lastPresentValid;
			_lastPresentNs = other._lastPresentNs;
			_frames = other._frames;
			_attemptedDirtyPixels = other._attemptedDirtyPixels;
			_uploadPixels = other._uploadPixels;
			_uploadBytes = other._uploadBytes;
			_uploadCalls = other._uploadCalls;
			_uploadWallNs = other._uploadWallNs;
			_drawCalls = other._drawCalls;
			_drawWallNs = other._drawWallNs;
			_presentCalls = other._presentCalls;
			_presentWallNs = other._presentWallNs;
			_presentSkips = other._presentSkips;
			_targetRecreates = other._targetRecreates;
			_gdiRecreates = other._gdiRecreates;
			_topBarDraws = other._topBarDraws;
			_stalledPresents = other._stalledPresents;
			_redrawSamples = other._redrawSamples;
			_frameIntervalSamples = other._frameIntervalSamples;

			other._file = nullptr;
			other._enabled = false;
			other._intervalActive = false;
			other._frameActive = false;
		}
		return *this;
	}

	~SdlRenderMetrics() { close(); }

	[[nodiscard]] bool enabled() const noexcept { return _enabled; }
	[[nodiscard]] uint64_t nowNs() const noexcept
	{
		return _enabled ? _clock(_clockOpaque) : 0;
	}

	void beginFrame(uint64_t attemptedDirtyPixels) noexcept
	{
		if (!_enabled)
			return;

		const auto now = nowNs();
		if (!_intervalActive || (now >= _intervalStartNs && now - _intervalStartNs >= intervalNs))
		{
			if (_intervalActive)
				writeInterval(now);
			if (!_enabled)
				return;
			startInterval(now);
		}

		/* A frame is one redraw attempt. The pixel count is the sum supplied by
		 * the caller and intentionally retains rectangle multiplicity; it is not
		 * a union-area calculation. */
		++_frames;
		_attemptedDirtyPixels += attemptedDirtyPixels;
		_frameActive = true;
		_frameStartNs = now;
	}

	void endFrame() noexcept
	{
		if (!_enabled || !_frameActive)
			return;

		const auto now = nowNs();
		_recordSample(_redrawSamples, now >= _frameStartNs ? now - _frameStartNs : 0);
		_frameActive = false;
	}

	[[nodiscard]] Timer beginUpload(uint64_t uploadedPixels, uint64_t uploadedBytes) noexcept
	{
		return _enabled ? Timer(this, Timer::Kind::Upload, uploadedPixels, uploadedBytes) : Timer{};
	}

	[[nodiscard]] Timer beginDraw() noexcept
	{
		return _enabled ? Timer(this, Timer::Kind::Draw, 0, 0) : Timer{};
	}

	[[nodiscard]] Timer beginPresent() noexcept
	{
		return _enabled ? Timer(this, Timer::Kind::Present, 0, 0) : Timer{};
	}

	/* Direct hooks are useful where the caller already has a duration. */
	void noteUpload(uint64_t uploadedPixels, uint64_t uploadedBytes, uint64_t wallNs) noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		_uploadPixels += uploadedPixels;
		_uploadBytes += uploadedBytes;
		++_uploadCalls;
		_uploadWallNs += wallNs;
	}

	void noteDraw(uint64_t wallNs) noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		++_drawCalls;
		_drawWallNs += wallNs;
	}

	void notePresent(uint64_t wallNs) noexcept
	{
		if (!_enabled)
			return;
		const auto now = nowNs();
		ensureInterval(now);
		++_presentCalls;
		_presentWallNs += wallNs;
		if (_lastPresentValid)
			_recordSample(_frameIntervalSamples,
			              now >= _lastPresentNs ? now - _lastPresentNs : 0);
		_lastPresentNs = now;
		_lastPresentValid = true;
	}

	/* Version 2 counters. All run on the UI/render thread only. Stalled
	 * overlay presents are counted separately from present_calls so that
	 * identical-input comparisons of present_calls stay stable; total
	 * onscreen presents are present_calls + stalled_presents. */
	void notePresentSkip() noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		++_presentSkips;
	}

	void noteTargetRecreate() noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		++_targetRecreates;
	}

	void noteGdiRecreate() noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		++_gdiRecreates;
	}

	void noteTopBarDraw() noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		++_topBarDraws;
	}

	void noteStalledPresent() noexcept
	{
		if (!_enabled)
			return;
		ensureInterval(nowNs());
		++_stalledPresents;
	}

	/* Shared appender for process-global records (e.g. the update-queue
	 * record written by SdlContext). Serialized with the same mutex as
	 * per-window intervals so stdio sequences never interleave. */
	static bool appendJsonLine(const char* line) noexcept
	{
		if (!line || line[0] == '\0')
			return false;
		const auto* path = std::getenv("FREERDP_SDL_RENDER_METRICS");
		if (!path || path[0] == '\0')
			return false;
		std::lock_guard<std::mutex> lock(outputMutex());
		FILE* file = std::fopen(path, "ab");
		if (!file)
			return false;
		const bool ok =
		    std::fputs(line, file) >= 0 && std::fputs("\n", file) >= 0 && std::fflush(file) == 0;
		std::fclose(file);
		return ok;
	}

	/* Force the current interval to disk. Normally the destructor and the next
	 * beginFrame do this automatically. */
	void flush() noexcept
	{
		if (!_enabled || !_intervalActive)
			return;
		writeInterval(nowNs());
		_intervalActive = false;
	}

	void setIdentity(uint64_t windowId, uint64_t monitorId) noexcept
	{
		if (_enabled && _intervalActive)
			flush();
		_windowId = windowId;
		_monitorId = monitorId;
		/* A present interval must never span two window/monitor identities. */
		_lastPresentValid = false;
	}

  private:
	struct Samples final
	{
		std::array<uint64_t, maxSamples> values{};
		size_t size = 0;
		uint64_t seen = 0;
		uint64_t state = 0x9e3779b97f4a7c15ULL;
	};

	static uint64_t steadyNowNs(void*) noexcept
	{
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		return static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
	}

	static std::mutex& outputMutex() noexcept
	{
		static std::mutex mutex;
		return mutex;
	}

	static void _recordSample(Samples& samples, uint64_t value) noexcept
	{
		++samples.seen;
		if (samples.size < maxSamples)
		{
			samples.values[samples.size++] = value;
			return;
		}

		/* Deterministic reservoir sampling keeps output bounded while retaining
		 * a representative distribution over long sessions. */
		samples.state = samples.state * 6364136223846793005ULL + 1442695040888963407ULL;
		const auto slot = samples.state % samples.seen;
		if (slot < maxSamples)
			samples.values[static_cast<size_t>(slot)] = value;
	}

	void startInterval(uint64_t now) noexcept
	{
		_intervalActive = true;
		_intervalStartNs = now;
		_frames = 0;
		_attemptedDirtyPixels = 0;
		_uploadPixels = 0;
		_uploadBytes = 0;
		_uploadCalls = 0;
		_uploadWallNs = 0;
		_drawCalls = 0;
		_drawWallNs = 0;
		_presentCalls = 0;
		_presentWallNs = 0;
		_presentSkips = 0;
		_targetRecreates = 0;
		_gdiRecreates = 0;
		_topBarDraws = 0;
		_stalledPresents = 0;
		_redrawSamples = Samples{};
		_frameIntervalSamples = Samples{};
	}

	void ensureInterval(uint64_t now) noexcept
	{
		if (!_intervalActive)
			startInterval(now);
	}

	static bool writeSamples(FILE* file, const Samples& samples) noexcept
	{
		if (std::fputc('[', file) == EOF)
			return false;
		for (size_t x = 0; x < samples.size; x++)
		{
			if ((x != 0 && std::fputc(',', file) == EOF) ||
			    std::fprintf(file, "%llu",
			                 static_cast<unsigned long long>(samples.values[x])) < 0)
				return false;
		}
		return std::fputc(']', file) != EOF;
	}

	void writeInterval(uint64_t now) noexcept
	{
		if (!_file || !_intervalActive)
			return;

		const auto duration = now >= _intervalStartNs ? now - _intervalStartNs : 0;
		std::lock_guard<std::mutex> lock(outputMutex());
		const auto written = std::fprintf(
		              _file,
		              "{\"schema\":\"freerdp.sdl_render_metrics\",\"version\":2,"
		              "\"window_id\":%llu,\"monitor_id\":%llu,"
		              "\"interval_start_ns\":%llu,\"interval_duration_ns\":%llu,"
		              "\"frames\":%llu,\"attempted_dirty_pixels\":%llu,"
		              "\"uploaded_pixels\":%llu,\"uploaded_bytes\":%llu,"
		              "\"upload_calls\":%llu,\"upload_wall_ns\":%llu,"
		              "\"draw_calls\":%llu,\"draw_wall_ns\":%llu,"
		              "\"present_calls\":%llu,\"present_wall_ns\":%llu,"
		              "\"present_skips\":%llu,\"target_recreates\":%llu,"
		              "\"gdi_recreates\":%llu,\"topbar_draws\":%llu,"
		              "\"stalled_presents\":%llu,"
		              "\"redraw_sample_count\":%llu,\"redraw_wall_ns_samples\":",
		              static_cast<unsigned long long>(_windowId),
		              static_cast<unsigned long long>(_monitorId),
		              static_cast<unsigned long long>(_intervalStartNs),
		              static_cast<unsigned long long>(duration),
		              static_cast<unsigned long long>(_frames),
		              static_cast<unsigned long long>(_attemptedDirtyPixels),
		              static_cast<unsigned long long>(_uploadPixels),
		              static_cast<unsigned long long>(_uploadBytes),
		              static_cast<unsigned long long>(_uploadCalls),
		              static_cast<unsigned long long>(_uploadWallNs),
		              static_cast<unsigned long long>(_drawCalls),
		              static_cast<unsigned long long>(_drawWallNs),
		              static_cast<unsigned long long>(_presentCalls),
		              static_cast<unsigned long long>(_presentWallNs),
		              static_cast<unsigned long long>(_presentSkips),
		              static_cast<unsigned long long>(_targetRecreates),
		              static_cast<unsigned long long>(_gdiRecreates),
		              static_cast<unsigned long long>(_topBarDraws),
		              static_cast<unsigned long long>(_stalledPresents),
		              static_cast<unsigned long long>(_redrawSamples.seen));
		bool ok = written >= 0 && writeSamples(_file, _redrawSamples) &&
		     std::fprintf(_file, ",\"frame_interval_sample_count\":%llu,\"frame_interval_wall_ns_samples\":",
		                  static_cast<unsigned long long>(_frameIntervalSamples.seen)) >= 0 &&
		     writeSamples(_file, _frameIntervalSamples) && std::fputs("}\n", _file) >= 0;
		if (ok && std::fflush(_file) == 0)
			return;

		/* Metrics output is diagnostic only. A write failure must never affect
		 * the session and should stop further output work. */
		std::fclose(_file);
		_file = nullptr;
		_enabled = false;
	}

	void close() noexcept
	{
		if (!_file)
			return;
		flush();
		if (_file)
		{
			std::fclose(_file);
			_file = nullptr;
		}
		_enabled = false;
	}

	uint64_t _windowId = 0;
	uint64_t _monitorId = 0;
	ClockFunction _clock = &steadyNowNs;
	void* _clockOpaque = nullptr;
	FILE* _file = nullptr;
	bool _enabled = false;
	bool _intervalActive = false;
	uint64_t _intervalStartNs = 0;
	bool _frameActive = false;
	uint64_t _frameStartNs = 0;
	bool _lastPresentValid = false;
	uint64_t _lastPresentNs = 0;

	uint64_t _frames = 0;
	uint64_t _attemptedDirtyPixels = 0;
	uint64_t _uploadPixels = 0;
	uint64_t _uploadBytes = 0;
	uint64_t _uploadCalls = 0;
	uint64_t _uploadWallNs = 0;
	uint64_t _drawCalls = 0;
	uint64_t _drawWallNs = 0;
	uint64_t _presentCalls = 0;
	uint64_t _presentWallNs = 0;
	uint64_t _presentSkips = 0;
	uint64_t _targetRecreates = 0;
	uint64_t _gdiRecreates = 0;
	uint64_t _topBarDraws = 0;
	uint64_t _stalledPresents = 0;
	Samples _redrawSamples;
	Samples _frameIntervalSamples;
};
