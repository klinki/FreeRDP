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
#include <cmath>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

/* Pixel origins of the local displays, derived from their logical (point) bounds and their pixel
 * sizes. Each entry maps a display to its logical bounds and its pixel rectangle; only the pixel
 * origin is computed. */
namespace sdl_display_offsets
{
	using Offsets = std::map<SDL_DisplayID, std::pair<SDL_Rect, SDL_Rect>>;

	/* Two displays are neighbours when they share an edge segment of positive length; edges
	 * that are merely in line, e.g. a display diagonally below another, do not count. */
	inline bool spans(int a, int alen, int b, int blen)
	{
		return (a < b + blen) && (b < a + alen);
	}

	inline bool touchX(const SDL_Rect& a, const SDL_Rect& b)
	{
		return ((a.x + a.w == b.x) || (b.x + b.w == a.x)) && spans(a.y, a.h, b.y, b.h);
	}

	inline bool touchY(const SDL_Rect& a, const SDL_Rect& b)
	{
		return ((a.y + a.h == b.y) || (b.y + b.h == a.y)) && spans(a.x, a.w, b.x, b.w);
	}

	/* Distance in points, converted to pixels of the display it lies on. */
	inline int scaled(int distance, int logical, int pixel)
	{
		if (logical <= 0)
			return distance;
		return static_cast<int>(std::lround(static_cast<double>(distance) * pixel / logical));
	}

	/* Pixel position along the axis that does not touch. Without this, a monitor that is
	 * adjacent on one axis but shifted on the other (e.g. a wider monitor above another, or a
	 * rotated monitor that is not top aligned) keeps offset 0 on that axis, the layout sent to
	 * the server no longer matches the local one and pointer input lands offset by that
	 * difference.
	 *
	 * The offset between the two starts lies on whichever display starts first, so it is scaled
	 * by that display; with the reference's scale only, a 1x display hanging off a 2x one could
	 * lose contact with it. fromEnd keeps the offset between the two ends instead. */
	inline int crossPosition(int ePos, int eLogical, int ePixel, int fPos, int fLogical, int fPixel,
	                         int fPixelPos, bool fromEnd)
	{
		if (!fromEnd)
		{
			const int d = ePos - fPos;
			return fPixelPos +
			       ((d >= 0) ? scaled(d, fLogical, fPixel) : scaled(d, eLogical, ePixel));
		}
		const int d = (ePos + eLogical) - (fPos + fLogical);
		const int end = fPixelPos + fPixel +
		                ((d <= 0) ? scaled(d, fLogical, fPixel) : scaled(d, eLogical, ePixel));
		return end - ePixel;
	}

	/* Displays that are sent to the server; an empty list means all of them. */
	inline bool selected(const std::vector<SDL_DisplayID>& selection, SDL_DisplayID id)
	{
		return selection.empty() ||
		       (std::find(selection.begin(), selection.end(), id) != selection.end());
	}

	/* Only an overlap between two selected displays matters: the others are not sent to the
	 * server and only connect the selected ones. */
	inline bool overlapsPlaced(const Offsets& offsets, SDL_DisplayID id, const SDL_Rect& pixel,
	                           const std::vector<SDL_DisplayID>& placed,
	                           const std::vector<SDL_DisplayID>& selection)
	{
		if (!selected(selection, id))
			return false;
		for (const auto other : placed)
		{
			if ((other == id) || !selected(selection, other))
				continue;
			const auto& o = offsets.at(other).second;
			if (spans(pixel.x, pixel.w, o.x, o.w) && spans(pixel.y, pixel.h, o.y, o.h))
				return true;
		}
		return false;
	}

	inline bool isPlaced(const std::vector<SDL_DisplayID>& placed, SDL_DisplayID id)
	{
		return std::find(placed.begin(), placed.end(), id) != placed.end();
	}

	/* The two displays meet only at a corner. */
	inline bool touchCorner(const SDL_Rect& a, const SDL_Rect& b)
	{
		return ((a.x + a.w == b.x) || (b.x + b.w == a.x)) &&
		       ((a.y + a.h == b.y) || (b.y + b.h == a.y));
	}

	/* How the position along the shared edge is taken over: the offset between the starts,
	 * scaled by the display it lies on; the offset between the ends; or the offset between the
	 * starts in pixels of the display already placed. */
	enum class Cross
	{
		Start,
		End,
		Reference
	};

	/* Pixel rectangle of display e next to the placed display f, beside it (horizontal) or above
	 * or below it. */
	inline SDL_Rect placeNext(const std::pair<SDL_Rect, SDL_Rect>& e,
	                          const std::pair<SDL_Rect, SDL_Rect>& f, bool horizontal, Cross cross)
	{
		const auto& el = e.first;
		const auto& fl = f.first;
		const auto& fp = f.second;
		auto pixel = e.second;
		const auto along = [&](int ePos, int eLogical, int ePixel, int fPos, int fLogical,
		                       int fPixel, int fPixelPos)
		{
			if (cross == Cross::Reference)
				return fPixelPos + scaled(ePos - fPos, fLogical, fPixel);
			return crossPosition(ePos, eLogical, ePixel, fPos, fLogical, fPixel, fPixelPos,
			                     cross == Cross::End);
		};
		if (horizontal)
		{
			pixel.x = (el.x < fl.x) ? fp.x - pixel.w : fp.x + fp.w;
			pixel.y = along(el.y, el.h, pixel.h, fl.y, fl.h, fp.h, fp.y);
		}
		else
		{
			pixel.y = (el.y < fl.y) ? fp.y - pixel.h : fp.y + fp.h;
			pixel.x = along(el.x, el.w, pixel.w, fl.x, fl.w, fp.w, fp.x);
		}
		return pixel;
	}

	/* Next display to place: in walk order, the first one sharing an edge with a placed
	 * display, else the first one meeting a placed display at a corner, else the first one
	 * left. */
	inline SDL_DisplayID nextDisplay(const Offsets& offsets,
	                                 const std::vector<SDL_DisplayID>& placed, bool& corner)
	{
		corner = false;
		for (const auto f : placed)
		{
			const auto& fl = offsets.at(f).first;
			for (const auto& entry : offsets)
			{
				if (!isPlaced(placed, entry.first) &&
				    (touchX(entry.second.first, fl) || touchY(entry.second.first, fl)))
					return entry.first;
			}
		}
		corner = true;
		for (const auto f : placed)
		{
			const auto& fl = offsets.at(f).first;
			for (const auto& entry : offsets)
			{
				if (!isPlaced(placed, entry.first) && touchCorner(entry.second.first, fl))
					return entry.first;
			}
		}
		for (const auto& entry : offsets)
		{
			if (!isPlaced(placed, entry.first))
				return entry.first;
		}
		return 0;
	}

	/* Positions to try for display id, most faithful first: next to each placed display it
	 * shares an edge with (or, failing that, meets at a corner), with each way of taking over
	 * the position along the edge. A display that touches no placed display keeps its point
	 * offset from the reference display, in pixels of the reference. */
	inline std::vector<SDL_Rect> candidates(const Offsets& offsets,
	                                        const std::vector<SDL_DisplayID>& placed,
	                                        SDL_DisplayID id, bool corner,
	                                        const std::pair<SDL_Rect, SDL_Rect>& reference)
	{
		std::vector<SDL_Rect> result;
		const auto& e = offsets.at(id);
		for (const auto f : placed)
		{
			const auto& p = offsets.at(f);
			const bool cornerOnly = corner && touchCorner(e.first, p.first);
			const bool beside = touchX(e.first, p.first) || cornerOnly;
			const bool stacked = touchY(e.first, p.first) || cornerOnly;
			for (const bool horizontal : { true, false })
			{
				if (!(horizontal ? beside : stacked))
					continue;
				const auto add = [&](const SDL_Rect& pixel)
				{
					const auto same = [&](const SDL_Rect& r)
					{ return (r.x == pixel.x) && (r.y == pixel.y); };
					if (std::none_of(result.begin(), result.end(), same))
						result.push_back(pixel);
				};
				for (const auto cross : { Cross::Start, Cross::End, Cross::Reference })
					add(placeNext(e, p, horizontal, cross));

				/* Displays meeting at a corner in points may only meet at a corner in pixels
				 * too; last, slide along the edge until the nearer edges of the two are in
				 * line, so that they share part of it. */
				if (cornerOnly)
				{
					auto pixel = placeNext(e, p, horizontal, Cross::Start);
					const auto& f = p.second;
					if (horizontal)
						pixel.y = (e.first.y < p.first.y) ? f.y : f.y + f.h - pixel.h;
					else
						pixel.x = (e.first.x < p.first.x) ? f.x : f.x + f.w - pixel.w;
					add(pixel);
				}
			}
		}
		if (result.empty())
		{
			auto pixel = e.second;
			pixel.x = reference.second.x +
			          scaled(e.first.x - reference.first.x, reference.first.w, reference.second.w);
			pixel.y = reference.second.y +
			          scaled(e.first.y - reference.first.y, reference.first.h, reference.second.h);
			result.push_back(pixel);
		}
		return result;
	}

	/* The selected displays form one area, connected through shared pixel edges. */
	inline bool connected(const Offsets& offsets, const std::vector<SDL_DisplayID>& selection)
	{
		std::vector<SDL_DisplayID> ids;
		for (const auto& entry : offsets)
		{
			if (selected(selection, entry.first))
				ids.push_back(entry.first);
		}
		if (ids.size() < 2)
			return true;
		std::vector<SDL_DisplayID> reached{ ids.front() };
		for (size_t i = 0; i < reached.size(); i++)
		{
			const auto& a = offsets.at(reached[i]).second;
			for (const auto id : ids)
			{
				const auto& b = offsets.at(id).second;
				if (!isPlaced(reached, id) && (touchX(a, b) || touchY(a, b)))
					reached.push_back(id);
			}
		}
		return reached.size() == ids.size();
	}

	/* Upper bound on the positions tried before giving up on a valid layout. */
	constexpr size_t searchLimit = 100000;

	/* Places the remaining displays one by one, trying their candidate positions in order and
	 * going back to an earlier display when no position of a later one fits. Valid means that
	 * no two selected displays overlap and the selected displays are connected. */
	inline bool search(Offsets& offsets, std::vector<SDL_DisplayID>& placed,
	                   const std::vector<SDL_DisplayID>& selection,
	                   const std::pair<SDL_Rect, SDL_Rect>& reference, size_t& budget)
	{
		if (placed.size() == offsets.size())
			return connected(offsets, selection);
		bool corner = false;
		const auto id = nextDisplay(offsets, placed, corner);
		for (const auto& pixel : candidates(offsets, placed, id, corner, reference))
		{
			if (budget == 0)
				return false;
			budget--;
			if (overlapsPlaced(offsets, id, pixel, placed, selection))
				continue;
			offsets.at(id).second = pixel;
			placed.push_back(id);
			if (search(offsets, placed, selection, reference, budget))
				return true;
			placed.pop_back();
		}
		return false;
	}

	/* Pixel origins of all displays, starting with the primary at its point origin. Each
	 * display is placed next to a display it touches, keeping the point layout where the pixel
	 * sizes allow it. When the first choices give no valid layout, the others are tried; when
	 * none is valid, each display takes its first position that does not overlap (or its first
	 * one) and the layout check reports the problem. All displays take part, as one that is not
	 * selected can connect selected ones; selection lists the displays sent to the server
	 * (empty: all). */
	inline void update(Offsets& offsets, SDL_DisplayID primary,
	                   const std::vector<SDL_DisplayID>& selection = {})
	{
		if (offsets.empty())
			return;
		if (offsets.count(primary) == 0)
			primary = offsets.begin()->first;
		auto& origin = offsets.at(primary);
		origin.second.x = origin.first.x;
		origin.second.y = origin.first.y;
		const auto reference = origin;

		std::vector<SDL_DisplayID> placed{ primary };
		size_t budget = searchLimit;
		if (search(offsets, placed, selection, reference, budget))
			return;

		placed = { primary };
		while (placed.size() < offsets.size())
		{
			bool corner = false;
			const auto id = nextDisplay(offsets, placed, corner);
			const auto choices = candidates(offsets, placed, id, corner, reference);
			const auto fits =
			    std::find_if(choices.begin(), choices.end(), [&](const SDL_Rect& pixel)
			                 { return !overlapsPlaced(offsets, id, pixel, placed, selection); });
			offsets.at(id).second = (fits != choices.end()) ? *fits : choices.front();
			placed.push_back(id);
		}
	}
} // namespace sdl_display_offsets
