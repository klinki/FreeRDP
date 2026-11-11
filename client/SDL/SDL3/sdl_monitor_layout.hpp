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
#include <climits>
#include <cstdlib>
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

	/* Length of the edge that b shares with a monitor before it on the axis (a's end is b's
	 * start), or 0. */
	inline int64_t sharedEdge(const rdpMonitor& a, const rdpMonitor& b, bool vertical)
	{
		const int64_t apos = vertical ? a.y : a.x, alen = vertical ? a.height : a.width;
		const int64_t bpos = vertical ? b.y : b.x;
		if (apos + alen != bpos)
			return 0;
		const int64_t ac = vertical ? a.x : a.y, acl = vertical ? a.width : a.height;
		const int64_t bc = vertical ? b.x : b.y, bcl = vertical ? b.width : b.height;
		return std::max<int64_t>(0, std::min(ac + acl, bc + bcl) - std::max(ac, bc));
	}

	/* One axis of the compaction. list holds the effective rectangles, displays the full
	 * display rectangles at the same origins, which tell which monitors touch.
	 *
	 * In order along the axis, each monitor
	 * - that touched monitors before it on the axis moves to the end of the furthest of them,
	 * - otherwise, that touched a monitor on its side, moves as far as that neighbor's start did,
	 *   or its end when the contact was in the part the neighbor lost (the neighbor with the
	 *   longest shared edge that has already been placed),
	 * - otherwise stays,
	 * but never moves before the end of a placed monitor that shares part of its range on the
	 * other axis. Monitors only move towards the start, so a layout without overlaps keeps none. */
	inline void closeAxis(std::vector<rdpMonitor>& list, const std::vector<rdpMonitor>& displays,
	                      bool vertical)
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
			const auto bi = order[i];
			const auto& b = list[bi];
			bool anchored = false;
			int64_t target = pos(b);
			int64_t floor = INT64_MIN;
			int64_t side = 0;
			int64_t sideShift = 0;
			for (size_t j = 0; j < i; j++)
			{
				const auto ai = order[j];
				const auto& a = list[ai];
				if (sharedEdge(displays[ai], displays[bi], vertical) > 0)
				{
					const auto end = moved[ai] + len(a);
					target = anchored ? std::max(target, end) : end;
					anchored = true;
				}
				const auto edge = sharedEdge(displays[ai], displays[bi], !vertical) +
				                  sharedEdge(displays[bi], displays[ai], !vertical);
				if (edge > side)
				{
					side = edge;
					// Follow the neighbor's start, or its end when the contact was in the part
					// the neighbor lost.
					sideShift = moved[ai] - pos(a);
					if (!spans(moved[ai], len(a), pos(b) + sideShift, len(b)))
						sideShift = moved[ai] + len(a) - pos(displays[ai]) - len(displays[ai]);
				}
				if ((pos(a) + len(a) <= pos(b)) &&
				    spans(cross(a), crossLen(a), cross(b), crossLen(b)))
					floor = std::max(floor, moved[ai] + len(a));
			}
			if (!anchored && (side > 0))
				target = pos(b) + sideShift;
			moved[bi] = std::min(pos(b), std::max(target, floor));
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

namespace sdl_monitor_layout
{
	inline std::vector<rdpMonitor> flipped(std::vector<rdpMonitor> list)
	{
		for (auto& m : list)
			m.y = -(m.y + m.height);
		return list;
	}

	inline int64_t displacement(const std::vector<rdpMonitor>& a, const std::vector<rdpMonitor>& b)
	{
		int64_t sum = 0;
		for (size_t x = 0; x < a.size(); x++)
			sum += std::llabs(int64_t{ a[x].x } - b[x].x) + std::llabs(int64_t{ a[x].y } - b[x].y);
		return sum;
	}

	inline std::vector<rdpMonitor> compact(std::vector<rdpMonitor> list,
	                                       const std::vector<rdpMonitor>& displays)
	{
		closeAxis(list, displays, true);
		closeAxis(list, displays, false);
		return list;
	}
} // namespace sdl_monitor_layout

/* A fullscreen window can be smaller than its display, for example below the notch of a MacBook,
 * while the origins still come from the full display bounds. Its monitor can then lose contact
 * with the monitors that touched its display, and the server ignores layouts whose monitors are
 * not adjacent (MS-RDPEDISP 3.1.5.2). displays holds the display rectangles in the same order as
 * list.
 *
 * Two arrangements are tried. In the first, a shortened monitor keeps the top of its display and
 * the monitors move up and then left to touch again what their displays touched. In the second, a
 * shortened monitor keeps the bottom of its display, where the drawable of a notched display is,
 * and the monitors move down instead. The valid one that moves the monitors least is used,
 * preferring the first. If neither is valid, the first one without overlaps is used, else list
 * keeps its positions. Returns true when the result is a valid layout. */
inline bool sdl_close_monitor_gaps(std::vector<rdpMonitor>& list,
                                   const std::vector<rdpMonitor>& displays)
{
	using namespace sdl_monitor_layout;
	if (displays.size() != list.size())
		return valid(list);

	const auto top = compact(list, displays);
	auto bottomStart = list;
	for (size_t x = 0; x < list.size(); x++)
		bottomStart[x].y = displays[x].y + displays[x].height - list[x].height;
	const auto bottom = flipped(compact(flipped(bottomStart), flipped(displays)));

	const bool topValid = valid(top);
	const bool bottomValid = valid(bottom);
	if (topValid && (!bottomValid || (displacement(top, list) <= displacement(bottom, list))))
		list = top;
	else if (bottomValid)
		list = bottom;
	else if (!overlapping(top))
		list = top;
	else if (!overlapping(bottom))
		list = bottom;
	return valid(list);
}
