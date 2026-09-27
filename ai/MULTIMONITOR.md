# Task: Add explicit per-monitor DPI scaling to FreeRDP SDL3 client

We are working on a fork of **FreeRDP 3.31.1**.

Implement the first part of improved multi-monitor support: **explicit per-monitor RDP scale overrides in the SDL3 client**.

Keep this task narrowly scoped. Do not attempt to fix automatic macOS DPI detection yet.

## Background

FreeRDP already represents scaling per monitor.

`rdpMonitor` contains monitor attributes including:

```cpp
desktopScaleFactor
deviceScaleFactor
```

The SDL3 Display Control path already takes these values from each `rdpMonitor` and places them into the corresponding:

```cpp
DISPLAY_CONTROL_MONITOR_LAYOUT
```

However, the existing command-line options:

```text
/scale-desktop
/scale-device
```

are global overrides. When used, they replace the scale values for every monitor.

That means a configuration such as:

```text
4K display:   175% desktop scale
1080p display: 100% desktop scale
```

cannot currently be represented explicitly from the command line.

There is also unreliable automatic scale detection on macOS, but **that is a separate task and must not be fixed in this change**.

Observed SDL3 output from macOS currently includes cases such as:

```text
monitor.orig_screen                   2
monitor.width                         3840
monitor.height                        2160
monitor.attributes.desktopScaleFactor 200
monitor.attributes.deviceScaleFactor  100

monitor.orig_screen                   3
monitor.width                         1920
monitor.height                        1080
monitor.attributes.desktopScaleFactor 100
monitor.attributes.deviceScaleFactor  100
```

and during monitor/window initialization those scale values can temporarily become associated with the wrong physical monitor.

For this first task we need a deterministic manual override independent of SDL's automatic scale detection.

---

# Goal

Add an SDL3-specific command-line option allowing the user to assign exact RDP scaling values to individual SDL monitor IDs.

Proposed syntax:

```text
/sdl-monitor-scale:<monitor-id>=<desktop-scale>/<device-scale>[,...]
```

Example:

```text
/sdl-monitor-scale:2=175/180,3=100/100
```

Meaning:

```text
SDL monitor 2:
    DesktopScaleFactor = 175
    DeviceScaleFactor  = 180

SDL monitor 3:
    DesktopScaleFactor = 100
    DeviceScaleFactor  = 100
```

The monitor IDs are the same SDL display IDs shown by:

```text
/list:monitor
```

and used by:

```text
/monitors:<ids>
```

For example:

```text
/monitors:2,3
/sdl-monitor-scale:2=175/180,3=100/100
```

Do not identify monitors by their final RDP array index because the monitor array may be sorted before transmission. Key overrides by the SDL display ID / `rdpMonitor.orig_screen`.

---

# Required behavior

## 1. Add the command-line option

Add an SDL3 client-specific argument:

```text
/sdl-monitor-scale:<spec>
```

Suggested help text:

```text
Override RDP scaling for individual SDL monitors.
Format: <id>=<desktop>/<device>[,<id>=<desktop>/<device>...]
Monitor IDs are shown by /list:monitor.
```

Do not add a new public `rdpSettings` property for this first implementation unless the existing SDL client architecture makes doing so clearly simpler.

Prefer storing the parsed overrides in `SdlContext`.

A suitable internal representation could be conceptually:

```cpp
struct MonitorScaleOverride
{
    SDL_DisplayID displayId;
    UINT32 desktopScaleFactor;
    UINT32 deviceScaleFactor;
};
```

Use the most idiomatic existing FreeRDP/SDL3 C++ representation rather than mechanically copying this struct if a map/vector type fits better.

For example:

```cpp
std::unordered_map<SDL_DisplayID, MonitorScaleOverride>
```

or a small `std::vector`.

There will normally be only a handful of monitors, so performance is irrelevant.

---

# 2. Parse and validate the value strictly

Accept:

```text
2=175/180
```

and:

```text
1=200/180,2=175/180,3=100/100
```

Validation:

### Monitor ID

Must be a valid positive integer representable as `SDL_DisplayID`.

Reject malformed/non-numeric values.

### DesktopScaleFactor

Valid range:

```text
100..500
```

inclusive.

Examples:

```text
100
125
150
175
200
250
```

are valid.

### DeviceScaleFactor

Only these RDP values are valid:

```text
100
140
180
```

Do not automatically derive this value in this task.

The user must specify it explicitly.

### Duplicates

Reject:

```text
2=175/180,2=200/180
```

with a useful error.

### Malformed input

Reject things such as:

```text
2
2=
2=175
2=175/
2=/180
foo=175/180
2=99/100
2=175/175
```

Do not silently coerce values.

Produce an actionable error message.

---

# 3. Do not allow global and per-monitor scale overrides simultaneously

The following combination must be rejected:

```text
/scale-desktop:175
/sdl-monitor-scale:2=175/180,3=100/100
```

Likewise for:

```text
/scale-device
```

and any legacy/global option which causes:

```cpp
FREERDP_MONITOR_OVERRIDE_DESKTOP_SCALE
```

or:

```cpp
FREERDP_MONITOR_OVERRIDE_DEVICE_SCALE
```

to be set.

Check the existing:

```cpp
FreeRDP_MonitorOverrideFlags
```

rather than relying only on presence of particular command-line strings, if practical.

Reason:

`sdl_disp.cpp` currently applies the global override after reading the individual monitor attributes. Allowing both would therefore silently destroy the per-monitor values.

Fail early with a message such as:

```text
/sdl-monitor-scale cannot be combined with global desktop/device scale overrides
```

Do not invent precedence rules in this task.

---

# 4. Apply overrides to `rdpMonitor`, not just to Display Control PDUs

This is important.

Do NOT implement this merely by changing:

```text
client/SDL/SDL3/sdl_disp.cpp
```

and overriding values while constructing `DISPLAY_CONTROL_MONITOR_LAYOUT`.

Instead, apply the configured values to:

```cpp
rdpMonitor.attributes.desktopScaleFactor
rdpMonitor.attributes.deviceScaleFactor
```

for the matching monitor.

The overridden `rdpMonitor` values should exist before the monitor array is placed into:

```cpp
FreeRDP_MonitorDefArray
```

during `preConnect`.

This should make the values available to all RDP layers that consume the monitor definitions, including initial monitor negotiation as well as subsequent Display Control monitor-layout PDUs.

Trace the connection-time monitor serialization code to verify this assumption rather than assuming it.

The design requirement is:

> initial monitor negotiation and runtime Display Control must see the same overridden values.

---

# 5. Primary application point

Inspect:

```text
client/SDL/SDL3/sdl_monitor.cpp
```

In particular:

```cpp
sdl_detect_monitors(...)
sdl_apply_display_properties(...)
```

Current logic builds a vector of `rdpMonitor` objects roughly through:

```cpp
const auto monitor = sdl->getDisplay(id);
monitors.emplace_back(monitor);
```

and eventually calls:

```cpp
freerdp_settings_set_monitor_def_array_sorted(...)
```

Add a reusable helper that applies a configured override to an `rdpMonitor` based on:

```cpp
monitor.orig_screen
```

Conceptually:

```cpp
bool SdlContext::applyMonitorScaleOverride(rdpMonitor& monitor) const;
```

or equivalent.

The exact class/function placement should follow the existing code style.

Conceptual behavior:

```cpp
auto monitor = sdl->getDisplay(id);

applyMonitorScaleOverride(monitor);

monitors.emplace_back(monitor);
```

Do this **before**:

```cpp
freerdp_settings_set_monitor_def_array_sorted(...)
```

Do not key overrides by the array position after sorting.

---

# 6. Preserve overrides during dynamic monitor/layout updates

This is just as important as applying them during initial connection.

SDL3 rebuilds monitor information when display/window state changes.

Inspect:

```text
client/SDL/SDL3/sdl_context.cpp
client/SDL/SDL3/sdl_disp.cpp
```

especially:

```cpp
SdlContext::updateWindow(...)
SdlContext::updateWindowList(...)
sdlDispContext::sendLayout(...)
```

Current SDL/window querying may regenerate `rdpMonitor` values and therefore restore automatically detected values such as:

```text
desktopScaleFactor = 200
deviceScaleFactor = 100
```

The explicit override must survive this.

Whenever an updated `rdpMonitor` is about to replace/update the monitor definition used by RDP, reapply the same override helper.

Do not duplicate parsing or scale-selection logic in several locations.

There should be one source of truth for:

```text
display ID -> explicit desktop/device scale
```

and one reusable function for applying it.

Acceptance condition:

After connecting with:

```text
/sdl-monitor-scale:2=175/180,3=100/100
```

a display/window resize or Display Control update must not revert monitor 2 to 200/100.

---

# 7. Leave unspecified monitors untouched

The option does not need to describe every monitor.

For example:

```text
/sdl-monitor-scale:2=175/180
```

must alter only monitor 2.

Other monitors retain whatever values FreeRDP/SDL automatically detected.

This makes incremental testing possible.

---

# 8. Monitor validation

Once SDL display enumeration is available, verify that every configured monitor ID refers to an actual SDL display.

For an invalid ID such as:

```text
/sdl-monitor-scale:99=175/180
```

fail clearly rather than silently ignoring it.

Example:

```text
Monitor scale override references unknown SDL monitor ID 99
Use /list:monitor to list available monitor IDs.
```

If architecture makes it substantially cleaner to validate during `sdl_detect_monitors()` instead of command-line parsing, do that.

Parsing should validate syntax and numeric ranges.

Display enumeration should validate existence.

An override may reference a connected monitor which is not selected by `/monitors`; that does not need to be an error. It can simply have no effect during that session.

---

# 9. Logging

Add useful DEBUG logging whenever an override is applied.

Example:

```text
monitor 2 scale override:
    desktopScaleFactor 200 -> 175
    deviceScaleFactor 100 -> 180
```

Prefer a compact single-line version consistent with FreeRDP logging style.

Example:

```text
Applying scale override for monitor 2: desktop 200->175, device 100->180
```

This is important for manual protocol testing.

Do not emit it at ERROR/WARN level for a valid configuration.

---

# 10. Do not change automatic scale detection

Out of scope for this task:

```cpp
SDL_GetWindowDisplayScale(...)
SDL_GetDisplayContentScale(...)
```

Do not try to determine whether macOS 2.0 should translate to RDP 175%, 200%, etc.

Do not change:

```cpp
monitor.attributes.deviceScaleFactor = 100;
```

globally as part of automatic detection.

Manual override must take precedence when configured, but default behavior without the new option must remain exactly as it is today.

Automatic macOS scale detection will be a separate follow-up task.

---

# 11. Do not fix physical monitor dimensions

We have separately identified suspicious handling of:

```cpp
physicalWidth
physicalHeight
```

in the SDL3 code.

That is explicitly out of scope.

Do not alter physical-size calculation in this patch.

---

# 12. Do not work on VideoToolbox

Also explicitly out of scope:

```text
WITH_VIDEOTOOLBOX
H.264 decoding
AVC444
graphics performance
```

This task is only per-monitor scaling.

---

# 13. Do not change RDP protocol code unless necessary

The existing FreeRDP data model and Display Control code already support different scale factors for each `rdpMonitor`.

Therefore this should primarily be an SDL3 client/configuration change.

Avoid modifying:

```text
libfreerdp/core/
channels/disp/
```

unless tracing reveals an actual blocker.

If you believe a core change is necessary, explain why before making a broad architectural change.

---

# Relevant source areas to inspect first

Start with:

```text
include/freerdp/settings_types.h

client/SDL/SDL3/sdl_context.hpp
client/SDL/SDL3/sdl_context.cpp

client/SDL/SDL3/sdl_monitor.hpp
client/SDL/SDL3/sdl_monitor.cpp

client/SDL/SDL3/sdl_window.hpp
client/SDL/SDL3/sdl_window.cpp

client/SDL/SDL3/sdl_disp.hpp
client/SDL/SDL3/sdl_disp.cpp

client/common/cmdline.c
client/common/cmdline.h
```

Also trace where:

```cpp
FreeRDP_MonitorDefArray
```

and:

```cpp
MONITOR_ATTRIBUTES
```

are serialized into the initial RDP monitor capability/data blocks.

Do this specifically to confirm that modifying the monitor attributes before `preConnect` returns affects the initial monitor data as expected.

---

# Current SDL3 implementation facts

Current SDL3 monitor querying does approximately:

```cpp
const float factor = SDL_GetWindowDisplayScale(window);
const float dpi = std::roundf(factor * 100.0f);

monitor.attributes.desktopScaleFactor =
    static_cast<UINT32>(dpi);

monitor.attributes.deviceScaleFactor = 100;
```

Do not replace this in this task.

The manual override should be layered on top of it.

Current Display Control generation does approximately:

```cpp
layout.DesktopScaleFactor =
    monitor->attributes.desktopScaleFactor;

layout.DeviceScaleFactor =
    monitor->attributes.deviceScaleFactor;
```

and then optionally replaces them using global monitor override settings.

Therefore no global scale override must be active when `/sdl-monitor-scale` is used.

---

# Tests

Add automated tests where practical.

At minimum, isolate the parser sufficiently that these cases can be tested without requiring actual monitors.

## Valid parser cases

```text
2=175/180
```

Expected:

```text
id=2
desktop=175
device=180
```

Multiple:

```text
1=200/180,2=175/180,3=100/100
```

Boundary desktop values:

```text
1=100/100
1=500/180
```

Device values:

```text
100
140
180
```

## Invalid parser cases

Test:

```text
""
2
2=
2=175
2=175/
2=/180
foo=175/180
2=foo/180
2=175/foo
2=99/100
2=501/180
2=175/175
2=175/0
2=175/180,2=100/100
```

All should fail cleanly.

## Application logic

If convenient to unit test, verify:

Given:

```cpp
rdpMonitor monitor;
monitor.orig_screen = 2;
monitor.attributes.desktopScaleFactor = 200;
monitor.attributes.deviceScaleFactor = 100;
```

and override:

```text
2=175/180
```

result must be:

```text
desktopScaleFactor = 175
deviceScaleFactor  = 180
```

A monitor with another ID must remain unchanged.

---

# Regression requirement

With no:

```text
/sdl-monitor-scale
```

specified, behavior must remain unchanged.

This is mandatory.

---

# Manual test scenario

Our current macOS machine has monitor IDs visible in SDL logs such as:

```text
1 -> MacBook display, 2940x1846
2 -> external 4K, 3840x2160
3 -> external 1080p, 1920x1080
```

Do not hard-code these IDs anywhere; they are only a test fixture.

Primary test is external 4K + 1080p:

```text
monitor 2:
    DesktopScaleFactor = 175
    DeviceScaleFactor  = 180

monitor 3:
    DesktopScaleFactor = 100
    DeviceScaleFactor  = 100
```

Example command:

```bash
sdl-freerdp \
  /v:davidpc \
  /port:3389 \
  /u:david \
  /d:davidpc \
  /multimon \
  /monitors:2,3 \
  /sdl-monitor-scale:2=175/180,3=100/100 \
  /gfx:AVC444:on \
  /network:lan \
  /cert:tofu \
  /clipboard \
  /from-stdin:force \
  /log-level:DEBUG \
  +f
```

There must be NO:

```text
/scale-desktop
/scale-device
```

in this command.

Expected FreeRDP debug information should show effectively:

```text
monitor 2:
3840x2160
desktopScaleFactor = 175
deviceScaleFactor  = 180

monitor 3:
1920x1080
desktopScaleFactor = 100
deviceScaleFactor  = 100
```

The override must continue to be present if Display Control later sends another monitor layout.

---

# End-to-end Windows acceptance test

On the Windows RDP server we know the desired behavior from a Microsoft MSTSC reference session.

The expected remote Windows monitor state is:

```text
4K monitor:
3840x2160
Effective DPI = 168
Windows scale = 175%

1080p monitor:
1920x1080
Effective DPI = 96
Windows scale = 100%
```

This is the primary functional acceptance criterion.

If the FreeRDP debug log says the overrides were applied but Windows does not expose those DPI values, investigate whether the initial monitor data and/or Display Control path is replacing them.

Do not work around that by bitmap scaling the SDL output.

The goal is for Windows itself to create the proper per-monitor DPI environment.

---

# Expected architecture

Prefer something conceptually like:

```text
command line
    |
    v
parse SDL monitor scale overrides
    |
    v
SdlContext stores:
    display ID -> desktop/device scale
    |
    +------------------------+
    |                        |
    v                        v
initial monitor detect       runtime monitor refresh
    |                        |
    v                        v
rdpMonitor attributes        rdpMonitor attributes
    |                        |
    +-----------+------------+
                |
                v
       FreeRDP_MonitorDefArray
                |
        +-------+-------+
        |               |
        v               v
 initial RDP data    Display Control
```

Do not implement two independent override paths for connection-time and runtime.

The `rdpMonitor` structure should remain the authoritative representation.

---

# Code quality

Follow existing FreeRDP naming, formatting and logging conventions.

Avoid introducing unnecessary dependencies.

Keep parsing separate from applying the overrides.

Keep the patch small enough that it could reasonably become an upstream PR later.

Do not perform unrelated formatting/refactoring.

Run the repository's formatter/linter if required by its contribution guidelines.

Build the SDL3 client and run relevant tests.

---

# Deliverables

When finished:

1. Summarize the implementation.
2. List every changed file and why it changed.
3. Show the exact new CLI syntax.
4. Show validation/error behavior.
5. Explain exactly where overrides are applied during initial connection.
6. Explain exactly how they remain applied during dynamic Display Control updates.
7. State whether initial `CS_MONITOR_EX` uses the overridden `rdpMonitor` attributes, citing the code path you traced.
8. Provide the commands used to build and test.
9. Provide the exact command for our `2=175/180,3=100/100` manual test.
10. Do not start the automatic macOS DPI-detection task.

Before editing, inspect the repository's contribution instructions and any `AGENTS.md` files that apply.

Make the implementation, tests, and build verification rather than only proposing a design.
