# Autonomous drag experiments — 2026-09-16

Outcome: the captured source-window coordinate transform defect was reproduced, fixed, and verified in both directions at the final input API. The normal fixed client is now running on the VM. The preliminary built-in-display experiment below was inconclusive; the Dell retry supplied the decisive measurement.

## Fix validation

The production change resolves the event's logical position into the destination client window before using its renderer scale and normalized desktop origin. Motion, button presses/releases, and focus-generated moves use `screenToRdp`. SDL capture and event window identity remain intact; relative deltas keep their existing source-window conversion. No current global mouse poll is used for mapping queued events. A missing destination retains the source mapping.

The same VM, monitor order, 175%/100% overrides, and global y=-811 were used. Temporary instrumentation recorded both final motion and button API arguments. UDP establishment and receive migration were logged. Results:

| Run | Held motion events | Stationary hold | Input at destination | Button-up input | Maximum mapping residual |
| --- | --- | --- | --- | --- | --- |
| Dell → M27UP → Dell | 1192 | 12 s | (3184,538) | (632,1349), back on Dell | 0.80 px |
| Dell → M27UP, release | 477 | 6 s | (3184,538) | (3184,538) | 0 px |
| M27UP → Dell → M27UP | 954 | 12 s | (1100,1349) | (3184,538), back on M27UP | 0.97 px |
| M27UP → Dell, release | 475 | 6 s | (1100,1349) | (1100,1349) | 0.97 px |

All 3,098 held motions match the expected destination mapping within integer truncation. The source window ID remains 8 for Dell-origin gestures and 7 for M27UP-origin gestures. Event age maxima are 19.949, 18.505, 13.202, and 10.158 ms respectively. Source raw event coordinates, not the later global-pointer sample, are used to calculate mapping residuals; this avoids falsely treating a newer physical pointer sample as the queued event's position.

Computer Use inspected the destination after each release. In the 1366×768 previews, the M27UP release point is approximately (449.6,191.3), on a title bar spanning x≈164–699. The Dell release point is approximately (782.6,191.3), on a title bar spanning x≈456–1067. The horizontal grab offsets (~286 and ~327 preview pixels) agree with the expected 1.75/2 local-display size ratio. These are post-release image observations, not simultaneous held-button images. A separate helper check confirmed the final mouse button was up.

Validation commands:

```text
cmake --build build/videotoolbox --target sdl3-freerdp -j 8
TestSDLInputMapping (standalone C++17 build, -Wall -Wextra -Werror)
TestSDLMonitorScale (standalone C++17 build, -Wall -Wextra -Werror)
python3 tools/udp-review-tests/logs/drag-20260916/fixed/verify.py
git diff --check
```

All passed. `TestSDLInputMapping` is also registered in the existing CMake testing block; the current local build has BUILD_TESTING disabled, so both focused tests were compiled/run directly. The SDL client build emits existing SSPI deprecation and duplicate-library warnings.

Fixed-run evidence: `../tools/udp-review-tests/logs/drag-20260916/fixed/`, including logs, probe source, build logs, binary hashes, `verify.py`, and `analysis.json`. The normal build executable was copied into the temporary app wrapper and reconnected without DYLD probes or the final-send logging object after validation.

## Dell retry — measured input mismatch

The Dell was reconnected and both external displays were confirmed before the retry:

| Display | SDL ID | macOS global logical bounds | Backing pixels | Requested Windows scale |
| --- | --- | --- | --- | --- |
| Built-in, excluded from RDP | 1 | (0,0,1470,956) | 2940×1912 | — |
| Dell U2419HC | 2 | (0,-1080,1920,1080) | 1920×1080 | 100% |
| M27UP | 3 | (1920,-1080,1920,1080) | 3840×2160 | 175% |

Client arguments included `/multimon +f /monitors:3,2 /sdl-monitor-scale:3=175/100,2=100/100 +multitransport`. Scale overrides were logged against the correct monitor names. TLS over RDP-UDP was established. The initial “staying on TCP” message was followed by “UDP recv migrated on first DVC PDU”; this was not the earlier explicit-TCP fallback experiment. This does not assert that mouse input itself used UDP.

The original binary plus the SDL conversion probe first reproduced source-window capture. A second temporary executable then added a `DRAG_FINAL` log immediately before the motion call to `freerdp_client_send_button_event` in `SdlTouch::handleEvent`, after the focus check, pixel conversion, and monitor offset. Only a temporary copy of `sdl_touch.cpp` was compiled; its object replaced the original object in a separate link using the existing build objects. Production source and the original build executable were unchanged. This log records actual final input API arguments, **not wire packets or server receipt**.

Both drags used the same title-bar height, global y=-811, so there is no hand-induced y drift:

| Run | Global pointer trajectory | Hold | Held motion events | Event age median / p95 / max |
| --- | --- | --- | --- | --- |
| M27UP → Dell → M27UP | (2552,-811) → (1100,-811) → (2552,-811) | 12 s | 948 | 0.669 / 1.474 / 15.814 ms |
| Dell → M27UP → Dell | (632,-811) → (2552,-811) → (632,-811) | 12 s | 1181 | 0.356 / 2.690 / 18.184 ms |

Independent no-button movements measured the normal mapping at each destination. Exact final-send samples:

| Unix timestamp | Physical pointer | State / event window | Final input (x,y) |
| --- | --- | --- | --- |
| 1789541124.288646 | (1100,-811) | ordinary / Dell 8 | (1100,1349) |
| 1789541150.064212 | (1100,-811) | held drag / M27UP 7 | (280,538) |
| 1789541228.308308 | (2552,-811) | ordinary / M27UP 7 | (3184,538) |
| 1789541243.820273 | (2552,-811) | held drag / Dell 8 | (2552,1349) |

For the entire M27UP-origin drag, final x = 2×global_x−1920 and y=538. For the entire Dell-origin drag, final x = global_x and y=1349. Residuals are under one pixel, consistent with integer truncation. All events retain the source window ID, including on the destination. On Dell, the M27UP-origin x error is global_x−1920, so it grows with distance left of the boundary. On M27UP, the Dell-origin error is 1920−global_x, growing with distance right. The y errors have opposite signs because the two displays use different local pixel scales and different negotiated y offsets.

The end-of-return samples again match the source transform. Computer Use verified Notepad returned to its starting position after each round trip. A separate helper check after the last run confirmed `down=false`. Source-window images during the hold showed Notepad partially off that surface, but simultaneous destination images were not captured. Do not treat this as a measured visual gap or as validation of a fix.

**Conclusion:** the client computes different final input positions for the same physical pointer location depending on where the drag began. The source renderer scale plus source monitor offset is the mechanism. This is sufficient to establish an input mapping defect independently of Windows' legitimate DPI resizing. No claim is made that every historical symptom has the same cause.

The next implementation should resolve the destination surface from the event's global logical position, then transform through that surface's pixel geometry and negotiated origin. Resolving the captured window's display alone leaves this defect intact. Button release and focus-generated positions need the same coordinate convention; capture itself should remain reliable.

Pre-fix evidence is in `../tools/udp-review-tests/logs/drag-20260916/dell-retry/`: both client logs, helper timelines, final-send instrumentation source, helper source, probe source, `analysis.json`, and binary hashes. That diagnostic run used a temporary instrumented executable and did not change production source.

## Preliminary built-in-display setup and limits

- VM: 192.168.64.2, existing David account.
- Current binary: build/videotoolbox/client/SDL/SDL3/sdl-freerdp (September 11 build); SHA256 and source revision recorded with artifacts.
- Dell is disconnected. User approved substituting the built-in display for a preliminary test.
- M27UP: 1920×1080 local points, 3840×2160 backing pixels, SDL ID 1; requested Windows scaling 175%.
- Built-in: 1470×956 local points, 2940×1912 backing pixels, SDL ID 2; requested Windows scaling 100%.
- Built-in is left of M27UP: local desktop origin (-1470,0). Its fullscreen client content frame is (-1470,33,1470,923), due to the top safe area. M27UP content frame is (0,0,1920,1080).
- BOTH local displays have 2× backing density. This does not reproduce the original 2×/1× local display pair, despite differing Windows DPI settings.
- Initial UDP-enabled session established a tunnel but logged that it retained TCP transport. Subsequent reconnect attempts ended with a server logoff response and a UDP timeout/reset respectively. The instrumented measurements below explicitly used TCP (-multitransport).

## Method

Computer Use opened and inspected Notepad. User explicitly authorized a temporary native mouse helper because Computer Use cannot pause with the button held. The helper posts CGEvents to the session event tap at approximately 60 movements/second, pauses, and releases or returns. A same-screen 100-point drag visibly moved the window approximately 83 pixels in a screenshot scaled from 1470 to 1224 pixels, as expected.

A temporary DYLD logging probe records SDL_ConvertEventToRenderCoordinates input/output and SDL event age at conversion. Repository code and the existing client binary were not changed. Attempts to interpose the final FreeRDP input functions did not produce records: there are NO measured post-offset / wire coordinates in these artifacts. Conversion logs must not be described as wire evidence.

## Measured results

All pointer coordinates here are macOS global logical points.

| Run | Pointer trajectory | Stationary hold | Held motion events | Median / p95 / max event age |
| --- | --- | --- | --- | --- |
| Initial traced round trip | (-529,301) → (550,301) → (-529,301) | 12 s | 921 | 0.324 / 4.441 / 19.215 ms |
| Forward, ending on M27UP | (-529,301) → (700,301) | 10 s | 462 | 0.308 / 2.670 / 13.065 ms |
| Reverse round trip | (700,270) → (-600,270) → (700,270) | 12 s | 916 | 0.345 / 6.099 / 20.206 ms |

The helper's pause-start and pause-end records show the same pointer position and button down=true. Its immediate post-release records can still show down=true because CGEvent delivery is asynchronous; subsequent runs start with down=false.

Forward drag remained on SDL window 6 (built-in), even with the pointer on M27UP. The first traced round trip followed:

- raw x = global x + 1470
- converted x = 2 × raw x
- raw y = 268 at global y = 301 (33-point content inset)
- converted y = 536 throughout

At the boundary, converted x progressed 2934.02 → 2938.52 → 2943.02 → 2947.51. There was no change of coordinate scale at the boundary. Maximum residual against 2 × (global x + 1470) was 0.01 pixels (logging precision).

Reverse drag remained on SDL window 5 (M27UP), including while the pointer was on the built-in display. Converted y stayed 540 for global y=270, and coordinates became negative relative to the M27UP window as expected during capture.

## Interpretation

This directly establishes source-window capture during these synthetic held-button drags. The pointer's destination display did not become the event window. Event ages do not show a seconds-long input backlog in these runs.

It does NOT establish absence or presence of the reported visual detachment. Computer Use captured the source window while dragging; changing window focus during the hold would confound the experiment. A request to extend the helper to capture both displays was still awaiting user approval when these notes were written.

The 33-point built-in content inset is an additional geometry difference to account for, not a proven bug. Since both local displays have the same backing density, this run also cannot rule out source-window pixel conversion being inappropriate when a captured pointer crosses onto the original Dell with different local backing density.

Next measurement: simultaneous destination/source images during the hold, correlated with actual post-offset input coordinates. Repeat with a 2×/1× local display pair (preferably the original Dell) before generalizing to the reported configuration.

Artifacts: ../tools/udp-review-tests/logs/drag-20260916/ . Temporary executable, probes, full initial logs and helpers: /tmp/rdp-drag-20260916/ . TLS secrets, if present in the temporary directory, were not copied into repository artifacts.
