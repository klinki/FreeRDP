#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-${repo_dir}/build/videotoolbox}"
build_type="${BUILD_TYPE:-Release}"

if [[ -n "${JOBS:-}" ]]; then
	jobs="${JOBS}"
elif command -v sysctl >/dev/null 2>&1; then
	jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '4')"
elif command -v nproc >/dev/null 2>&1; then
	jobs="$(nproc)"
else
	jobs=4
fi

cmake -S "${repo_dir}" -B "${build_dir}" \
	-DCMAKE_BUILD_TYPE="${build_type}" \
	-DWITH_CLIENT=ON \
	-DWITH_CLIENT_SDL=ON \
	-DWITH_CLIENT_SDL2=OFF \
	-DWITH_CLIENT_SDL3=ON \
	-DWITH_CLIENT_SDL_VERSIONED=OFF \
	-DWITH_FFMPEG=ON \
	-DWITH_VIDEO_FFMPEG=ON \
	-DWITH_VIDEOTOOLBOX=ON \
	-DWITH_OPUS=OFF

cmake --build "${build_dir}" --target sdl3-freerdp --parallel "${jobs}"

printf 'Built %s\n' "${build_dir}/client/SDL/SDL3/sdl-freerdp"
