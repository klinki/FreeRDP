/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client redraw scheduling tests
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

#include "../sdl_update_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <thread>

namespace
{
	bool expect(bool condition, const char* message)
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	bool contains(const std::vector<SDL_Rect>& rects, const SDL_Rect& wanted)
	{
		return std::any_of(rects.begin(), rects.end(),
		                   [&](const SDL_Rect& rect)
		                   {
			                   return rect.x <= wanted.x && rect.y <= wanted.y &&
			                          rect.x + rect.w >= wanted.x + wanted.w &&
			                          rect.y + rect.h >= wanted.y + wanted.h;
		                   });
	}

	bool takeEvent(Uint32 type)
	{
		SDL_Event event{};
		return SDL_PeepEvents(&event, 1, SDL_GETEVENT, type, type) == 1;
	}

	bool burst(Uint32 type)
	{
		SdlUpdateQueue queue;
		if (!expect(queue.push({}, type) && queue.push({ { 0, 0, 0, 10 } }, type) &&
		                !SDL_HasEvent(type),
		            "Empty damage must not schedule a redraw"))
			return false;

		// Old frames cannot build an event backlog or discard damage from
		// another monitor. These two sparse areas should remain separate.
		const SDL_Rect video{ 1920, 0, 3840, 2160 };
		const SDL_Rect cursor{ 10, 10, 20, 20 };
		for (size_t x = 0; x < 10000; x++)
		{
			if (!queue.push({ video, cursor }, type))
				return false;
		}
		if (!expect(takeEvent(type) && !SDL_HasEvent(type), "Burst must post exactly one wakeup"))
			return false;
		const auto rects = queue.pop();
		return expect(rects.size() == 2 && contains(rects, video) && contains(rects, cursor),
		              "Burst lost damage or expanded sparse monitor updates") &&
		       expect(queue.pop().empty(), "Taking a batch must consume its damage");
	}

	bool coverage(Uint32 type)
	{
		SdlUpdateQueue queue;
		std::vector<SDL_Rect> original;
		// Thousands of disjoint tiles force the bounded-list fallback.
		for (int y = 0; y < 100; y++)
		{
			for (int x = 0; x < 100; x++)
				original.push_back({ x * 12, y * 12, 5, 5 });
		}
		if (!queue.push(original, type) || !takeEvent(type))
			return false;
		const auto rects = queue.pop();
		if (!expect(rects.size() <= SdlUpdateQueue::maxRects, "Upload count is unbounded"))
			return false;
		for (const auto& rect : original)
		{
			if (!expect(contains(rects, rect), "Coalescing lost an invalidated tile"))
				return false;
		}

		// A new rectangle bridges two earlier ones. All three become one.
		if (!queue.push({ { 0, 0, 10, 10 }, { 20, 0, 10, 10 }, { 5, 0, 20, 10 } }, type) ||
		    !takeEvent(type))
			return false;
		const auto bridged = queue.pop();
		return expect(bridged.size() == 1 && contains(bridged, { 0, 0, 30, 10 }),
		              "Transitive overlap was not merged");
	}

	bool inputFairness(Uint32 type)
	{
		SdlUpdateQueue queue;
		if (!queue.push({ { 0, 0, 1920, 1080 } }, type))
			return false;
		SDL_Event key{};
		key.type = SDL_EVENT_KEY_DOWN;
		if (!SDL_PushEvent(&key))
			return false;

		SDL_Event event{};
		if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) != 1 ||
		    event.type != type || queue.pop().empty())
			return false;
		// Simulate a producer that continues throughout the draw. The UI
		// consumes one batch per event, rather than draining until empty.
		for (size_t x = 0; x < 10000; x++)
		{
			if (!queue.push({ { 0, 0, 1920, 1080 } }, type))
				return false;
		}
		if (!expect(SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1 &&
		                event.type == SDL_EVENT_KEY_DOWN,
		            "Continuous redraws overtook queued keyboard input"))
			return false;
		return expect(takeEvent(type) && !queue.pop().empty() && !SDL_HasEvent(type),
		              "Damage arriving during drawing lost its next wakeup");
	}

	bool SDLCALL rejectUpdate(void* data, SDL_Event* event)
	{
		return event->type != *static_cast<Uint32*>(data);
	}

	bool wakeupFailureAndReset(Uint32 type)
	{
		SdlUpdateQueue queue;
		SDL_SetEventFilter(rejectUpdate, &type);
		const bool failed = !queue.push({ { 0, 0, 10, 10 } }, type);
		SDL_SetEventFilter(nullptr, nullptr);
		if (!expect(failed && !SDL_HasEvent(type), "Filtered wakeup must report failure") ||
		    !queue.push({}, type) || !takeEvent(type) ||
		    !expect(contains(queue.pop(), { 0, 0, 10, 10 }), "Retry lost pending damage"))
			return false;

		if (!queue.push({ { 100, 100, 10, 10 } }, type))
			return false;
		queue.clear();
		// A stale SDL wakeup from the old connection may still be queued.
		if (!queue.push({ { 0, 0, 5, 5 } }, type) || !takeEvent(type))
			return false;
		const auto rects = queue.pop();
		if (!expect(rects.size() == 1 && contains(rects, { 0, 0, 5, 5 }) &&
		                !contains(rects, { 100, 100, 10, 10 }),
		            "Old session damage survived reset"))
			return false;
		if (!expect(!SDL_HasEvent(type), "Reset left duplicate wakeups"))
			return false;

		// A modal dialog can consume a redraw event without painting. A
		// subsequent frame must restore the wakeup and retain both regions.
		if (!queue.push({ { 10, 10, 5, 5 } }, type) || !takeEvent(type) ||
		    !queue.push({ { 30, 30, 5, 5 } }, type) || !takeEvent(type))
			return false;
		const auto afterDialog = queue.pop();
		return expect(contains(afterDialog, { 10, 10, 5, 5 }) &&
		                  contains(afterDialog, { 30, 30, 5, 5 }),
		              "A consumed wakeup stranded pending damage");
	}

	bool concurrentProducer(Uint32 type)
	{
		SdlUpdateQueue queue;
		std::atomic<bool> done = false;
		std::atomic<bool> ok = true;
		constexpr int count = 4096;
		std::array<bool, count> painted{};
		std::thread producer(
		    [&]()
		    {
			    for (int x = 0; x < count; x++)
			    {
				    if (!queue.push({ { x % 64, x / 64, 1, 1 } }, type))
					    ok = false;
			    }
			    done = true;
		    });
		while (!done || SDL_HasEvent(type))
		{
			if (!takeEvent(type))
			{
				std::this_thread::yield();
				continue;
			}
			const auto rects = queue.pop();
			if (rects.size() > SdlUpdateQueue::maxRects)
				ok = false;
			for (int x = 0; x < count; x++)
				painted[x] = painted[x] || contains(rects, { x % 64, x / 64, 1, 1 });
		}
		producer.join();
		return expect(ok && queue.pop().empty() &&
		                  std::all_of(painted.begin(), painted.end(), [](bool val) { return val; }),
		              "Concurrent producer lost damage or a wakeup");
	}
} // namespace

int main()
{
	if (!SDL_Init(SDL_INIT_EVENTS))
		return 1;
	const auto type = SDL_RegisterEvents(1);
	const bool ok = type != 0 && burst(type) && coverage(type) && inputFairness(type) &&
	                wakeupFailureAndReset(type) && concurrentProducer(type);
	SDL_Quit();
	return ok ? 0 : 1;
}
