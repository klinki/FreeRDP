/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Display Control Channel
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
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

#include <vector>
#include <atomic>
#include <mutex>

#include <freerdp/types.h>
#include <freerdp/event.h>
#include <freerdp/client/disp.h>

#include "sdl_types.hpp"

#include <SDL3/SDL.h>

class sdlDispContext
{

  public:
	explicit sdlDispContext(SdlContext* sdl);
	sdlDispContext(const sdlDispContext& other) = delete;
	sdlDispContext(sdlDispContext&& other) = delete;
	virtual ~sdlDispContext();

	sdlDispContext& operator=(const sdlDispContext& other) = delete;
	sdlDispContext& operator=(sdlDispContext&& other) = delete;

	[[nodiscard]] bool init(DispClientContext* disp);
	[[nodiscard]] bool uninit(DispClientContext* disp);

	[[nodiscard]] bool handleEvent(const SDL_DisplayEvent& ev);
	[[nodiscard]] bool handleEvent(const SDL_WindowEvent& ev);
	void service();           // Display probing and resize sends run only on the SDL thread.
	void service(UINT64 now); // Monotonic time injection for regression coverage.

  private:
	[[nodiscard]] UINT DisplayControlCaps(UINT32 maxNumMonitors, UINT32 maxMonitorAreaFactorA,
	                                      UINT32 maxMonitorAreaFactorB);
	[[nodiscard]] bool setWindowResizeable();

	[[nodiscard]] bool sendResize(UINT64 now);
	[[nodiscard]] bool settings_changed(const std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT>& layout);
	[[nodiscard]] bool sendLayout(const rdpMonitor* monitors, size_t nmonitors);

	[[nodiscard]] bool scheduleResize();

	[[nodiscard]] bool updateMonitor(SDL_WindowID id);
	[[nodiscard]] bool updateMonitors(SDL_EventType type, SDL_DisplayID displayID);

	[[nodiscard]] static UINT DisplayControlCaps(DispClientContext* disp, UINT32 maxNumMonitors,
	                                             UINT32 maxMonitorAreaFactorA,
	                                             UINT32 maxMonitorAreaFactorB);
	static void OnActivated(void* context, const ActivatedEventArgs* e);
	static void OnGraphicsReset(void* context, const GraphicsResetEventArgs* e);

	SdlContext* _sdl = nullptr;
	DispClientContext* _disp = nullptr;
	UINT64 _lastSentDate = 0;
	std::mutex _channelMutex;
	std::atomic<bool> _activated{ false };
	std::atomic<bool> _resizeRequested{ false }, _topologyRequested{ false },
	    _forceRequested{ false };
	std::atomic<int> _resizeableUpdate{ -1 };
	bool _pending = false, _refreshTopology = false, _forceLayout = false;
	UINT64 _nextAttempt = 0;
	unsigned _retries = 0;
	std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> _last_sent_layout;
};
