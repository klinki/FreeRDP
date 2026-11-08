/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client launcher hotkey binding tests
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

#include "../sdl_hotkeys.hpp"

#include <iostream>

static int failures = 0;

static void check(bool condition, const char* name)
{
	if (!condition)
	{
		std::cerr << name << " failed\n";
		++failures;
	}
}

static SdlHotkey parse(const char* text)
{
	const auto result = sdl_parse_hotkey(text);
	check(result.has_value(), text);
	return result.value_or(SdlHotkey{});
}

int main()
{
	const auto fullscreen = parse("ctrl+gui+f");
	check(sdl_hotkey_matches(fullscreen, SDLK_F, SDL_KMOD_LCTRL | SDL_KMOD_LGUI),
	      "Control-Command-F with left modifiers");
	check(sdl_hotkey_matches(fullscreen, SDLK_F, SDL_KMOD_RCTRL | SDL_KMOD_RGUI),
	      "Control-Command-F with right modifiers");
	check(sdl_hotkey_matches(fullscreen, SDLK_F, SDL_KMOD_LCTRL | SDL_KMOD_LGUI | SDL_KMOD_CAPS),
	      "Caps Lock is ignored");
	check(!sdl_hotkey_matches(fullscreen, SDLK_F, SDL_KMOD_LGUI), "Command-F is forwarded");
	check(!sdl_hotkey_matches(fullscreen, SDLK_F,
	                          SDL_KMOD_LCTRL | SDL_KMOD_LGUI | SDL_KMOD_LSHIFT),
	      "an extra modifier is forwarded");
	check(!sdl_hotkey_matches(fullscreen, SDLK_G, SDL_KMOD_LCTRL | SDL_KMOD_LGUI),
	      "another key is forwarded");

	const auto legacy = parse("RShift+Return");
	check(sdl_hotkey_matches(legacy, SDLK_RETURN, SDL_KMOD_RSHIFT), "Right Shift+Return");
	check(!sdl_hotkey_matches(legacy, SDLK_RETURN, SDL_KMOD_LSHIFT),
	      "Left Shift+Return is forwarded");
	check(!sdl_hotkey_matches(legacy, SDLK_RETURN, SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT),
	      "both Shift keys are forwarded");

	const auto off = parse("off");
	check(!off.enabled(), "off disables the action");
	check(!sdl_hotkey_matches(off, SDLK_UNKNOWN, SDL_KMOD_NONE), "off never matches");
	check(!sdl_hotkey_matches(off, SDLK_RETURN, SDL_KMOD_RSHIFT), "off forwards Shift+Return");

	check(parse("alt+f12").key == SDLK_F12, "function keys");
	check(parse("ctrl+1").key == SDLK_1, "digits");
	check(parse("shift+space").key == SDLK_SPACE, "space");

	for (const auto* invalid :
	     { "", "f", "return", "ctrl+", "+f", "ctrl++f", "ctrl+ctrl+f", "ctrl+lctrl+f",
	       "hyper+f", "ctrl+f13", "ctrl+f0", "ctrl+f01", "ctrl+enter", "ctrl+gui", "ctrl+ff" })
		check(!sdl_parse_hotkey(invalid).has_value(), invalid);

	SdlHotkeys hotkeys{};
	hotkeys[static_cast<size_t>(SdlHotkeyAction::Fullscreen)] = fullscreen;
	hotkeys[static_cast<size_t>(SdlHotkeyAction::Disconnect)] = off;
	check(sdl_hotkey_action(hotkeys, SDLK_F, SDL_KMOD_LCTRL | SDL_KMOD_LGUI) ==
	          SdlHotkeyAction::Fullscreen,
	      "action lookup");
	check(!sdl_hotkey_action(hotkeys, SDLK_D, SDL_KMOD_RSHIFT).has_value(),
	      "Right Shift+D is forwarded when disconnect is off");
	check(!sdl_hotkey_action(hotkeys, SDLK_RETURN, SDL_KMOD_RSHIFT).has_value(),
	      "Right Shift+Return is forwarded when fullscreen is rebound");

	check(sdlHotkeyActionNames.size() == static_cast<size_t>(SdlHotkeyAction::Count),
	      "every action has a wire name");
	return failures == 0 ? 0 : 1;
}
