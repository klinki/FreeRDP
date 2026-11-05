#!/bin/bash
# Build the Sheltie runtime configuration of the SDL3 client.
# BUILD_ENGINE_TESTS=ON also builds and runs every SDL client test and the YUV codec tests.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/FreeRDP" >&2
  exit 2
fi
ENGINE_SOURCE="$(cd "$1" && pwd)"
ENGINE_BUILD="${ENGINE_BUILD_DIR:-$ENGINE_SOURCE/build/launcher}"
BREW_PREFIX="$(brew --prefix)"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
SAMPLE_SERVER="${BUILD_SAMPLE_SERVER:-OFF}"
ENGINE_TESTS="${BUILD_ENGINE_TESTS:-OFF}"
JOBS="${BUILD_JOBS:-8}"

# The macOS 27 SDK can detect pipe2 even when the running macOS lacks it.
# Use WinPR's portable pipe implementation for older deployment targets.
# BUILD_TESTING only adds test targets; unlike BUILD_TESTING_INTERNAL it does
# not export internal symbols, so the shipped client is unchanged.
cmake -S "$ENGINE_SOURCE" -B "$ENGINE_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="$BREW_PREFIX" \
  -DOPENSSL_ROOT_DIR="$BREW_PREFIX/opt/openssl@3" \
  -DWITH_CLIENT_SDL=ON -DWITH_CLIENT_SDL3=ON -DWITH_CLIENT_SDL2=OFF \
  -DWITH_LAUNCHER_BRIDGE=ON \
  -DWITH_CLIENT_MAC=OFF -DWITH_X11=OFF -DWITH_SDL_LINK_SHARED=ON \
  -DWITH_VIDEOTOOLBOX=ON -DWITH_FFMPEG=ON -DWITH_VIDEO_FFMPEG=ON \
  -DWITH_SDL_IMAGE_DIALOGS=OFF -DWITH_WEBVIEW=OFF -DWITH_RDTK=OFF \
  -DWITH_KRB5=OFF -DWITH_FUSE=OFF -DWITH_OPENH264=OFF \
  -DWITH_RDPEUDP=ON \
  -DWITH_SERVER="$SAMPLE_SERVER" -DWITH_PROXY=OFF -DWITH_SHADOW=OFF -DWITH_SAMPLE="$SAMPLE_SERVER" \
  -DWITH_CCACHE=OFF -DBUILD_TESTING="$ENGINE_TESTS" -DBUILD_TESTING_INTERNAL=OFF \
  -DSDL_LAUNCHER_TEST="$ENGINE_TESTS" \
  -DWINPR_HAVE_PIPE2=OFF
cmake --build "$ENGINE_BUILD" --target sdl3-freerdp --parallel "$JOBS"
if [[ "$SAMPLE_SERVER" == ON ]]; then
  cmake --build "$ENGINE_BUILD" --target sfreerdp-server --parallel "$JOBS"
fi
if [[ "$ENGINE_TESTS" == ON ]]; then
  tests=()
  while IFS= read -r target; do
    tests+=("$target")
  done < <(grep -oE 'add_executable\(TestSDL[A-Za-z0-9]+' "$ENGINE_SOURCE/client/SDL/SDL3/CMakeLists.txt" |
    sed 's/add_executable(//' | sort -u)
  cmake --build "$ENGINE_BUILD" --target "${tests[@]}" TestFreeRDPCodec --parallel "$JOBS"
  ctest --test-dir "$ENGINE_BUILD/client/SDL/SDL3" --output-on-failure
  ctest --test-dir "$ENGINE_BUILD" -R '^TestFreeRDPCodecYuv' --output-on-failure
fi
echo "Built engine: $ENGINE_BUILD/client/SDL/SDL3/sdl-freerdp"
