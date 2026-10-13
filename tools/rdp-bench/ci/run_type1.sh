#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${DUT_LIBRARY:?Set candidate shared library}"
: "${PEER_LIBRARY:?Set fixed reference shared library}"
: "${DUT_COMMIT:?Set tested source commit}"
: "${OUTPUT_ROOT:?Set a fresh artifact root}"
python3 -B -m unittest discover -s tests -v
mkdir -p "$OUTPUT_ROOT"
python3 -B bench.py replay --library "$DUT_LIBRARY" --wrap --output "$OUTPUT_ROOT/replay.json"
extra=()
if [[ -n "${NETNS_STATE:-}" ]]; then extra+=(--netns "$NETNS_STATE"); fi
if [[ "${DIRECT_SOCKETS:-0}" == 1 ]]; then extra+=(--direct); fi
if [[ -n "${CALIBRATION_RECEIPT:-}" ]]; then extra+=(--calibration "$CALIBRATION_RECEIPT"); fi
status=0
python3 -B bench.py type1 --library "$DUT_LIBRARY" --peer-library "$PEER_LIBRARY" --commit "$DUT_COMMIT" \
  --suite "${SUITE_CONFIG:-configs/type1.json}" --output "$OUTPUT_ROOT/trials" ${extra[@]+"${extra[@]}"} || status=$?
# Publish diagnostics even when the campaign detected failures.
python3 -B bench.py report --input "$OUTPUT_ROOT/trials" --output "$OUTPUT_ROOT/dashboard"
gate=()
if [[ "${REQUIRE_CALIBRATION:-0}" == 1 ]]; then gate+=(--require-calibration); fi
python3 -B ci/check_results.py "$OUTPUT_ROOT/trials" ${gate[@]+"${gate[@]}"} || status=1
exit "$status"
