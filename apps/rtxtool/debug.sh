#!/usr/bin/env bash
# Builds and runs the harness with validation; `game` runs OpenMW on the quicksave.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug"

# Keep assertions enabled: CMake defaults RelWithDebInfo to -DNDEBUG.
# Never use --clean-first: upstream declares files/lang/*.ts as build byproducts.
if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g" \
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DOPENMW_RTX=ON \
        -DOPENMW_DLSS_SDK="${OPENMW_DLSS_SDK:?point OPENMW_DLSS_SDK at an unpacked DLSS SDK}" \
        -DBUILD_COMPONENTS_TESTS=ON -DBUILD_OPENMW_TESTS=ON \
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

cmake --build "$build" -j32 --target openmw-rtxtool

# dispatch reads the verb from argv[1]; putting switches first silently selects view.
verb=()
if [ $# -gt 0 ] && [[ "${1}" != -* ]]; then
    verb=("$1")
    shift
fi

# From the build directory, because --resources defaults to ./resources.
cd "$build"
exec ./openmw-rtxtool "${verb[@]}" --validation --sync-validation "$@"
