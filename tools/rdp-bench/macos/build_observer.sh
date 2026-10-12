#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
task_cache="${RDP_BENCH_SWIFT_MODULE_CACHE:-$(pwd)/.module-cache}"
mkdir -p "$task_cache"
swiftc -O -parse-as-library -module-cache-path "$task_cache" BenchObserver.swift -o bench-observer
printf '%s\n' 'Built observer. Grant permissions, set US input layout, then run ./bench-observer --list.'
