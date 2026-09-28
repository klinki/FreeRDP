#!/bin/sh
# Repeat identical captured graphics against the existing baseline/current builds.
set -eu
TASK_ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
cd "$TASK_ROOT"
BASELINE_SOURCE=${REPLAY_BASELINE_SOURCE:-/Users/david/.codex/worktrees/render-counters-baseline/FreeRDP}
BASELINE_BUILD=${REPLAY_BASELINE_BUILD:-$BASELINE_SOURCE/build/counters}
CURRENT_BUILD=${REPLAY_CURRENT_BUILD:-$TASK_ROOT/build/video-responsive}
REPLAY_INPUT=${REPLAY_INPUT:-$TASK_ROOT/diagnostics/graphics-replay-20260920/baseline.gfx}
REPLAY_OUTPUT=${REPLAY_OUTPUT:-$TASK_ROOT/diagnostics/graphics-replay-$(date +%Y%m%d-%H%M%S)-$$}
umask 077
mkdir -p "$TASK_ROOT/build/graphics-replay"
python3 tools/graphics-replay/build.py --source "$TASK_ROOT" --build "$CURRENT_BUILD" \
  --output "$TASK_ROOT/build/graphics-replay/optimized"
for REPLAY_ARG in "$@"; do
  if [ "$REPLAY_ARG" = "--visible" ]; then
    exec "$TASK_ROOT/build/graphics-replay/optimized" --input "$REPLAY_INPUT" \
      --renderer metal --paced "$@"
  fi
done
python3 tools/graphics-replay/build.py --source "$BASELINE_SOURCE" --build "$BASELINE_BUILD" \
  --output "$TASK_ROOT/build/graphics-replay/baseline"
exec python3 tools/graphics-replay/compare.py --input "$REPLAY_INPUT" \
  --baseline "$TASK_ROOT/build/graphics-replay/baseline" \
  --optimized "$TASK_ROOT/build/graphics-replay/optimized" --output "$REPLAY_OUTPUT" "$@"
