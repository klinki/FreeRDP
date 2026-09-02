/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client
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

#include "sdl_monitor_scale.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace
{
	[[nodiscard]] bool parse_uint64(std::string_view value, uint64_t& result)
	{
		if (value.empty())
			return false;

		const auto first = value.data();
		const auto last = first + value.size();
		const auto parsed = std::from_chars(first, last, result, 10);
		return parsed.ec == std::errc{} && parsed.ptr == last;
	}

	[[nodiscard]] std::string quoted(std::string_view value)
	{
		return "'" + std::string(value) + "'";
	}
} // namespace

bool sdl_parse_monitor_scale_overrides(const char* value, SdlMonitorScaleOverrides& overrides,
                                       std::string& error)
{
	if (!value || *value == '\0')
	{
		error = "/sdl-monitor-scale requires at least one monitor scale override";
		return false;
	}

	SdlMonitorScaleOverrides parsed;
	std::string_view input(value);
	size_t start = 0;
	while (start <= input.size())
	{
		const auto comma = input.find(',', start);
		const auto end = comma == std::string_view::npos ? input.size() : comma;
		const auto item = input.substr(start, end - start);

		const auto equal = item.find('=');
		const auto slash = item.find('/');
		if (item.empty() || equal == std::string_view::npos || slash == std::string_view::npos ||
		    equal == 0 || slash <= equal + 1 || slash + 1 >= item.size() ||
		    item.find('=', equal + 1) != std::string_view::npos ||
		    item.find('/', slash + 1) != std::string_view::npos)
		{
			error = "invalid /sdl-monitor-scale entry " + quoted(item) +
			        "; expected <id>=<desktop>/<device>";
			return false;
		}

		uint64_t displayId = 0;
		if (!parse_uint64(item.substr(0, equal), displayId) || displayId == 0)
		{
			error = "invalid SDL monitor ID " + quoted(item.substr(0, equal)) +
			        "; it must be a positive integer";
			return false;
		}
		if (displayId > std::numeric_limits<SDL_DisplayID>::max())
		{
			error = "SDL monitor ID " + quoted(item.substr(0, equal)) + " is out of range";
			return false;
		}

		uint64_t desktopScaleFactor = 0;
		if (!parse_uint64(item.substr(equal + 1, slash - equal - 1), desktopScaleFactor) ||
		    desktopScaleFactor < 100 || desktopScaleFactor > 500)
		{
			error = "invalid desktop scale factor in /sdl-monitor-scale entry " + quoted(item) +
			        "; expected an integer in the range 100..500";
			return false;
		}

		uint64_t deviceScaleFactor = 0;
		if (!parse_uint64(item.substr(slash + 1), deviceScaleFactor) ||
		    (deviceScaleFactor != 100 && deviceScaleFactor != 140 && deviceScaleFactor != 180))
		{
			error = "invalid device scale factor in /sdl-monitor-scale entry " + quoted(item) +
			        "; expected one of 100, 140, or 180";
			return false;
		}

		const auto id = static_cast<SDL_DisplayID>(displayId);
		const auto result =
		    parsed.emplace(id, SdlMonitorScaleOverride{ id, static_cast<UINT32>(desktopScaleFactor),
		                                                static_cast<UINT32>(deviceScaleFactor) });
		if (!result.second)
		{
			error = "duplicate SDL monitor ID " + quoted(item.substr(0, equal)) +
			        " in /sdl-monitor-scale";
			return false;
		}

		if (comma == std::string_view::npos)
			break;
		start = comma + 1;
	}

	overrides = std::move(parsed);
	return true;
}

bool sdl_apply_monitor_scale_override(const SdlMonitorScaleOverrides& overrides,
                                      rdpMonitor& monitor)
{
	const auto it = overrides.find(monitor.orig_screen);
	if (it == overrides.end())
		return false;

	monitor.attributes.desktopScaleFactor = it->second.desktopScaleFactor;
	monitor.attributes.deviceScaleFactor = it->second.deviceScaleFactor;
	return true;
}
