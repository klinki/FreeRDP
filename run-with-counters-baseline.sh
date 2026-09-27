#!/usr/bin/env bash
# Counters-only baseline: same session settings as run-with-counters.sh.
# Arguments are passed to free-rdp-debug.py (for example, --server davidpc).
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo"

for arg in "$@"; do
  if [[ "$arg" == "--help" || "$arg" == "-h" ]]; then
    exec python3 "$repo/free-rdp-debug.py" --help
  fi
done

baseline_commit="f6d34cd3fd034e0803c9089f5dbcb9ffdcc4eaa8"
baseline_worktree="${FREERDP_BASELINE_WORKTREE:-/Users/david/.codex/worktrees/render-counters-baseline/FreeRDP}"
actual_commit="$(git -C "$baseline_worktree" rev-parse HEAD)"
if [[ "$actual_commit" != "$baseline_commit" ]]; then
  printf 'Baseline worktree must be at %s, but is at %s.\n' "$baseline_commit" "$actual_commit" >&2
  exit 1
fi

# Select the baseline explicitly, even if FREERDP_BIN was exported previously.
export FREERDP_BIN="$baseline_worktree/build/counters/client/SDL/SDL3/sdl-freerdp"
export PRIMARY_MONITOR_ID="${PRIMARY_MONITOR_ID:-3}"     # M27UP
export SECONDARY_MONITOR_ID="${SECONDARY_MONITOR_ID:-2}" # DELL U2419HC
if [[ ! -x "$FREERDP_BIN" ]]; then
  printf 'Baseline executable not found: %s\n' "$FREERDP_BIN" >&2
  exit 1
fi

umask 077
render_run="$repo/diagnostics/render-baseline-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$render_run"
printf '*\n' > "$render_run/.gitignore"
export FREERDP_SDL_RENDER_METRICS="$render_run/render.jsonl"
{
  printf 'Variant: counters only; before per-monitor clipping and upload batching\n'
  printf 'Source worktree: %s\nSource commit: %s\n' "$baseline_worktree" "$actual_commit"
  printf 'Binary: %s\nBinary SHA-256: ' "$FREERDP_BIN"
  shasum -a 256 "$FREERDP_BIN"
  printf 'Monitor order: %s,%s\n' "$PRIMARY_MONITOR_ID" "$SECONDARY_MONITOR_ID"
  printf 'The nested session.txt Git rev describes the shared launcher repository.\n'
} > "$render_run/baseline.txt"

printf 'BASELINE: counters enabled; clipping and upload batching absent (%s).\n' "${baseline_commit:0:9}"
printf 'Render counters: %s\n' "$FREERDP_SDL_RENDER_METRICS"
printf 'Summarize during or after the session:\n  python3 %q %q --warmup-intervals 2\n\n' \
  "$repo/tools/render-benchmark/analyze.py" "$FREERDP_SDL_RENDER_METRICS"

# Reuse the current launcher so monitors, AVC444, logging, and capture agree.
exec python3 "$repo/free-rdp-debug.py" --output "$render_run" "$@"
