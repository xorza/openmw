#!/usr/bin/env bash
# Builds optimised and opens the harness on the ship at Seyda Neen, with no validation layers. Its
# own build directory, so it does not fight debug.sh over one set of objects. Extra arguments are
# passed to the tool: `release.sh --view=balmora`.
#
# `release.sh build` only builds and `release.sh bench` measures, which is what an optimised build
# with no layers in it is for. `profile.sh` is where the measurement turns into an explanation.
#
# **Every verb drives a real game**, so there is one binary and one bench: `bench` stands at each
# place of a suite and measures the frames a player would have seen.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-release"

# **Light debug data and frame pointers, in the build the numbers are quoted from.** `-g1` is line
# tables and nothing else — no locals, no types — so it costs nothing at runtime and a profile can
# name a line rather than an offset. `-fno-omit-frame-pointer` measured at 0.2% here, against a
# run-to-run spread of 2.7%, and it is what lets perf walk a stack for the price of reading it.
#
# Both are on the measured build rather than on a profiling build beside it, because two binaries
# means the profile explains a frame the benchmark did not time. Where the distribution builds its
# own libraries with the same two flags, the chain runs out through libstdc++, OSG and SDL as well.
profiling="-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"

if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$profiling" \
        -DCMAKE_CXX_FLAGS="$profiling" \
        -DOPENMW_RTX=ON \
        -DOPENMW_DLSS_SDK="${OPENMW_DLSS_SDK:?point OPENMW_DLSS_SDK at an unpacked DLSS SDK}" \
        -DBUILD_COMPONENTS_TESTS=OFF -DBUILD_OPENMW_TESTS=OFF \
        -DBUILD_OPENCS=OFF -DBUILD_WIZARD=OFF -DBUILD_ESSIMPORTER=OFF \
        -DBUILD_MWINIIMPORTER=OFF -DBUILD_OPENCS_TESTS=OFF \
        -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
fi

# `release.sh build` stops at the binary. That is what `profile.sh` calls: the flags above and the
# configure line under them are this script's, and perf has to read the binary they produced rather
# than one built beside it.
if [ "${1-}" = build ]; then
    exec cmake --build "$build" -j32 --target openmw-rtxtool
fi

cmake --build "$build" -j32 --target openmw-rtxtool

# **The verb stays first.** `dispatch` reads it off argv[1] and takes a leading dash to mean nobody
# named one, so appending it after the switches below silently ran `view` instead — `release.sh
# bench` opened a window and profiled nothing.
verb=()
if [ $# -gt 0 ] && [[ "${1}" != -* ]]; then
    verb=("$1")
    shift
fi

cd "$build"
exec ./openmw-rtxtool "${verb[@]}" --validation=false "$@"
