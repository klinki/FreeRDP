# Connection failures (transport / server-side / black-screen-at-connect)

Split from `bugs/rapid-pace-freeze.md` 2026-09-10 — that file stays render-path
only. Rule of thumb: render freezes recover by themselves and keep logging
input; connection failures end in broken pipes, pre-connect aborts, or a
screen that never presents.

## 2026-09-10 10:16 (VM): pre-connect abort

`validateMonitorScaleOverrides: unknown SDL monitor ID 3`. The multi-screen
launcher defaults (monitors 3,2 + scale overrides) do not fit a
single-monitor target. No session, no screen. Fixed by pointing
`free-rdp-debug-vm.py` at the single-monitor launcher.

## 2026-09-10 12:02 (VM, PID 33605): mid-session transport death

UDP send timeouts from the first datagrams, TCP `Broken pipe` at :43,
`sdl_run` exception on mouse input, SIGTERM then Ctrl+C to kill. Server-side
pattern per the DavidPC cases (cf. 0x80090330 + TCP 1236 local abort there).
Log ends in UDP retry loop.

## Black screen from the start (undiagnosed)

Symptoms: BLACK screen, macOS beachball, app never presents a single frame —
while underneath the session is alive (UDP tunnel up at :33, GDI init,
channels loaded, input events flowing at :34). I.e. main thread wedged before
first present, RDP thread healthy. Suspects: first-draw window sync stall,
topbar font/resource load block, vsync on half-migrated display state.
NOT YET DIAGNOSED — needs a `sample` taken during the black window, before
kill (see `freeze-snapshot.sh`, generated `WHEN_FROZEN.txt` per session).

## 2026-09-11 ~09:20 (davidpc, PID 37903): server-side stall, keepalives-only
(DIAGNOSED as non-client; server cause open)

Session `20260911-090725-37896`, ~15 min in, image froze (remote clock stuck
at 9:20, no reaction to mouse), no beachball, topbar still reactive.
Measured innocent, in order:

- Thread dump (`freeze-092226`, `sample` during the freeze): main thread 90%
  idle in event wait + 8% normal mouse→redraw→Metal-present; RDP thread 98%
  parked in `poll`, 2% reading the UDP tunnel. No deadlock, no spin, process
  CPU idle (3:55 over 15 min). Present path proven working by the live topbar.
- Transport alive both directions (socket ESTABLISHED, ACKs flowing).
- Client input reaches the core and the wire (clicks 09:24:02–03 logged and
  sent) — server ignores them.
- Server→client in the frozen window: 96-byte keepalives every ~1–2s, zero
  graphics payload, zero deactivation, zero errors.

Signature: frozen last frame + keepalives-only + ignored input + healthy
client = the Windows side stopped composing and processing input while
keeping the connection up. Machine confirmed not asleep; physical console
state unknown (no access at the time). Differentials: secure-desktop switch
(UAC/Ctrl+Alt+Del never renders to RDP — expected frozen frame), session
lock/hang, wedged DWM/compositor, display-driver event. Post-hoc witness:
Event Viewer → System + `RdpCoreTS/Operational` around the freeze window.
NOT a client bug — no client change indicated; the new Reconnecting overlay
correctly stays off (transport never died).
