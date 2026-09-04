#!/usr/bin/env bash
# Builds and runs the optimized harness; `build` stops after building and `game` runs OpenMW.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-release"

# Line tables and frame pointers let perf resolve source locations and walk stacks.
# Use the same binary for benchmarks and profiles so they describe the same frame.
profiling="-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"

if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$profiling" \
        -DCMAKE_CXX_FLAGS="$profiling" \
        -DOPENMW_RTX=ON \
        -DOPENMW_RTX_BENCH=ON \
        -DOPENMW_DLSS_SDK="${OPENMW_DLSS_SDK:?point OPENMW_DLSS_SDK at an unpacked DLSS SDK}" \
        -DBUILD_COMPONENTS_TESTS=OFF -DBUILD_OPENMW_TESTS=OFF \
        -DBUILD_OPENCS=OFF -DBUILD_WIZARD=OFF -DBUILD_ESSIMPORTER=OFF \
        -DBUILD_MWINIIMPORTER=OFF -DBUILD_OPENCS_TESTS=OFF \
        -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=ON \
        -DCMAKE_C_COMPILER_LAUNCHER="ccache;cache_dir=$root/build-cache/ccache" \
        -DCMAKE_CXX_COMPILER_LAUNCHER="ccache;cache_dir=$root/build-cache/ccache" \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
fi

if [ "${1-}" = game ]; then
    shift
    cmake --build "$build" -j32 --target openmw
    cd "$build"
    exec ./openmw --skip-menu --load-savegame "$HOME/.local/share/openmw/saves/asd/Quicksave.omwsave" "$@"
fi

# profile.sh uses this entry point to build the binary it measures.
if [ "${1-}" = build ]; then
    exec cmake --build "$build" -j32 --target openmw-rtxtool
fi

cmake --build "$build" -j32 --target openmw-rtxtool

# dispatch reads the verb from argv[1]; putting switches first silently selects view.
verb=()
if [ $# -gt 0 ] && [[ "${1}" != -* ]]; then
    verb=("$1")
    shift
fi

cd "$build"
exec ./openmw-rtxtool "${verb[@]}" --validation=false "$@"
