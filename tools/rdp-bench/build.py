#!/usr/bin/env python3
"""Configure a separate development build and record native benchmark provenance."""
import argparse
import hashlib
import os
import subprocess
from pathlib import Path
from rdpbench.common import sha256, write_json
from rdpbench.adapter import PR_ADAPTER,native_source_files

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=str(ROOT / "build/rdp-bench"))
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    parser.add_argument("cmake_options", nargs=argparse.REMAINDER, help="Extra CMake flags after --")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    build = Path(args.build).expanduser().resolve()
    extra = args.cmake_options
    if extra and extra[0] == "--":
        extra = extra[1:]
    subprocess.run([
        "cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
        "-DWITH_CLIENT=OFF", "-DWITH_CLIENT_COMMON=ON", "-DWITH_SERVER=OFF",
        "-DWITH_SAMPLE=OFF", *extra, "-DBUILD_SHARED_LIBS=ON", "-DWITH_RDPEUDP=ON", "-DWITH_RDP_BENCH=ON",
    ], check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "freerdp", "--parallel", str(args.jobs)], check=True)
    options = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("//", "#")) and "=" in line:
            key, value = line.split("=", 1)
            if ":INTERNAL" not in key:
                options[key] = value
    libraries = sorted((build / "libfreerdp").glob("libfreerdp[0-9]*.dylib"))
    if not libraries:
        libraries = sorted((build / "libfreerdp").glob("libfreerdp[0-9]*.so*"))
    if not libraries:
        raise RuntimeError("benchmark shared library not found")
    library = libraries[0].resolve()
    commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    diff = subprocess.check_output(["git", "-C", str(ROOT), "diff", "HEAD", "--"])
    write_json(build / "rdp-bench-build.json", {
        "commit": commit, "tracked_changes_sha256": hashlib.sha256(diff).hexdigest(),
        "tracked_dirty": bool(diff), "library": str(library), "binary_sha256": sha256(library),
        "native_adapter_sha256": sha256(ROOT / "libfreerdp/core/rdpudp_bench_private.inc"),
        "native_adapter": PR_ADAPTER,
        "native_source_sha256": {
            str(path.relative_to(ROOT)): sha256(path)
            for path in native_source_files(ROOT / "libfreerdp/core",backend=2).values()
        },
        "build_options": options,
    })
    print("Benchmark library:", library)


if __name__ == "__main__":
    main()
