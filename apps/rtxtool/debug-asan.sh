#!/usr/bin/env bash
# Runs component tests under ASan; `tool` runs the harness and `game` runs OpenMW.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug-asan"

# Keep assertions enabled: CMake defaults RelWithDebInfo to -DNDEBUG.
# Exclude unrelated Qt and content tools from the sanitizer build.
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
        -DCMAKE_C_COMPILER_LAUNCHER="ccache;cache_dir=$root/build-cache/ccache" \
        -DCMAKE_CXX_COMPILER_LAUNCHER="ccache;cache_dir=$root/build-cache/ccache" \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
fi

# NVIDIA maps memory inside ASan’s shadow gap; protecting it prevents device creation.
# Disabling that guard leaves the other ASan checks enabled.
# Driver and loader lifetime allocations trigger LeakSanitizer; LSAN=1 enables that check.
options="protect_shadow_gap=0"
if [ "${LSAN-0}" != 1 ]; then
    options="$options:detect_leaks=0"
fi

export ASAN_OPTIONS="$options${ASAN_OPTIONS:+:$ASAN_OPTIONS}"

if [ "${1-}" = game ]; then
    shift
    cmake --build "$build" -j32 --target openmw
    cd "$build"
    exec ./openmw --skip-menu --load-savegame "$HOME/.local/share/openmw/saves/asd/Quicksave.omwsave" "$@"
fi

if [ "${1-}" = tool ]; then
    shift
    cmake --build "$build" -j32 --target openmw-rtxtool

    # Resources resolve from the build directory; the verb must remain argv[1].
    cd "$build"
    exec ./openmw-rtxtool "$@" --validation --sync-validation
fi

cmake --build "$build" -j32 --target components-tests

# From the build directory, because the tests that read game data resolve it the way the tool does.
cd "$build"
exec ./components-tests "$@"
