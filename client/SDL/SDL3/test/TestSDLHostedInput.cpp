/** Hosted rendering input: launcher messages to SDL events, without a window or server. */
#include "../sdl_hosted_input.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}

static SdlHostedInput::Message key(int code, bool down, uint64_t flags = 0, bool repeat = false)
{
	SdlHostedInput::Message message;
	message.action = "key";
	message.window = 7;
	message.keyCode = code;
	message.down = down;
	message.flags = flags;
	message.repeat = repeat;
	return message;
}

int main()
{
	try
	{
		using namespace SdlHostedInput;
		require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());

		/* Key codes: letters, the ISO key, keypad, arrows, function keys, modifiers, bounds. */
		require(scancode(0) == SDL_SCANCODE_A && scancode(11) == SDL_SCANCODE_B, "letters");
		require(scancode(10) == SDL_SCANCODE_NONUSBACKSLASH, "ISO section key");
		require(scancode(36) == SDL_SCANCODE_RETURN && scancode(53) == SDL_SCANCODE_ESCAPE, "return, escape");
		require(scancode(76) == SDL_SCANCODE_KP_ENTER && scancode(82) == SDL_SCANCODE_KP_0, "keypad");
		require(scancode(123) == SDL_SCANCODE_LEFT && scancode(126) == SDL_SCANCODE_UP, "arrows");
		require(scancode(122) == SDL_SCANCODE_F1 && scancode(111) == SDL_SCANCODE_F12, "function keys");
		require(scancode(55) == SDL_SCANCODE_LGUI && scancode(54) == SDL_SCANCODE_RGUI, "command keys");
		require(scancode(56) == SDL_SCANCODE_LSHIFT && scancode(60) == SDL_SCANCODE_RSHIFT, "shift keys");
		require(scancode(58) == SDL_SCANCODE_LALT && scancode(61) == SDL_SCANCODE_RALT, "option keys");
		require(scancode(59) == SDL_SCANCODE_LCTRL && scancode(62) == SDL_SCANCODE_RCTRL, "control keys");
		require(scancode(-1) == SDL_SCANCODE_UNKNOWN && scancode(127) == SDL_SCANCODE_UNKNOWN &&
		            scancode(4000) == SDL_SCANCODE_UNKNOWN,
		        "out-of-range key codes");

		/* Modifier flags: sides from the device bits, left when there is none. */
		const uint64_t shift = 1ULL << 17, control = 1ULL << 18, option = 1ULL << 19, command = 1ULL << 20;
		require(modifiers(0) == SDL_KMOD_NONE, "no modifiers");
		require(modifiers(shift | 0x2) == SDL_KMOD_LSHIFT, "left shift");
		require(modifiers(shift | 0x4) == SDL_KMOD_RSHIFT, "right shift");
		require(modifiers(shift | 0x2 | 0x4) == (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT), "both shifts");
		require(modifiers(shift) == SDL_KMOD_LSHIFT, "shift without a side");
		require(modifiers(control | 0x2000) == SDL_KMOD_RCTRL && modifiers(control | 0x1) == SDL_KMOD_LCTRL, "control");
		require(modifiers(option | 0x40) == SDL_KMOD_RALT && modifiers(option | 0x20) == SDL_KMOD_LALT, "option");
		require(modifiers(command | 0x10) == SDL_KMOD_RGUI && modifiers(command | 0x8) == SDL_KMOD_LGUI, "command");
		require(modifiers(1ULL << 16) == SDL_KMOD_CAPS, "caps lock");
		require(modifiers(0x2 | 0x8) == SDL_KMOD_NONE, "device bits alone are not modifiers");

		/* Keys become SDL key events carrying the modifiers of their message. */
		State state;
		auto event = state.event(key(56, true, shift | 0x2));
		require(event && event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_LSHIFT, "shift down");
		event = state.event(key(0, true, shift | 0x2));
		require(event && event->key.scancode == SDL_SCANCODE_A && event->key.down && !event->key.repeat &&
		            event->key.windowID == 7 && (event->key.mod & SDL_KMOD_LSHIFT) != 0,
		        "shifted A");
		require(state.mods() == SDL_KMOD_LSHIFT && state.anythingHeld(), "state after shifted A");
		event = state.event(key(0, true, shift | 0x2, true));
		require(event && event->key.repeat, "key repeat");
		require(!state.event(key(127, true)), "unknown key produced an event");

		/* Mouse: buttons are remembered and reported with motion. */
		Message mouse;
		mouse.window = 7;
		mouse.action = "mouse_button";
		mouse.button = 1;
		mouse.down = true;
		mouse.clicks = 2;
		mouse.x = 10;
		mouse.y = 20;
		event = state.event(mouse);
		require(event && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == 1 &&
		            event->button.clicks == 2 && event->button.x == 10 && event->button.y == 20,
		        "button down");
		mouse.action = "mouse_move";
		mouse.x = 30;
		mouse.y = 40;
		mouse.dx = 20;
		mouse.dy = 20;
		event = state.event(mouse);
		require(event && event->type == SDL_EVENT_MOUSE_MOTION && event->motion.x == 30 &&
		            event->motion.xrel == 20 && (event->motion.state & SDL_BUTTON_LMASK) != 0,
		        "drag motion");
		mouse.action = "mouse_button";
		mouse.button = 9;
		require(!state.event(mouse), "button 9 accepted");
		mouse.action = "wheel";
		mouse.dx = 0;
		mouse.dy = -1.5f;
		event = state.event(mouse);
		require(event && event->type == SDL_EVENT_MOUSE_WHEEL && event->wheel.y == -1.5f &&
		            event->wheel.integer_y == -1 && event->wheel.mouse_x == 30,
		        "fractional wheel");
		mouse.action = "pinch";
		require(!state.event(mouse), "unknown action accepted");

		/* Losing focus releases exactly what is held: shift, A and the left button. */
		auto released = state.releaseAll();
		int keysUp = 0, buttonsUp = 0;
		for (const auto& up : released)
		{
			if (up.type == SDL_EVENT_KEY_UP && !up.key.down && up.key.mod == SDL_KMOD_NONE)
				keysUp++;
			if (up.type == SDL_EVENT_MOUSE_BUTTON_UP && up.button.button == 1 && up.button.x == 30)
				buttonsUp++;
		}
		require(released.size() == 3 && keysUp == 2 && buttonsUp == 1, "release did not match what was held");
		require(!state.anythingHeld() && state.mods() == SDL_KMOD_NONE && state.buttons() == 0, "state after release");
		require(state.releaseAll().empty(), "second release produced events");

		/* A key that went up normally is not released again. */
		std::ignore = state.event(key(1, true));
		std::ignore = state.event(key(1, false));
		require(state.releaseAll().empty(), "released a key that was already up");

		SDL_Quit();
		printf("Hosted input: ok\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		fprintf(stderr, "TestSDLHostedInput failed: %s\n", error.what());
		return 1;
	}
}
