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
