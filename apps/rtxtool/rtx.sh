#!/usr/bin/env bash
# The one way in: a build flavour, then what to do with it.
#
#   rtx.sh <flavour> build                  configure once, build the harness, the game and the tests
#   rtx.sh <flavour> test [gtest args]      components-tests Rtx* Sky*, then openmw-tests Rtx*
#   rtx.sh <flavour> game [args]            openmw on the quicksave
#   rtx.sh <flavour> repeat [--pairs=N] [bench args]
#                                           two runs of one binary walk one place and must agree
#   rtx.sh <flavour> gate                   clang-format 14, build, test, check, repeat — stops at the first failure
#   rtx.sh <flavour> <verb> [args]          openmw-rtxtool <verb>, under the flavour's validation
#
#   flavour   directory          what it is
#   debug     build-debug        -O2 -g with every assert, the layers on with synchronization
#                                validation: the everyday build
#   release   build-release      -O3 -DNDEBUG, no layers, line tables and frame pointers so perf can
#                                name a line: the build a number is quoted from
#   asan      build-debug-asan   debug under AddressSanitizer; `LSAN=1` turns the leak check back on
#
# **One grammar for three builds**, because three scripts carried three: one had verbs only, one
# had tests by default and `tool` in front of a verb, and none built the game. What differed
# between them is the table below, and everything else is the same eight lines.
#
# **The flags are overridden because CMake's own `RelWithDebInfo` carries `-DNDEBUG`**, and that
# compiles out every `assert` in the fork — the contracts this code states are then checked by
# nothing at all, in the one build anybody develops in. `release` is where `NDEBUG` belongs.
#
# **The verb stays first.** `dispatch` reads it off argv[1] and takes a leading dash to mean nobody
# named one, so a switch put before it silently ran `view` instead — a `bench` typed after them once opened
# a window and profiled nothing.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

flavour="${1-}"
what="${2-}"
if [ -z "$flavour" ] || [ -z "$what" ]; then
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi
shift 2

# **A sanitizer build compiles out nothing and links no Qt.** The launcher and the content tools are
# Qt and ESM code this fork does not touch, and building them under ASan doubles the wait for
# nothing; a release build builds no tests, because nothing measured is measured through one.
#
# A function, because the gate reaches for a second flavour: what a build without asserts compiles
# is a question the debug build cannot answer.
describeFlavour() {
    case "$1" in
        debug)
            build="$root/build-debug"
            validation=sync
            configure=(
                -DCMAKE_BUILD_TYPE=RelWithDebInfo
                -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g"
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                -DBUILD_COMPONENTS_TESTS=ON -DBUILD_OPENMW_TESTS=ON
            )
            targets=(openmw-rtxtool openmw components-tests openmw-tests)
            ;;
        release)
            # **Light debug data and frame pointers, in the build the numbers are quoted from.** `-g1`
            # is line tables and nothing else, so it costs nothing at runtime and a profile can name a
            # line rather than an offset; the frame pointers cost less than the run-to-run spread and
            # are what let perf walk a stack for the price of reading it. Both on the measured build
            # rather than on a profiling build beside it, because two binaries means the profile
            # explains a frame the benchmark did not time.
            build="$root/build-release"
            validation=off
            profiling="-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
            configure=(
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_FLAGS="$profiling" -DCMAKE_CXX_FLAGS="$profiling"
                -DBUILD_COMPONENTS_TESTS=OFF -DBUILD_OPENMW_TESTS=OFF
                -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_LAUNCHER=OFF
                -DBUILD_NAVMESHTOOL=OFF -DBUILD_NIFTEST=OFF -DBUILD_BULLETOBJECTTOOL=OFF
            )
            targets=(openmw-rtxtool openmw)
            ;;
        asan)
            build="$root/build-debug-asan"
            validation=sync
            sanitize="-fsanitize=address -fno-omit-frame-pointer"
            configure=(
                -DCMAKE_BUILD_TYPE=RelWithDebInfo
                -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g"
                -DCMAKE_C_FLAGS="$sanitize" -DCMAKE_CXX_FLAGS="$sanitize"
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                -DBUILD_COMPONENTS_TESTS=ON -DBUILD_OPENMW_TESTS=ON
                -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_LAUNCHER=OFF
                -DBUILD_NAVMESHTOOL=OFF -DBUILD_NIFTEST=OFF -DBUILD_BULLETOBJECTTOOL=OFF
            )
            targets=(openmw-rtxtool openmw components-tests openmw-tests)

            # **`protect_shadow_gap=0` is not a preference — without it there is no device at all.**
            # The NVIDIA driver maps its own enormous address ranges, ASan's shadow gap is mapped
            # `PROT_NONE` across part of what it wants, and `vkCreateDevice` comes back
            # `VK_ERROR_INITIALIZATION_FAILED`. Every RTX test then *skips*, which reads exactly like a
            # clean run. What it costs is the guard page that catches a wild pointer landing in the
            # gap; every other check ASan makes is untouched.
            #
            # **The driver's own allocations are not this fork's to answer for.** LeakSanitizer walks a
            # heap that the driver, the loader, the validation layers and dbus keep for the life of the
            # process, and reports each with a stack the fork does not appear in.
            options="protect_shadow_gap=0"
            if [ "${LSAN-0}" != 1 ]; then
                options="$options:detect_leaks=0"
            fi
            export ASAN_OPTIONS="$options${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
            ;;
        *)
            echo "rtx.sh: no flavour is called '$1' — debug, release or asan" >&2
            exit 2
            ;;
    esac
}

# Configured once, which is the one moment the SDK has to be named. `--clean-first` is never used
# here: it deletes files/lang/*.ts, which are source.
configureIfNeeded() {
    if [ ! -f "$build/CMakeCache.txt" ]; then
        cmake -S "$root" -B "$build" -G Ninja \
            -DOPENMW_RTX=ON \
            -DOPENMW_DLSS_SDK="${OPENMW_DLSS_SDK:?point OPENMW_DLSS_SDK at an unpacked DLSS SDK}" \
            -DBUILD_OPENCS=OFF -DBUILD_WIZARD=OFF -DBUILD_ESSIMPORTER=OFF \
            -DBUILD_MWINIIMPORTER=OFF -DBUILD_OPENCS_TESTS=OFF \
            -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=ON \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
            "${configure[@]}"
    fi
}

describeFlavour "$flavour"
configureIfNeeded

# **Run from the build directory**, because `--resources` defaults to `./resources`, and the tests
# that read game data resolve it the way the tool does.
buildTargets() {
    cmake --build "$build" -j"$(nproc)" --target "$@"
}

# The flavour's layers, unless the line names a level of its own: a typed level wins.
validationFor() {
    for arg in "$@"; do
        case "$arg" in
            --validation=*) return ;;
        esac
    done
    echo "--validation=$validation"
}

# **Every source in the working tree, and not every one git tracks.** CI's own script walks
# `git ls-files`, which names a file a move has deleted and misses one it has created — so a tree
# with an uncommitted move failed it on the file that was gone and passed it on the file that was
# new. CI pins clang-format 14; this box has 22 and they disagree.
checkFormat() {
    (cd "$root" && git ls-files --cached --others --exclude-standard -- ':(exclude)extern/' '*.cpp' '*.hpp' '*.h' \
        | while read -r file; do [ -f "$file" ] && printf '%s\n' "$file"; done \
        | xargs -P "$(nproc)" clang-format-14 --dry-run -Werror)
}

# **The fork's two libraries, compiled the way a number is taken.** `-DNDEBUG` compiles every
# assert out, and a diagnostic that fires only once the assert is gone — a lookup the optimizer
# can now prove reaches a null — is one the debug build never sees. A minute with the cache warm,
# in a subshell so the gate's own flavour stays what it was.
compileWithoutAsserts() {
    (
        describeFlavour release
        configureIfNeeded
        buildTargets openmw-rtx openmw-rtx-vulkan
    )
}

# **Three processes for the components tests**, through gtest's own sharding, because the pixel
# tests are one process waiting on the device: a frame, a readback, a wait. Each shard pays the
# device and the shared renderer again, in parallel, and the three together take about half the
# wall time of one. The pipeline cache writes through a uniquely named temporary, so three
# closing at once is a rename each. Each shard's output goes to a file of its own and is printed
# whole where it failed, so a failure reads as one process's.
runTests() {
    if [ ! -x "$build/components-tests" ]; then
        echo "the $flavour build has no tests: \`rtx.sh debug test\` runs them" >&2
        return 1
    fi

    local shards=3 out status=0 shard
    out="$(mktemp -d)"
    for shard in $(seq 0 $((shards - 1))); do
        (cd "$build" && GTEST_TOTAL_SHARDS="$shards" GTEST_SHARD_INDEX="$shard" \
            ./components-tests --gtest_filter='Rtx*:Sky*' "$@" > "$out/$shard.log" 2>&1) &
    done
    for shard in $(seq 0 $((shards - 1))); do
        if ! wait -n; then
            status=1
        fi
    done
    for shard in $(seq 0 $((shards - 1))); do
        if grep -q '^\[  PASSED  \]' "$out/$shard.log" && ! grep -q '^\[  FAILED  \]' "$out/$shard.log"; then
            grep -E '^\[==========\] .* ran|^\[  PASSED  \]' "$out/$shard.log" | sed "s/^/shard $shard: /"
        else
            status=1
            echo "shard $shard failed:" >&2
            cat "$out/$shard.log" >&2
        fi
    done
    rm -rf "$out"
    [ "$status" -eq 0 ] || return 1

    (cd "$build" && ./openmw-tests --gtest_filter='Rtx*' "$@")
}

# **Two processes and not two stops of one.** A second stop starts from the world the first one
# left, so the two cannot be compared frame for frame. What this asks is whether a run of the binary
# is a function of the binary, and only a second run of it answers that. A walk and not a still,
# because a still passed through both of the defects this catches: a camera that stands still was
# exactly reproducible while `osg::FrameStamp`'s reference time aged OpenMW's caches by the wall,
# and again while MyGUI aged the hit overlay by one.
#
# **The upscaler and the denoiser are off**, so what is walked is what a reconstruction is fed,
# which is where every defect this has caught showed itself — a picture that moved here is one the
# trace drew differently, with nothing temporal between. `--exposure=1` reads a difference in the
# picture: a measured exposure couples every pixel to every other and every frame to the one before.
#
# **Every column is the gate.** The second run is compared by `bench --against`, which names the
# frames whose picture moved and the parts of the scene that did, and fails on either. One pair
# gates and ten read: `.notes/repeatable.txt` holds the readings.
#
# **The second leg runs with the queue held behind the host.** Two runs of one binary keep the
# same phase between the host and the device, so a frame that read the device's clock — a report
# that had or had not landed, a structure compacted a frame earlier — could repeat exactly and
# still be a function of the wall; `Rtx::Timeline` says why no frame reads it now. Held, the
# device trails by half a frame, which is the other phase a run can have: a picture that is a
# function of the frames alone does not move between the two.
runRepeat() {
    local pairs=1 place=() length=() extra=()
    for arg in "$@"; do
        case "$arg" in
            --pairs=*) pairs="${arg#*=}" ;;
            --views=*|--suite=*) place+=("$arg") ;;
            --seconds=*|--frames=*) length+=("$arg") ;;
            *) extra+=("$arg") ;;
        esac
    done
    [ ${#place[@]} -eq 0 ] && place=(--views=one-cell-walk)
    [ ${#length[@]} -eq 0 ] && length=(--seconds=6)

    local out
    out="$(mktemp -d)"
    local bench=(./openmw-rtxtool bench "${place[@]}" "${length[@]}" --window=false --upscale=off --filter=false
        --validation=off "${extra[@]}")

    local pair status=0
    for pair in $(seq 1 "$pairs"); do
        (cd "$build" && "${bench[@]}" --hashes="$out/$pair.csv" > "$out/$pair-1.log" 2>&1) || {
            echo "the run itself failed, see $out/$pair-1.log:" >&2
            tail -20 "$out/$pair-1.log" >&2
            return 1
        }
        if (cd "$build" && "${bench[@]}" --hold=8 --against="$out/$pair.csv" > "$out/$pair-2.log" 2>&1); then
            echo "pair $pair of $pairs: identical"
        elif grep -q '^against ' "$out/$pair-2.log"; then
            status=1
            echo "pair $pair of $pairs: NOT repeatable" >&2
            sed -n '/^against /,$p' "$out/$pair-2.log" >&2
        else
            echo "the run itself failed, see $out/$pair-2.log:" >&2
            tail -20 "$out/$pair-2.log" >&2
            return 1
        fi
    done

    if [ "$status" -eq 0 ]; then
        echo "repeat: $pairs pair(s), identical over every one"
        rm -rf "$out"
    else
        echo "the runs are in $out" >&2
    fi
    return "$status"
}

case "$what" in
    build)
        buildTargets "${targets[@]}"
        ;;
    test)
        buildTargets components-tests openmw-tests
        runTests "$@"
        ;;
    game)
        buildTargets openmw
        cd "$build"
        exec ./openmw --skip-menu --load-savegame "$HOME/.local/share/openmw/saves/asd/Quicksave.omwsave" "$@"
        ;;
    repeat)
        buildTargets openmw-rtxtool
        runRepeat "$@"
        ;;
    gate)
        # **Never a gate beside a build, or beside another gate**: the reading is then about the
        # machine. In order of cost, so a formatting slip is found in seconds and not after the
        # walk. `check` writes its pictures where it always does, under `check/`.
        checkFormat
        buildTargets "${targets[@]}"
        compileWithoutAsserts
        if [ -x "$build/components-tests" ]; then
            runTests
        fi
        (cd "$build" && ./openmw-rtxtool check "--validation=$validation")
        runRepeat --pairs=1
        echo "gate: clean"
        ;;
    -*)
        echo "rtx.sh: name a verb before the switches — \`rtx.sh $flavour view $what $*\`" >&2
        exit 2
        ;;
    *)
        buildTargets openmw-rtxtool
        cd "$build"
        exec ./openmw-rtxtool "$what" "$@" $(validationFor "$@")
        ;;
esac
