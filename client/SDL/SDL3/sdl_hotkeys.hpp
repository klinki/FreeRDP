/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client local hotkey bindings supplied by the macOS launcher bridge
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

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <SDL3/SDL_keycode.h>

/* Local actions the client handles instead of forwarding the key to the server. */
enum class SdlHotkeyAction : size_t
{
	Fullscreen,
	Resizable,
	Grab,
	Disconnect,
	Minimize,
	Count
};

/* Wire names used by `start.localHotkeys`. */
inline constexpr std::array<std::string_view, static_cast<size_t>(SdlHotkeyAction::Count)>
    sdlHotkeyActionNames = { "fullscreen", "resizable", "grab", "disconnect", "minimize" };

/* Which side of a modifier pair a binding requires. `None` means the modifier must not be
 * held, so a binding never fires while extra modifiers are down (for example Shift+Control+
 * Command+F does not trigger Control+Command+F). */
enum class SdlHotkeySide : unsigned char
{
	None,
	Either,
	Left,
	Right
};

struct SdlHotkey
{
	SDL_Keycode key = SDLK_UNKNOWN; // SDLK_UNKNOWN: the action is disabled.
	SdlHotkeySide shift = SdlHotkeySide::None;
	SdlHotkeySide ctrl = SdlHotkeySide::None;
	SdlHotkeySide alt = SdlHotkeySide::None;
	SdlHotkeySide gui = SdlHotkeySide::None;

	[[nodiscard]] bool enabled() const
	{
		return key != SDLK_UNKNOWN;
	}
};

using SdlHotkeys = std::array<SdlHotkey, static_cast<size_t>(SdlHotkeyAction::Count)>;

namespace sdl_hotkeys_detail
{
	[[nodiscard]] inline std::string lower(std::string_view value)
	{
		std::string result(value);
		for (auto& c : result)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		return result;
	}

	/* Keys are matched by their layout-mapped keycode, as macOS shortcuts are, so a binding
	 * for F follows the F character on non-QWERTY layouts. */
	[[nodiscard]] inline SDL_Keycode key(const std::string& name)
	{
		if (name.size() == 1)
		{
			const char c = name.front();
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				return static_cast<SDL_Keycode>(c);
			return SDLK_UNKNOWN;
		}
		if (name == "return")
			return SDLK_RETURN;
		if (name == "space")
			return SDLK_SPACE;
		if (name == "tab")
			return SDLK_TAB;
		if (name == "escape")
			return SDLK_ESCAPE;
		if (name.size() <= 3 && name.front() == 'f')
		{
			const auto digits = name.substr(1);
			if (digits.empty() || digits.front() == '0')
				return SDLK_UNKNOWN;
			int number = 0;
			for (const char c : digits)
			{
				if (c < '0' || c > '9')
					return SDLK_UNKNOWN;
				number = number * 10 + (c - '0');
			}
			if (number >= 1 && number <= 12)
				return static_cast<SDL_Keycode>(SDLK_F1 + number - 1);
		}
		return SDLK_UNKNOWN;
	}

	[[nodiscard]] inline bool modifier(const std::string& name, SdlHotkey& hotkey)
	{
		struct Entry
		{
			std::string_view name;
			SdlHotkeySide SdlHotkey::* group;
			SdlHotkeySide side;
		};
		static constexpr Entry entries[] = {
			{ "shift", &SdlHotkey::shift, SdlHotkeySide::Either },
			{ "lshift", &SdlHotkey::shift, SdlHotkeySide::Left },
			{ "rshift", &SdlHotkey::shift, SdlHotkeySide::Right },
			{ "ctrl", &SdlHotkey::ctrl, SdlHotkeySide::Either },
			{ "lctrl", &SdlHotkey::ctrl, SdlHotkeySide::Left },
			{ "rctrl", &SdlHotkey::ctrl, SdlHotkeySide::Right },
			{ "alt", &SdlHotkey::alt, SdlHotkeySide::Either },
			{ "lalt", &SdlHotkey::alt, SdlHotkeySide::Left },
			{ "ralt", &SdlHotkey::alt, SdlHotkeySide::Right },
			{ "gui", &SdlHotkey::gui, SdlHotkeySide::Either },
			{ "lgui", &SdlHotkey::gui, SdlHotkeySide::Left },
			{ "rgui", &SdlHotkey::gui, SdlHotkeySide::Right },
		};
		for (const auto& entry : entries)
		{
			if (entry.name != name)
				continue;
			auto& side = hotkey.*entry.group;
			if (side != SdlHotkeySide::None)
				return false; // The same modifier pair named twice.
			side = entry.side;
			return true;
		}
		return false;
	}

	[[nodiscard]] inline bool side(SdlHotkeySide expected, SDL_Keymod mods, SDL_Keymod left,
	                               SDL_Keymod right)
	{
		const bool l = (mods & left) != 0;
		const bool r = (mods & right) != 0;
		switch (expected)
		{
			case SdlHotkeySide::None:
				return !l && !r;
			case SdlHotkeySide::Either:
				return l || r;
			case SdlHotkeySide::Left:
				return l && !r;
			case SdlHotkeySide::Right:
				return r && !l;
		}
		return false;
	}
} // namespace sdl_hotkeys_detail

/* Parses "off" or "<modifier>+...+<key>", case-insensitively, for example "ctrl+gui+f" or
 * "rshift+return". At least one modifier is required so that a binding never captures
 * ordinary typing. */
[[nodiscard]] inline std::optional<SdlHotkey> sdl_parse_hotkey(std::string_view text)
{
	const auto value = sdl_hotkeys_detail::lower(text);
	if (value == "off")
		return SdlHotkey{};
	SdlHotkey hotkey;
	size_t start = 0;
	size_t modifiers = 0;
	while (true)
	{
		const auto plus = value.find('+', start);
		const auto token = value.substr(start, plus == std::string::npos ? plus : plus - start);
		if (token.empty())
			return std::nullopt;
		if (plus == std::string::npos)
		{
			hotkey.key = sdl_hotkeys_detail::key(token);
			if (!hotkey.enabled() || modifiers == 0)
				return std::nullopt;
			return hotkey;
		}
		if (!sdl_hotkeys_detail::modifier(token, hotkey))
			return std::nullopt;
		++modifiers;
		start = plus + 1;
	}
}

/* True when a key-down event with these modifiers triggers the binding. Lock modifiers
 * (Caps Lock, Num Lock, Scroll Lock) and AltGr level modifiers are ignored. */
[[nodiscard]] inline bool sdl_hotkey_matches(const SdlHotkey& hotkey, SDL_Keycode key,
                                             SDL_Keymod mods)
{
	using sdl_hotkeys_detail::side;
	return hotkey.enabled() && key == hotkey.key &&
	       side(hotkey.shift, mods, SDL_KMOD_LSHIFT, SDL_KMOD_RSHIFT) &&
	       side(hotkey.ctrl, mods, SDL_KMOD_LCTRL, SDL_KMOD_RCTRL) &&
	       side(hotkey.alt, mods, SDL_KMOD_LALT, SDL_KMOD_RALT) &&
	       side(hotkey.gui, mods, SDL_KMOD_LGUI, SDL_KMOD_RGUI);
}

[[nodiscard]] inline std::optional<SdlHotkeyAction>
sdl_hotkey_action(const SdlHotkeys& hotkeys, SDL_Keycode key, SDL_Keymod mods)
{
	for (size_t i = 0; i < hotkeys.size(); ++i)
		if (sdl_hotkey_matches(hotkeys[i], key, mods))
			return static_cast<SdlHotkeyAction>(i);
	return std::nullopt;
}
