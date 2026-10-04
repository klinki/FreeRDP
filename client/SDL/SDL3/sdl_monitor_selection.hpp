/** Stable launcher monitor selection during hotplug. Apache-2.0. */
#pragma once
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "sdl_monitor_scale.hpp"

struct SdlMonitorSelection
{
	std::string uuid;
	std::optional<SdlMonitorScaleOverride> scale;
};
struct SdlResolvedMonitorSelection
{
	std::vector<SDL_DisplayID> ids;
	SdlMonitorScaleOverrides scales;
};
inline SdlResolvedMonitorSelection
sdl_resolve_monitor_selection(const std::vector<SdlMonitorSelection>& wanted,
                              const std::vector<std::pair<std::string, UINT32>>& available,
                              SDL_DisplayID fallback)
{
	SdlResolvedMonitorSelection result;
	for (const auto& selection : wanted)
	{
		const auto same = [&](const auto& display)
		{
			return !selection.uuid.empty() && selection.uuid.size() == display.first.size() &&
			       std::equal(selection.uuid.begin(), selection.uuid.end(), display.first.begin(),
			                  [](unsigned char a, unsigned char b)
			                  { return std::tolower(a) == std::tolower(b); });
		};
		if (std::count_if(available.begin(), available.end(), same) != 1)
			continue;
		const auto id = std::find_if(available.begin(), available.end(), same)->second;
		if (std::find(result.ids.begin(), result.ids.end(), id) != result.ids.end())
			continue;
		result.ids.push_back(id);
		if (selection.scale)
		{
			auto scale = *selection.scale;
			scale.displayId = id;
			result.scales.emplace(id, scale);
		}
	}
	if (result.ids.empty() && fallback != 0)
		result.ids.push_back(fallback);
	return result;
}
