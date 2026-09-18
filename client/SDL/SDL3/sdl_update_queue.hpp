/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client pending redraws
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

#include <cstddef>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>

/* The framebuffer already contains the latest decoded pixels. Keep the union
 * of pending damage, not a history of frames to repaint. Only one SDL wakeup
 * may be outstanding; new damage during drawing schedules the next turn. */
class SdlUpdateQueue
{
  public:
	static constexpr size_t maxRects = 32;

	[[nodiscard]] bool push(const std::vector<SDL_Rect>& rects, Uint32 eventType)
	{
		std::lock_guard lock(_mutex);
		for (const auto& rect : rects)
			merge(rect);
		/* Consult SDL's queue rather than caching a pending flag: modal
		 * dialogs can consume our wakeup without drawing the desktop. */
		if (_rects.empty() || SDL_HasEvent(eventType))
			return true;

		SDL_Event event{};
		event.type = eventType;
		return SDL_PushEvent(&event);
	}

	[[nodiscard]] std::vector<SDL_Rect> pop()
	{
		std::lock_guard lock(_mutex);
		std::vector<SDL_Rect> rects;
		rects.swap(_rects);
		return rects;
	}

	void clear()
	{
		std::lock_guard lock(_mutex);
		_rects.clear();
	}

  private:
	void merge(SDL_Rect rect)
	{
		if (rect.w <= 0 || rect.h <= 0)
			return;

		/* Restart after merging: the larger bounding rectangle may now
		 * overlap an earlier entry. The list is always bounded. */
		for (size_t x = 0; x < _rects.size();)
		{
			if (!SDL_HasRectIntersection(&rect, &_rects[x]))
			{
				x++;
				continue;
			}
			SDL_Rect combined{};
			SDL_GetRectUnion(&rect, &_rects[x], &combined);
			rect = combined;
			_rects.erase(_rects.begin() + static_cast<ptrdiff_t>(x));
			x = 0;
		}

		/* Many disjoint tiles must not turn one UI turn into an unbounded
		 * number of texture uploads. A bounding box preserves all damage. */
		if (_rects.size() == maxRects)
		{
			for (const auto& pending : _rects)
			{
				SDL_Rect combined{};
				SDL_GetRectUnion(&rect, &pending, &combined);
				rect = combined;
			}
			_rects.clear();
		}
		_rects.push_back(rect);
	}

	std::mutex _mutex;
	std::vector<SDL_Rect> _rects;
};
