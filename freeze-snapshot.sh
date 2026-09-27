#!/usr/bin/env bash
# One-shot freeze evidence kit for sdl-freerdp UI freezes.
# Run WHILE the UI is frozen, BEFORE killing anything:
#   freeze-snapshot.sh <pid> <outdir>
# Everything is best-effort; a missing tool skips its artifact, never fails.
set -uo pipefail

pid="${1:?usage: freeze-snapshot.sh <pid> <outdir>}"
out="${2:?usage: freeze-snapshot.sh <pid> <outdir>}"
mkdir -p "$out"
stamp="$(date +%H%M%S)"

# 1. Per-thread stacks: WHERE each thread is stuck (spin vs wait vs render).
sample "$pid" 5 -f "$out/sample-$stamp.txt" >/dev/null 2>"$out/sample-$stamp.err" || true

# 2. CPU delta: climbing fast = spinning; frozen = parked.
{
	ps -o pid,time,etime,command -p "$pid"
	sleep 10
	ps -o pid,time,etime,command -p "$pid"
} >"$out/cpu-$stamp.txt" 2>&1 || true

# 3. System load + memory pressure: throttled/swapping machine vs sick app.
uptime >"$out/load-$stamp.txt" 2>&1 || true
memory_pressure >"$out/memory-$stamp.txt" 2>&1 || true

# 4. CPU/GPU power + thermal pressure (needs sudo; skipped without it).
if sudo -n true 2>/dev/null; then
	sudo powermetrics --samplers cpu_power,gpu_power -n 1 >"$out/power-$stamp.txt" 2>&1 || true
else
	echo "skipped: no passwordless sudo for powermetrics" >"$out/power-$stamp.txt"
fi

# 5. Socket queues: full buffers mean the peer stopped reading.
netstat -anv -p tcp 2>/dev/null | grep 3389 >"$out/sockets-$stamp.txt" || true

# 6. Open files/sockets of the process.
lsof -p "$pid" >"$out/fds-$stamp.txt" 2>&1 || true

echo "freeze evidence saved to $out"
