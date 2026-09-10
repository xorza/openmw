#!/usr/bin/env bash
# Builds with debug info, under the validation layers. With no argument it stops at the binary.
# With a verb it runs the harness on it: `debug.sh view --view=balmora` opens the window,
# `debug.sh shot --view=balmora` writes a frame.
#
# **There is no `game` entry point any more, because every verb is one.** `view` opens a window on
# the game with the player own camera, and `--load-savegame=<file>` starts from a save rather than
# from a new game. Profiling belongs in `release.sh` and `profile.sh`: the layers cost between a
# tenth and half the frame rate, and `bench` will say so.
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

# **No argument, or `build`, stops at the binary.** A run needs a verb named on the command line:
# the tool's own default is `view`, and a game window is not something a build should open on its
# own.
if [ $# -eq 0 ] || [ "${1}" = build ]; then
    exec cmake --build "$build" -j32 --target openmw-rtxtool
fi

if [[ "${1}" == -* ]]; then
    echo "debug.sh: name a verb before the switches — \`debug.sh view $*\`" >&2
    exit 2
fi

cmake --build "$build" -j32 --target openmw-rtxtool

# **The verb stays first.** `dispatch` reads it off argv[1] and takes a leading dash to mean nobody
# named one, so appending it after the switches below silently ran `view` instead — `release.sh
# bench` opened a window and profiled nothing.
verb="$1"
shift

# From the build directory, because --resources defaults to ./resources.
cd "$build"
exec ./openmw-rtxtool "$verb" --validation --sync-validation "$@"
