/**
 * Regression coverage for session fullscreen and connection-bar state:
 * native window events are the source of truth, the bar follows its owner
 * window, pin state is preserved, and mixed multi-monitor sessions toggle
 * consistently. Needs no display.
 *
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#include <cstdio>
#include <tuple>

#include "../sdl_fullscreen_state.hpp"

static int failures = 0;

static void expect(bool condition, const char* scenario, const char* message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL [%s]: %s\n", scenario, message);
		failures++;
	}
}

static constexpr SDL_WindowID primary = 5;
static constexpr SDL_WindowID secondary = 6;

/* A session whose windows have connected and entered fullscreen through the
 * initial request, as after /f or /multimon. */
static SdlFullscreenState connected(bool multimon, uint64_t now)
{
	SdlFullscreenState state;
	state.addWindow(primary);
	if (multimon)
		state.addWindow(secondary);
	state.request(true, now);
	std::ignore = state.observe(primary, true);
	if (multimon)
		std::ignore = state.observe(secondary, true);
	return state;
}

static void freshSessionShowsBar()
{
	const char* s = "fresh session";
	SdlFullscreenState state;
	state.addWindow(primary);
	expect(state.barOwner == primary, s, "first window owns the bar");
	expect(!state.barShown(primary, false), s, "no bar before the window is actually fullscreen");
	state.request(true, 0);
	expect(state.sessionFullscreen(10), s, "request in flight reports its target");
	expect(!state.barShown(primary, false), s, "request alone does not show the bar");
	const auto observed = state.observe(primary, true);
	expect(observed.changed && observed.owner && observed.requested, s,
	       "enter event answers the request on the owner");
	expect(state.barShown(primary, false), s, "bar shown once actually fullscreen");
	expect(state.barPinned, s, "bar pinned by default");
}

/* The reported sequence: Right Shift+Return leaves, the green button
 * re-enters each window. */
static void shortcutLeaveNativeReenter(bool pinned, bool multimon)
{
	char s[96];
	snprintf(s, sizeof(s), "shortcut leave, native re-enter (%s, %s)",
	         pinned ? "pinned" : "unpinned", multimon ? "two monitors" : "one monitor");
	uint64_t now = 1000;
	auto state = connected(multimon, now);
	state.barPinned = pinned;

	now += 100;
	expect(state.toggleTarget(primary, now) == false, s, "toggle from fullscreen leaves");
	state.request(false, now);
	expect(state.barShown(primary, multimon), s, "bar stays until the window actually leaves");
	auto observed = state.observe(primary, false);
	expect(observed.owner && observed.requested, s, "leave answers the request");
	if (multimon)
		std::ignore = state.observe(secondary, false);
	expect(!state.barShown(primary, multimon), s, "bar hidden after leaving");
	expect(!state.sessionFullscreen(now), s, "session windowed");
	expect(state.barPinned == pinned, s, "leaving keeps the pin state");

	/* Native re-entry, long after the request. */
	now += 20000;
	observed = state.observe(primary, true);
	expect(observed.changed && observed.owner && !observed.requested, s,
	       "native enter is recorded as such");
	expect(state.barShown(primary, multimon), s, "bar restored by native re-entry");
	expect(state.barPinned == pinned, s, "native re-entry keeps the pin state");
	if (multimon)
	{
		expect(!state.sessionFullscreen(now), s, "one window back is a mixed session");
		observed = state.observe(secondary, true);
		expect(observed.changed && !observed.owner, s, "secondary does not own the bar");
	}
	expect(state.sessionFullscreen(now), s, "all windows back: session fullscreen");
	expect(state.toggleTarget(primary, now) == false, s,
	       "next toggle leaves, without repeating the shortcut first");
}

static void nativeLeaveSessionToggleReenters()
{
	const char* s = "native leave, toggle re-enters";
	uint64_t now = 1000;
	auto state = connected(false, now);
	now += 10000;
	std::ignore = state.observe(primary, false);
	expect(!state.barShown(primary, false), s, "bar hidden after native leave");
	expect(state.toggleTarget(primary, now) == true, s, "toggle enters after native leave");
	state.request(true, now);
	std::ignore = state.observe(primary, true);
	expect(state.barShown(primary, false), s, "bar shown again");
}

static void unpinnedHoverInWindowedMultimon()
{
	const char* s = "unpinned multimon windowed";
	uint64_t now = 1000;
	auto state = connected(true, now);
	state.barPinned = false;
	std::ignore = state.observe(primary, false);
	std::ignore = state.observe(secondary, false);
	expect(state.barAllowed(true), s, "multimon windowed still allows a revealed bar");
	expect(!state.barShown(primary, true), s, "hidden until revealed");
	state.barVisible = true; /* hover reveal */
	expect(state.barShown(primary, true), s, "hover reveals the bar");
	expect(!state.barAllowed(false), s, "single-monitor windowed never allows the bar");
}

static void mixedMonitors()
{
	const char* s = "mixed monitors";
	uint64_t now = 1000;
	auto state = connected(true, now);
	now += 10000;

	/* Only the secondary leaves (its green button). */
	auto observed = state.observe(secondary, false);
	expect(observed.changed && !observed.owner, s, "secondary leave does not touch the bar");
	expect(state.barShown(primary, true), s, "bar stays on the fullscreen owner");
	expect(!state.sessionFullscreen(now), s, "mixed session is not fullscreen");
	expect(state.anyFullscreen(), s, "one window still fullscreen");
	expect(state.toggleTarget(secondary, now) == true, s,
	       "toggle from the windowed monitor brings the session back");
	expect(state.toggleTarget(primary, now) == false, s,
	       "toggle from the fullscreen monitor (or its bar) leaves");
	expect(state.toggleTarget(0, now) == true, s, "unknown origin: mixed session enters");

	/* Only the owner leaves. */
	std::ignore = state.observe(secondary, true);
	observed = state.observe(primary, false);
	expect(observed.owner, s, "owner leave reported");
	expect(!state.barShown(primary, true), s, "bar hidden with its owner");
	expect(!state.barShown(secondary, true), s, "bar never moves to a fullscreen sibling");
	std::ignore = state.observe(primary, true);
	expect(state.barShown(primary, true), s, "bar back when the owner re-enters");
}

static void toggleInFlightReverses()
{
	const char* s = "toggle in flight";
	uint64_t now = 1000;
	auto state = connected(true, now);
	now += 10000;
	state.request(false, now);
	expect(state.anyPending(now + 10), s, "request pending");
	expect(state.toggleTarget(primary, now + 10) == true, s,
	       "second toggle before the transition completes reverses it");
	std::ignore = state.observe(primary, false);
	expect(!state.sessionFullscreen(now + 20), s, "pending target reported while one window is still leaving");
	expect(state.anyPending(now + 20), s, "secondary still pending");
	std::ignore = state.observe(secondary, false);
	expect(!state.anyPending(now + 30), s, "all answered");
	expect(!state.sessionFullscreen(now + 30), s, "session windowed");

	/* A request that never produces its event stops deciding the toggle. */
	state.request(true, now + 100);
	const auto late = now + 100 + SdlFullscreenState::pendingTimeoutMs;
	expect(!state.anyPending(late), s, "stale request expires");
	expect(state.toggleTarget(primary, late) == true, s,
	       "after expiry the actual (windowed) state decides");
	/* An unrelated event for the stale request still doesn't fake success. */
	auto observed = state.observe(primary, false);
	expect(!observed.changed && !observed.requested, s, "no change, no answer");
}

static void failedTransition()
{
	const char* s = "failed transition";
	uint64_t now = 1000;
	auto state = connected(false, now);
	now += 10000;
	state.request(false, now);
	auto observed = state.requestFailed(primary, true);
	expect(!observed.changed, s, "window stays fullscreen");
	expect(!state.pending(primary, now), s, "failure clears the request");
	expect(state.sessionFullscreen(now), s, "session still reported fullscreen");
	expect(state.barShown(primary, false), s, "bar stays in sync with the actual window");
	expect(state.toggleTarget(primary, now) == false, s, "next toggle tries to leave again");

	/* A failed enter of a windowed window. */
	std::ignore = state.observe(primary, false);
	state.request(true, now);
	observed = state.requestFailed(primary, false);
	expect(!observed.changed && !state.anyPending(now), s, "failed enter leaves it windowed");
	expect(!state.barShown(primary, false), s, "no bar for a failed enter");
}

static void ownerRemoved()
{
	const char* s = "owner removed";
	uint64_t now = 1000;
	auto state = connected(true, now);
	state.barPinned = false;
	state.removeWindow(primary);
	expect(state.barOwner == 0, s, "bar has no owner after its window closes");
	state.ensureOwner(secondary);
	expect(state.barOwner == secondary, s, "next window owns the bar");
	expect(state.barShown(secondary, true), s, "new fullscreen owner shows the bar");
	expect(!state.barPinned, s, "pin state kept");
	state.ensureOwner(42);
	expect(state.barOwner == secondary, s, "existing owner is kept");

	state.resetWindows();
	expect(state.barOwner == 0 && !state.anyFullscreen(), s, "windows forgotten on recreate");
	expect(!state.barPinned, s, "recreating windows keeps the pin");
	state.clear();
	expect(state.barPinned && state.barVisible, s, "session cleanup restores defaults");
}

static void unknownWindowIgnored()
{
	const char* s = "unknown window";
	auto state = connected(false, 1000);
	const auto observed = state.observe(99, false);
	expect(!observed.changed, s, "events for other windows are ignored");
	expect(state.barShown(primary, false), s, "bar unaffected");
}

int TestSDLFullscreenState(int, char*[])
{
	freshSessionShowsBar();
	for (const bool pinned : { true, false })
		for (const bool multimon : { false, true })
			shortcutLeaveNativeReenter(pinned, multimon);
	nativeLeaveSessionToggleReenters();
	unpinnedHoverInWindowedMultimon();
	mixedMonitors();
	toggleInFlightReverses();
	failedTransition();
	ownerRemoved();
	unknownWindowIgnored();
	if (failures == 0)
		printf("TestSDLFullscreenState: all scenarios passed\n");
	return failures == 0 ? 0 : 1;
}

int main(int argc, char* argv[])
{
	return TestSDLFullscreenState(argc, argv);
}
