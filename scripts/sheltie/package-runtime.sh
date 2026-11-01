#!/bin/bash
# Stage, smoke-test and package a built Sheltie runtime as release assets.
# Usage: package-runtime.sh /path/to/FreeRDP /path/to/output [release-tag]
# Requires a full clone with upstream/master fetched (see scripts/sheltie/README.md).
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 /path/to/FreeRDP /path/to/output [release-tag]" >&2
  exit 2
fi
SOURCE="$(cd "$1" && pwd)"
OUTPUT="$2"
TAG="${3:-}"
TOOLS="$SOURCE/scripts/sheltie"
BUILD="${ENGINE_BUILD_DIR:-$SOURCE/build/launcher}"
TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"

if [[ -e "$OUTPUT" ]]; then
  echo "Output already exists: $OUTPUT" >&2
  exit 1
fi
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"
STAGE="$OUTPUT/stage"
DIST="$OUTPUT/dist"
mkdir -p "$STAGE" "$DIST"

python3 "$SOURCE/scripts/sheltie-version.py" --repo "$SOURCE" --json > "$STAGE/version.json"
if [[ -n "$TAG" ]]; then
  python3 "$TOOLS/release_manifest.py" check-release --version-json "$STAGE/version.json" --tag "$TAG"
fi

python3 "$TOOLS/macos_bundle.py" stage --binary "$BUILD/client/SDL/SDL3/sdl-freerdp" \
  --engine-build "$BUILD" --source "$SOURCE" --output "$STAGE/runtime" --target "$TARGET"
python3 "$TOOLS/macos_bundle.py" stage --binary "$BUILD/server/Sample/sfreerdp-server" \
  --engine-build "$BUILD" --source "$SOURCE" --output "$STAGE/validation-runtime" --target "$TARGET"
# Sources are a separate release asset rather than a copy inside runtime.tar.gz.
python3 "$TOOLS/macos_bundle.py" sources --runtime "$STAGE/runtime" --source "$SOURCE" \
  --output "$DIST/runtime-sources.tar.gz"
(cd "$SOURCE" && python3 "$TOOLS/build_provenance.py" --output "$STAGE/runtime/build-provenance.json")
python3 "$TOOLS/release_manifest.py" complete --runtime "$STAGE/runtime" \
  --version-json "$STAGE/version.json" --tag "$TAG" --output "$DIST/runtime-manifest.json"

# Smoke-test the staged, relocated client against the staged sample server.
CLIENT="$STAGE/runtime/Contents/Helpers/sdl-freerdp"
SERVER="$STAGE/validation-runtime/Contents/Helpers/sfreerdp-server"
python3 "$TOOLS/bridge-smoke.py" "$CLIENT"
python3 "$TOOLS/local-session-smoke.py" "$CLIENT" --sample-server "$SERVER" --suite release \
  --diagnostics-dir "$OUTPUT/smoke-release"
python3 "$TOOLS/local-session-smoke.py" "$CLIENT" --sample-server "$SERVER" --case connect_cancel \
  --reverse-mouse-wheel --diagnostics-dir "$OUTPUT/smoke-reverse-wheel"

# Ship the matching test kit beside the sample server for consumers' packaged checks.
KIT="$STAGE/validation-runtime/test-kit"
mkdir -p "$KIT"
cp "$TOOLS/bridge-smoke.py" "$TOOLS/local-session-smoke.py" "$TOOLS/macos_bundle.py" "$TOOLS/README.md" "$KIT/"
cp "$STAGE/version.json" "$KIT/engine-version.json"

tar -czf "$DIST/runtime.tar.gz" -C "$STAGE/runtime" .
tar -czf "$DIST/validation-runtime.tar.gz" -C "$STAGE/validation-runtime" .
(cd "$DIST" && shasum -a 256 runtime.tar.gz validation-runtime.tar.gz runtime-sources.tar.gz \
  runtime-manifest.json > SHA256SUMS)
echo "Packaged $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["engine_version"])' \
  "$DIST/runtime-manifest.json") in $DIST"
