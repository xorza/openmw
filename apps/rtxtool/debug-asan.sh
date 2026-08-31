#!/usr/bin/env bash
# Builds under AddressSanitizer and runs the component tests. Extra arguments go to the test
# binary: `debug-asan.sh --gtest_filter='RtxGui*'`.
#
# `debug-asan.sh tool shot --view=balmora` runs the harness instead, and `debug-asan.sh game` runs
# OpenMW itself on the quicksave. **Nothing here is a measurement**: `release.sh` is the build a
# number comes from, and this one carries the sanitizer and the validation layers as well.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug-asan"

# **The same override `debug.sh` makes, for the same reason**: CMake's `RelWithDebInfo` carries
# `-DNDEBUG`, and a sanitizer build that has compiled out every `assert` in the fork is checking
# half of what it could.
#
# **Fewer targets than `debug.sh`.** The launcher and the content tools are Qt and ESM code that
# this fork does not touch, and building them under ASan doubles the wait for nothing.
if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g" \
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g" \
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DOPENMW_RTX=ON \
        -DOPENMW_DLSS_SDK="${OPENMW_DLSS_SDK:?point OPENMW_DLSS_SDK at an unpacked DLSS SDK}" \
        -DBUILD_COMPONENTS_TESTS=ON -DBUILD_OPENMW_TESTS=OFF \
        -DBUILD_OPENCS=OFF -DBUILD_WIZARD=OFF -DBUILD_ESSIMPORTER=OFF \
        -DBUILD_MWINIIMPORTER=OFF -DBUILD_OPENCS_TESTS=OFF \
        -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_LAUNCHER=OFF \
        -DBUILD_NAVMESHTOOL=OFF -DBUILD_NIFTEST=OFF -DBUILD_BULLETOBJECTTOOL=OFF \
        -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
fi

# **`protect_shadow_gap=0` is not a preference — without it there is no device at all.** The NVIDIA
# driver maps its own enormous address ranges, ASan's shadow gap is mapped `PROT_NONE` across part
# of what it wants, and `vkCreateDevice` comes back `VK_ERROR_INITIALIZATION_FAILED`. Every RTX test
# then *skips*, which reads exactly like a clean run. What it costs is the guard page that catches a
# wild pointer landing in the gap; every other check ASan makes is untouched.
#
# **The driver's own allocations are not this fork's to answer for.** LeakSanitizer walks a heap
# that the driver, the loader, the validation layers and dbus keep for the life of the process, and
# reports each with a stack the fork does not appear in. `LSAN=1 debug-asan.sh` turns it back on for
# a run that wants it.
options="protect_shadow_gap=0"
if [ "${LSAN-0}" != 1 ]; then
    options="$options:detect_leaks=0"
fi

export ASAN_OPTIONS="$options${ASAN_OPTIONS:+:$ASAN_OPTIONS}"

# Three entry points off argv[1] — the engine, the harness, and the tests, which are the default
# because they are what this build is mostly for. Three of them rather than three more scripts,
# because the build directory and its configure line are the whole of what they share.
#
# **`tool` rather than the harness's own verbs.** There are nine of those, `dispatch` in `main.cpp`
# owns the list, and a second copy here would be one more thing to keep in step with it.
if [ "${1-}" = game ]; then
    shift
    cmake --build "$build" -j32 --target openmw
    cd "$build"
    exec ./openmw --skip-menu --load-savegame "$HOME/.local/share/openmw/saves/asd/Quicksave.omwsave" "$@"
fi

if [ "${1-}" = tool ]; then
    shift
    cmake --build "$build" -j32 --target openmw-rtxtool

    # From the build directory, because --resources defaults to ./resources. The switches go last,
    # so whatever the caller named stays at argv[1] — `dispatch` takes a leading dash there to mean
    # nobody named a verb, and runs `view`.
    cd "$build"
    exec ./openmw-rtxtool "$@" --validation --sync-validation
fi

cmake --build "$build" -j32 --target components-tests

# From the build directory, because the tests that read game data resolve it the way the tool does.
cd "$build"
exec ./components-tests "$@"
