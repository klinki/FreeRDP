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
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include <freerdp/settings_types.h>

namespace sdl_monitor_layout
{
	inline bool spans(int64_t a, int64_t alen, int64_t b, int64_t blen)
	{
		return (a < b + blen) && (b < a + alen);
	}

	inline bool intersect(const rdpMonitor& a, const rdpMonitor& b)
	{
		return spans(a.x, a.width, b.x, b.width) && spans(a.y, a.height, b.y, b.height);
	}

	/* Two monitors are adjacent when they share an edge segment of positive length. */
	inline bool touch(const rdpMonitor& a, const rdpMonitor& b)
	{
		const int64_t ax = a.x, ay = a.y, bx = b.x, by = b.y;
		if (((ax + a.width == bx) || (bx + b.width == ax)) && spans(ay, a.height, by, b.height))
			return true;
		return ((ay + a.height == by) || (by + b.height == ay)) && spans(ax, a.width, bx, b.width);
	}

	inline bool overlapping(const std::vector<rdpMonitor>& list)
	{
		for (size_t i = 0; i < list.size(); i++)
			for (size_t j = i + 1; j < list.size(); j++)
				if (intersect(list[i], list[j]))
					return true;
		return false;
	}

	/* MS-RDPEDISP 3.1.5.2: monitors must not overlap and each one must be adjacent to another. */
	inline bool valid(const std::vector<rdpMonitor>& list)
	{
		if (overlapping(list))
			return false;
		if (list.size() < 2)
			return true;
		for (size_t i = 0; i < list.size(); i++)
		{
			bool adjacent = false;
			for (size_t j = 0; (j < list.size()) && !adjacent; j++)
				adjacent = (i != j) && touch(list[i], list[j]);
			if (!adjacent)
				return false;
		}
		return true;
	}

	/* Moves each monitor up (vertical) or left until it touches the nearest monitor before it on
	 * that axis that shares part of its range on the other axis. The positions on the other axis
	 * are the current ones, so the second pass sees the moves of the first. A monitor only moves
	 * into space that no such monitor occupies, which keeps a non-overlapping layout free of
	 * overlaps. */
	inline void closeAxis(std::vector<rdpMonitor>& list, bool vertical)
	{
		const auto pos = [vertical](const rdpMonitor& m) -> int64_t
		{ return vertical ? m.y : m.x; };
		const auto len = [vertical](const rdpMonitor& m) -> int64_t
		{ return vertical ? m.height : m.width; };
		const auto cross = [vertical](const rdpMonitor& m) -> int64_t
		{ return vertical ? m.x : m.y; };
		const auto crossLen = [vertical](const rdpMonitor& m) -> int64_t
		{ return vertical ? m.width : m.height; };

		std::vector<size_t> order(list.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		std::stable_sort(order.begin(), order.end(),
		                 [&](size_t a, size_t b) { return pos(list[a]) < pos(list[b]); });
		std::vector<int64_t> moved(list.size());
		for (size_t x = 0; x < list.size(); x++)
			moved[x] = pos(list[x]);

		for (size_t i = 0; i < order.size(); i++)
		{
			const auto& b = list[order[i]];
			bool found = false;
			int64_t target = 0;
			for (size_t j = 0; j < i; j++)
			{
				const auto& a = list[order[j]];
				if ((pos(a) + len(a) > pos(b)) ||
				    !spans(cross(a), crossLen(a), cross(b), crossLen(b)))
					continue;
				const auto end = moved[order[j]] + len(a);
				target = found ? std::max(target, end) : end;
				found = true;
			}
			if (found && (target < moved[order[i]]))
				moved[order[i]] = target;
		}

		for (size_t x = 0; x < list.size(); x++)
		{
			if (vertical)
				list[x].y = static_cast<INT32>(moved[x]);
			else
				list[x].x = static_cast<INT32>(moved[x]);
		}
	}
} // namespace sdl_monitor_layout

/* A fullscreen window can be smaller than its display, for example below the notch of a MacBook,
 * while the origins still come from the full display bounds. A monitor that was below or right of
 * a shortened one would then be separated from it by a gap, and the server ignores layouts whose
 * monitors are not adjacent (MS-RDPEDISP 3.1.5.2). Move each monitor up and then left so that it
 * touches the nearest monitor before it on that axis again. If the result is not a valid layout,
 * keep the original one unless that is worse (overlapping, or also invalid while the result has no
 * overlap). Returns true when the compacted layout is used. */
inline bool sdl_close_monitor_gaps(std::vector<rdpMonitor>& list)
{
	const auto original = list;
	sdl_monitor_layout::closeAxis(list, true);
	sdl_monitor_layout::closeAxis(list, false);
	if (sdl_monitor_layout::valid(list))
		return true;
	if (sdl_monitor_layout::valid(original) || sdl_monitor_layout::overlapping(list))
	{
		list = original;
		return false;
	}
	return true;
}
