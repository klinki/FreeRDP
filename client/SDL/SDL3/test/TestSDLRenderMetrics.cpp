/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL client rendering metrics tests
 *
 * Copyright 2026 FreeRDP contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "../sdl_render_metrics.hpp"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>

namespace
{
struct FakeClock final
{
	uint64_t now = 0;
	uint64_t reads = 0;

	static uint64_t read(void* opaque) noexcept
	{
		auto* clock = static_cast<FakeClock*>(opaque);
		++clock->reads;
		return clock->now;
	}
};

bool setMetricPath(const std::string& path)
{
#if defined(_WIN32)
	return _putenv_s("FREERDP_SDL_RENDER_METRICS", path.c_str()) == 0;
#else
	return setenv("FREERDP_SDL_RENDER_METRICS", path.c_str(), 1) == 0;
#endif
}

bool clearMetricPath()
{
#if defined(_WIN32)
	return _putenv_s("FREERDP_SDL_RENDER_METRICS", "") == 0;
#else
	return unsetenv("FREERDP_SDL_RENDER_METRICS") == 0;
#endif
}

struct TemporaryMetricsDirectory final
{
	std::filesystem::path path;

	~TemporaryMetricsDirectory()
	{
		std::ignore = clearMetricPath();
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

bool has(const std::string& line, const char* value)
{
	return line.find(value) != std::string::npos;
}
} // namespace

int main()
{
	std::error_code error;
	const auto base = std::filesystem::temp_directory_path(error);
	if (error)
		return 1;

	std::filesystem::path directory;
	const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
	for (uint64_t attempt = 0; attempt < 100 && directory.empty(); attempt++)
	{
		auto candidate = base / ("freerdp-sdl-render-metrics-" + std::to_string(serial) + "-" +
		                         std::to_string(attempt));
		if (std::filesystem::create_directory(candidate, error))
			directory = std::move(candidate);
	}
	if (directory.empty())
		return 1;
	TemporaryMetricsDirectory cleanup{ directory };
	const auto path = directory / "metrics.jsonl";

	if (!clearMetricPath())
		return 1;
	FakeClock disabledClock;
	SdlRenderMetrics disabled(0, 0, &FakeClock::read, &disabledClock);
	if (disabled.enabled() || disabled.nowNs() != 0)
		return 1;
	disabled.beginFrame(1);
	{
		auto timer = disabled.beginUpload(1, 4);
		timer.stop();
	}
	if (disabledClock.reads != 0)
		return 1;

#if defined(__linux__)
	if (!setMetricPath("/dev/full"))
		return 1;
	SdlRenderMetrics failedOutput(0, 0, &FakeClock::read, &disabledClock);
	failedOutput.beginFrame(1);
	failedOutput.flush();
	if (failedOutput.enabled())
		return 1;
#endif

	if (!setMetricPath(path.string()))
		return 1;

	FakeClock clock;
	{
		SdlRenderMetrics metrics(17, 2, &FakeClock::read, &clock);
		if (!metrics.enabled())
			return 1;

		clock.now = 100;
		metrics.beginFrame(100);
		clock.now = 200;
		{
			auto upload = metrics.beginUpload(80, 320);
			clock.now = 250;
		}
		metrics.noteDraw(12);
		clock.now = 300;
		{
			auto present = metrics.beginPresent();
			clock.now = 350;
		}
		clock.now = 400;
		metrics.endFrame();

		/* The next frame rotates the one-second interval before recording itself. */
		clock.now = SdlRenderMetrics::intervalNs + 500;
		metrics.beginFrame(200);
		clock.now += 50;
		{
			auto present = metrics.beginPresent();
			clock.now += 50;
		}
		metrics.endFrame();

		SdlRenderMetrics moved(std::move(metrics));
		if (metrics.enabled())
			return 1;
		moved.setIdentity(18, 3);
		clock.now += 100;
		moved.beginFrame(300);
		{
			auto present = moved.beginPresent();
			clock.now += 25;
		}
		moved.endFrame();
		moved.flush();
	} // The moved-from recorder must not flush a duplicate line.

	std::ifstream input(path);
	std::string first;
	std::string second;
	std::string third;
	if (!std::getline(input, first) || !std::getline(input, second) ||
	    !std::getline(input, third) || first.empty() || second.empty() || third.empty() ||
	    has(first, "\"window_id\":17") == false || has(first, "\"monitor_id\":2") == false ||
	    has(first, "\"attempted_dirty_pixels\":100") == false ||
	    has(first, "\"uploaded_pixels\":80") == false ||
	    has(first, "\"uploaded_bytes\":320") == false ||
	    has(first, "\"upload_calls\":1") == false ||
	    has(first, "\"upload_wall_ns\":50") == false ||
	    has(first, "\"draw_wall_ns\":12") == false ||
	    has(first, "\"present_wall_ns\":50") == false ||
	    has(first, "\"redraw_sample_count\":1") == false ||
	    has(second, "\"attempted_dirty_pixels\":200") == false ||
	    has(second, "\"frame_interval_sample_count\":1") == false ||
	    has(third, "\"window_id\":18") == false || has(third, "\"monitor_id\":3") == false ||
	    has(third, "\"attempted_dirty_pixels\":300") == false ||
	    has(third, "\"frame_interval_sample_count\":0") == false)
	{
		std::cerr << first << '\n' << second << '\n' << third << '\n';
		return 1;
	}

	std::string extra;
	if (std::getline(input, extra))
		return 1;
	input.close();
	return 0;
}
