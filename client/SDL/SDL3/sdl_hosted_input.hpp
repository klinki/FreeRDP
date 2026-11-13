/** Hosted rendering input: turns the launcher's input messages into the SDL events the client
 * already handles, so that hotkeys, key mapping and mouse handling keep one implementation
 * (launcher-bridge.md, "Hosted rendering"). No SDL window takes part: a hosted window is hidden. */
#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <SDL3/SDL.h>

namespace SdlHostedInput
{
/** macOS virtual key code (NSEvent.keyCode) to SDL scancode. */
inline SDL_Scancode scancode(int keyCode)
{
	static const SDL_Scancode table[] = {
		SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_H,
		SDL_SCANCODE_G, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
		SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_B, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E,
		SDL_SCANCODE_R, SDL_SCANCODE_Y, SDL_SCANCODE_T, SDL_SCANCODE_1, SDL_SCANCODE_2,
		SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_6, SDL_SCANCODE_5, SDL_SCANCODE_EQUALS,
		SDL_SCANCODE_9, SDL_SCANCODE_7, SDL_SCANCODE_MINUS, SDL_SCANCODE_8, SDL_SCANCODE_0,
		SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_O, SDL_SCANCODE_U, SDL_SCANCODE_LEFTBRACKET,
		SDL_SCANCODE_I, SDL_SCANCODE_P, SDL_SCANCODE_RETURN, SDL_SCANCODE_L, SDL_SCANCODE_J,
		SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_K, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_BACKSLASH,
		SDL_SCANCODE_COMMA, SDL_SCANCODE_SLASH, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_PERIOD,
		SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_BACKSPACE,
		SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RGUI, SDL_SCANCODE_LGUI,
		SDL_SCANCODE_LSHIFT, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_LALT, SDL_SCANCODE_LCTRL,
		SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RCTRL, SDL_SCANCODE_UNKNOWN,
		SDL_SCANCODE_F17, SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_MULTIPLY,
		SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_NUMLOCKCLEAR,
		SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_DIVIDE,
		SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_F18,
		SDL_SCANCODE_F19, SDL_SCANCODE_KP_EQUALS, SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1,
		SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
		SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_8,
		SDL_SCANCODE_KP_9, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
		SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F3, SDL_SCANCODE_F8,
		SDL_SCANCODE_F9, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F11, SDL_SCANCODE_UNKNOWN,
		SDL_SCANCODE_F13, SDL_SCANCODE_F16, SDL_SCANCODE_F14, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F10,
		SDL_SCANCODE_APPLICATION, SDL_SCANCODE_F12, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F15,
		SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE,
		SDL_SCANCODE_F4, SDL_SCANCODE_END, SDL_SCANCODE_F2, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_F1,
		SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP
	};
	if (keyCode < 0 || keyCode >= static_cast<int>(sizeof(table) / sizeof(table[0])))
		return SDL_SCANCODE_UNKNOWN;
	return table[keyCode];
}

/** NSEvent modifier flags, including the device-dependent left and right bits, to SDL modifiers.
 * Without a device-dependent bit a modifier counts as the left one. */
inline SDL_Keymod modifiers(uint64_t flags)
{
	Uint32 mods = SDL_KMOD_NONE;
	auto sided = [&](uint64_t any, uint64_t left, uint64_t right, Uint32 leftMod, Uint32 rightMod) {
		if ((flags & any) == 0)
			return;
		if (flags & right)
			mods |= rightMod;
		if ((flags & left) || (flags & right) == 0)
			mods |= leftMod;
	};
	if (flags & (1ULL << 16))
		mods |= SDL_KMOD_CAPS;
	sided(1ULL << 17, 0x2, 0x4, SDL_KMOD_LSHIFT, SDL_KMOD_RSHIFT);
	sided(1ULL << 18, 0x1, 0x2000, SDL_KMOD_LCTRL, SDL_KMOD_RCTRL);
	sided(1ULL << 19, 0x20, 0x40, SDL_KMOD_LALT, SDL_KMOD_RALT);
	sided(1ULL << 20, 0x8, 0x10, SDL_KMOD_LGUI, SDL_KMOD_RGUI);
	return static_cast<SDL_Keymod>(mods);
}

/** One `input` message, already validated as JSON. Positions are in SDL window coordinates:
 * the caller converts from the message's window pixels. */
struct Message
{
	std::string action;
	SDL_WindowID window = 0;
	int keyCode = -1;
	bool down = false;
	bool repeat = false;
	uint64_t flags = 0;
	float x = 0, y = 0, dx = 0, dy = 0;
	int button = 0;
	int clicks = 1;
};

/** Builds events and remembers what is held down, so that it can all be released when the
 * launcher's window loses focus or the launcher goes away. One thread only. */
class State
{
  public:
	/** The SDL event for a message; nothing for a message that must be ignored. */
	std::optional<SDL_Event> event(const Message& message)
	{
		SDL_Event ev = {};
		const auto now = SDL_GetTicksNS();
		if (message.action == "key")
		{
			const auto code = scancode(message.keyCode);
			if (code == SDL_SCANCODE_UNKNOWN)
				return std::nullopt;
			_mods = modifiers(message.flags);
			if (message.down)
				_keys.insert(code);
			else
				_keys.erase(code);
			_keyWindow = message.window;
			return key(code, message.down, message.repeat, message.window, now);
		}
		if (message.action == "mouse_move")
		{
			ev.motion.type = SDL_EVENT_MOUSE_MOTION;
			ev.motion.timestamp = now;
			ev.motion.windowID = message.window;
			ev.motion.state = _buttons;
			ev.motion.x = message.x;
			ev.motion.y = message.y;
			ev.motion.xrel = message.dx;
			ev.motion.yrel = message.dy;
			_x = message.x;
			_y = message.y;
			return ev;
		}
		if (message.action == "mouse_button")
		{
			if (message.button < 1 || message.button > 5)
				return std::nullopt;
			if (message.down)
				_buttons |= SDL_BUTTON_MASK(message.button);
			else
				_buttons &= ~SDL_BUTTON_MASK(message.button);
			_buttonWindow = message.window;
			_x = message.x;
			_y = message.y;
			return button(message.button, message.down, message.clicks, message.window, now);
		}
		if (message.action == "wheel")
		{
			ev.wheel.type = SDL_EVENT_MOUSE_WHEEL;
			ev.wheel.timestamp = now;
			ev.wheel.windowID = message.window;
			ev.wheel.x = message.dx;
			ev.wheel.y = message.dy;
			ev.wheel.integer_x = static_cast<Sint32>(message.dx);
			ev.wheel.integer_y = static_cast<Sint32>(message.dy);
			ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
			ev.wheel.mouse_x = message.x;
			ev.wheel.mouse_y = message.y;
			return ev;
		}
		return std::nullopt;
	}

	/** Key-up and button-up events for everything still held, in a stable order. */
	std::vector<SDL_Event> releaseAll()
	{
		std::vector<SDL_Event> events;
		const auto now = SDL_GetTicksNS();
		_mods = SDL_KMOD_NONE;
		for (const auto code : _keys)
			events.push_back(key(code, false, false, _keyWindow, now));
		for (int index = 1; index <= 5; index++)
		{
			if (_buttons & SDL_BUTTON_MASK(index))
				events.push_back(button(index, false, 1, _buttonWindow, now));
		}
		_keys.clear();
		_buttons = 0;
		return events;
	}

	/** Modifiers as of the last key message. */
	SDL_Keymod mods() const { return _mods; }
	SDL_MouseButtonFlags buttons() const { return _buttons; }
	bool anythingHeld() const { return !_keys.empty() || _buttons != 0; }

  private:
	SDL_Event key(SDL_Scancode code, bool down, bool repeat, SDL_WindowID window, Uint64 now) const
	{
		SDL_Event ev = {};
		ev.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		ev.key.timestamp = now;
		ev.key.windowID = window;
		ev.key.scancode = code;
		ev.key.key = SDL_GetKeyFromScancode(code, _mods, false);
		ev.key.mod = _mods;
		ev.key.down = down;
		ev.key.repeat = repeat;
		return ev;
	}
	SDL_Event button(int index, bool down, int clicks, SDL_WindowID window, Uint64 now) const
	{
		SDL_Event ev = {};
		ev.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		ev.button.timestamp = now;
		ev.button.windowID = window;
		ev.button.button = static_cast<Uint8>(index);
		ev.button.down = down;
		ev.button.clicks = static_cast<Uint8>(clicks < 1 ? 1 : (clicks > 255 ? 255 : clicks));
		ev.button.x = _x;
		ev.button.y = _y;
		return ev;
	}

	std::set<SDL_Scancode> _keys;
	SDL_MouseButtonFlags _buttons = 0;
	SDL_Keymod _mods = SDL_KMOD_NONE;
	SDL_WindowID _keyWindow = 0;
	SDL_WindowID _buttonWindow = 0;
	float _x = 0;
	float _y = 0;
};
} // namespace SdlHostedInput
