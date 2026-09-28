#!/usr/bin/env bash
# Run the instrumented SDL3 client with per-monitor counters and packet capture.
# Arguments are passed to free-rdp-debug.py (for example, --server davidpc).
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo"

export FREERDP_BIN="${FREERDP_BIN:-$repo/build/video-responsive/client/SDL/SDL3/sdl-freerdp}"
export PRIMARY_MONITOR_ID="${PRIMARY_MONITOR_ID:-3}"     # M27UP
export SECONDARY_MONITOR_ID="${SECONDARY_MONITOR_ID:-2}" # DELL U2419HC

if [[ ! -x "$FREERDP_BIN" ]]; then
  printf 'FreeRDP executable not found: %s\n' "$FREERDP_BIN" >&2
  exit 1
fi

umask 077
render_run="$repo/diagnostics/render-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$render_run"
printf '*\n' > "$render_run/.gitignore"
export FREERDP_SDL_RENDER_METRICS="$render_run/render.jsonl"

printf 'Render counters: %s\n' "$FREERDP_SDL_RENDER_METRICS"
printf 'After the session, summarize them with:\n  python3 %q %q --warmup-intervals 2\n\n' \
  "$repo/tools/render-benchmark/analyze.py" "$FREERDP_SDL_RENDER_METRICS"

exec python3 "$repo/free-rdp-debug.py" --output "$render_run" "$@"
