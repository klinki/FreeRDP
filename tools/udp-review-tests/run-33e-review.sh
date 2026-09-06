#!/usr/bin/env bash
# Compile historical review harnesses against an existing FreeRDP build.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../.." && pwd)"
build="${1:-/tmp/freerdp-build}"
if [[ ! -d "$build" ]]; then
  echo "Build directory does not exist: $build" >&2
  exit 1
fi
build="$(cd -- "$build" && pwd)"
results="${RESULTS_DIR:-$here/results/review-33e410c56}"
mkdir -p "$results"
cc_bin="${CC:-cc}"
common=(-std=gnu11 -include winpr/file.h
  -I"$repo" -I"$repo/winpr/include" -I"$build/winpr/include"
  -I"$build/include" -I"$repo/include"
  -L"$build/libfreerdp" -L"$build/winpr/libwinpr"
  -Wl,-rpath,"$build/libfreerdp" -Wl,-rpath,"$build/winpr/libwinpr")
openssl_include="${OPENSSL_INCLUDE_DIR:-$(sed -n 's/^OPENSSL_INCLUDE_DIR:PATH=//p' "$build/CMakeCache.txt" | head -n 1)}"
if [[ -n "$openssl_include" ]]; then
  common+=(-I"$openssl_include")
fi
failed=0
for src in "$here"/harnesses/udp-review-33e-*.c; do
  name="$(basename "$src" .c)"
  printf '%s\n' "Running $name"
  if "$cc_bin" "${common[@]}" "$src" -lfreerdp3 -lwinpr3 -o "$results/$name" >"$results/$name.build.log" 2>&1; then
    if "$results/$name" >"$results/$name.txt" 2>&1; then
      cat "$results/$name.txt"
    else
      cat "$results/$name.txt"
      failed=1
    fi
  else
    cat "$results/$name.build.log"
    failed=1
  fi
done
exit "$failed"
