#!/usr/bin/env bash
# Builds with debug info and opens the harness on the ship at Seyda Neen, under the validation
# layers. Extra arguments are passed to the tool: `debug.sh --view=balmora`.
#
# `debug.sh game` builds and runs OpenMW itself on the quicksave instead. Profiling belongs in
# `release.sh` and `profile.sh`: the layers cost between a tenth and half the frame rate, and
# `bench` will say so.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug"

# Configured once. `--clean-first` is never used here: it deletes files/lang/*.ts, which are source.
#
# **The flags are overridden because CMake's own `RelWithDebInfo` carries `-DNDEBUG`**, and that
# compiles out every `assert` in the fork — the contracts this code states are then checked by
# nothing at all, in the one build anybody develops in. `release.sh` is where `NDEBUG` belongs.
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
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
fi

# `game` runs OpenMW itself on the quicksave; anything else goes to the harness. Two entry points
# rather than two more scripts, because the build directory and its configure line are the whole of
# what the two share and the whole of what makes them different from each other.
if [ "${1-}" = game ]; then
    shift
    cmake --build "$build" -j32 --target openmw
    cd "$build"
    exec ./openmw --skip-menu --load-savegame "$HOME/.local/share/openmw/saves/asd/Quicksave.omwsave" "$@"
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

# From the build directory, because --resources defaults to ./resources.
cd "$build"
exec ./openmw-rtxtool "${verb[@]}" --validation --sync-validation "$@"
