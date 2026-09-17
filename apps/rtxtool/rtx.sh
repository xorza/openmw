#!/usr/bin/env bash
# The one way in: a build flavour, then what to do with it.
#
#   rtx.sh <flavour> build [targets]        configure once, build the harness, the game and the tests,
#                                           or the targets named
#   rtx.sh <flavour> test [gtest args]      components-tests Rtx* Sky*, then openmw-tests Rtx*
#   rtx.sh <flavour> game [args]            openmw on the quicksave
#   rtx.sh <flavour> repeat [--pairs=N] [bench args]
#                                           two runs of one binary walk one place and must agree
#   rtx.sh <flavour> gate                   clang-format 14, build, the release and no-DLSS compiles, test,
#                                           check, repeat — stops at the first failure
#   rtx.sh <flavour> <verb> [args]          openmw-rtxtool <verb>, under the flavour's validation
#
#   flavour   directory          what it is
#   debug     build-debug        -O2 -g with every assert, the layers on with synchronization
#                                validation: the everyday build
#   release   build-release      -O3 -DNDEBUG, no layers, line tables and frame pointers so perf can
#                                name a line: the build a number is quoted from
#   asan      build-debug-asan   debug under AddressSanitizer; `LSAN=1` turns the leak check back on
#   nodlss    build-nodlss       debug with -DOPENMW_RTX_DLSS=OFF: the other binary, whose upscaler
#                                refuses every mode by name and whose DLSS test skips
#
# **One grammar for four builds**, because three scripts carried three: one had verbs only, one
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
#
# **The same grammar under Git Bash on Windows, with MSVC.** Upstream's prebuilt dependency set is
# fetched into `deps/` the way `windows.yml` fetches it, the compiler is found through vswhere and
# its environment captured into this shell, and the flags below are MSVC's where GCC's are named.
# The Vulkan SDK, CMake and Ninja are the installers' and stay on the PATH they put themselves on;
# `NGX_ROOT` names the checkout as it does anywhere. `asan` is refused: MSVC has a sanitizer, and
# nobody has measured this tree under it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Bash's own word for the system it was built for, which no environment variable can misstate:
# `msys` under Git Bash, `linux-gnu` on the desk.
onWindows() {
    case "$OSTYPE" in
        msys* | cygwin*) return 0 ;;
        *) return 1 ;;
    esac
}

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
    # **The flags are the compiler's, chosen once.** MSVC's `RelWithDebInfo` carries `/DNDEBUG`
    # exactly as GCC's carries `-DNDEBUG`, and `-O2 -Zi` is its `-O2 -g`. For the measured build,
    # `-Zi` with `-DEBUG` at link is the line table a profiler names a line from, and `-Oy-` keeps
    # the frame pointers a stack walk reads. Spelt with a dash, which every Microsoft tool takes,
    # because Git Bash turns `/O2` into `C:/Program Files/Git/O2` on its way to cmake. The
    # flags for every configuration go through `CMAKE_<LANG>_FLAGS_INIT`, which CMake appends its
    # platform's own to — `/EHsc` and `/machine:x64` among them, which `CMAKE_<LANG>_FLAGS` set
    # outright would drop.
    local asserts profiling profilingLink
    if onWindows; then
        asserts="-O2 -Zi"
        profiling="-Zi -Oy-"
        profilingLink=(-DCMAKE_EXE_LINKER_FLAGS_INIT=-DEBUG)
    else
        asserts="-O2 -g"
        profiling="-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
        profilingLink=()
    fi

    case "$1" in
        debug)
            build="$root/build-debug"
            validation=sync
            configure=(
                -DCMAKE_BUILD_TYPE=RelWithDebInfo
                -DCMAKE_C_FLAGS_RELWITHDEBINFO="$asserts" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="$asserts"
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
            configure=(
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_FLAGS_INIT="$profiling" -DCMAKE_CXX_FLAGS_INIT="$profiling"
                "${profilingLink[@]}"
                -DBUILD_COMPONENTS_TESTS=OFF -DBUILD_OPENMW_TESTS=OFF
                -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_LAUNCHER=OFF
                -DBUILD_NAVMESHTOOL=OFF -DBUILD_NIFTEST=OFF -DBUILD_BULLETOBJECTTOOL=OFF
            )
            targets=(openmw-rtxtool openmw)
            ;;
        asan)
            if onWindows; then
                echo "rtx.sh: asan is Linux's here — MSVC's sanitizer has not been measured on this tree" >&2
                exit 2
            fi
            build="$root/build-debug-asan"
            validation=sync
            sanitize="-fsanitize=address -fno-omit-frame-pointer"
            configure=(
                -DCMAKE_BUILD_TYPE=RelWithDebInfo
                -DCMAKE_C_FLAGS_RELWITHDEBINFO="$asserts" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="$asserts"
                -DCMAKE_C_FLAGS_INIT="$sanitize" -DCMAKE_CXX_FLAGS_INIT="$sanitize"
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
        nodlss)
            # **The build the SDK is absent from, which is a different binary and not the same one
            # with a feature skipped**: `noupscaler.cpp` links where `dlss*.cpp` did, and every file
            # that reaches NGX has to be behind the `#ifdef`. The gate compiles it so that a symbol
            # reached from the wrong side is found here and not by whoever next configures without
            # the SDK.
            build="$root/build-nodlss"
            validation=sync
            configure=(
                -DCMAKE_BUILD_TYPE=RelWithDebInfo
                -DCMAKE_C_FLAGS_RELWITHDEBINFO="$asserts" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="$asserts"
                -DOPENMW_RTX_DLSS=OFF
                -DBUILD_COMPONENTS_TESTS=ON -DBUILD_OPENMW_TESTS=ON
                -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_LAUNCHER=OFF
                -DBUILD_NAVMESHTOOL=OFF -DBUILD_NIFTEST=OFF -DBUILD_BULLETOBJECTTOOL=OFF
            )
            targets=(openmw-rtxtool openmw components-tests openmw-tests)
            ;;
        *)
            echo "rtx.sh: no flavour is called '$1' — debug, release, asan or nodlss" >&2
            exit 2
            ;;
    esac
}

# **MSVC's environment, captured into this shell.** `cl` finds its headers and libraries through
# `INCLUDE` and `LIB`, which Ninja does not bake into the build, so every build needs them and not
# only the configure. `VsDevCmd.bat` sets them for the installation vswhere names, and `VSCMD_VER`
# is the documented sign that it already has. What is imported is the difference between two
# `declare -px` dumps taken inside the same `cmd`, one before the batch file and one after: the
# variables it set and nothing else, with MSYS's own conversion of the PATH. They are sourced as
# `export` lines, because a `declare` sourced inside a function declares locals. Each command in
# the `cmd` line starts with `call`, since `cmd /c` strips the quotes off a line that starts with
# one; vswhere's directory is on the PATH for it because the batch file looks for it there and
# says so when it cannot; and `-startdir=none` keeps `VSCMD_START_DIR` from moving the shell.
activateMsvc() {
    if [ -n "${VSCMD_VER-}" ]; then
        return
    fi

    local installer="/c/Program Files (x86)/Microsoft Visual Studio/Installer"
    if [ ! -x "$installer/vswhere.exe" ]; then
        echo "rtx.sh: no Visual Studio installer at $installer, so no vswhere to find a compiler with" >&2
        exit 2
    fi

    local studio
    studio="$("$installer/vswhere.exe" -latest -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)"
    if [ -z "$studio" ]; then
        echo "rtx.sh: vswhere found no Visual Studio with the C++ tools" >&2
        exit 2
    fi

    local captured bash
    captured="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$captured'" RETURN
    bash="$(cygpath -w "$BASH")"
    PATH="$installer:$PATH" cmd //c \
        call "$bash" -c "declare -px > '$captured/before'" \
        "&&" call "$studio\\Common7\\Tools\\VsDevCmd.bat" -no_logo -arch=amd64 -host_arch=amd64 -startdir=none \
        "&&" call "$bash" -c "declare -px > '$captured/after'"

    comm -13 <(sort "$captured/before") <(sort "$captured/after") | sed 's/^declare -x /export /' > "$captured/changed"
    # shellcheck disable=SC1091
    source "$captured/changed"

    if [ -z "${VSCMD_VER-}" ] || ! command -v cl > /dev/null 2>&1; then
        echo "rtx.sh: VsDevCmd.bat ran and left no compiler on the PATH" >&2
        exit 2
    fi
}

# **Upstream's prebuilt dependency set, the way `windows.yml` gets it.** A manifest on
# `openmw-deps` names the archive and its SHA-512 for the tag `CI/deps_versions.msvc.sh` pins —
# the 2022 set, which upstream's own script links against Visual Studio 2026 as well. The
# archive is fetched over TLS only, checked, and unpacked beside its final name, which it takes
# only once the whole of it is there: a run cut off mid-way leaves nothing a later run mistakes
# for a set. 7-Zip is the one tool the SDK installers do not bring, and it is looked for where
# its installer puts it.
fetchWindowsDeps() {
    if [ -d "$deps" ]; then
        return
    fi

    local sevenZip
    sevenZip="$(command -v 7z || echo "/c/Program Files/7-Zip/7z.exe")"
    if [ ! -x "$sevenZip" ]; then
        echo "rtx.sh: the dependency archive is a .7z and there is no 7-Zip: \`winget install 7zip.7zip\`" >&2
        exit 2
    fi

    local manifest fetch
    manifest="$(basename "$deps")-manifest.txt"
    fetch=(curl --proto '=https' --fail --location --silent --show-error --retry 3)
    mkdir -p "$root/deps"
    (
        cd "$root/deps"
        "${fetch[@]}" --output "$manifest" "https://gitlab.com/OpenMW/openmw-deps/-/raw/main/windows/$manifest"
        local url archive
        url="$(sed -n '1p' "$manifest")"
        archive="$(sed -n '2p' "$manifest" | awk '{ print $2 }')"
        echo "fetching $archive"
        "${fetch[@]}" --output "$archive" "$url"
        # The second line alone: the first is the URL, which sha512sum would call malformed.
        sed -n '2p' "$manifest" | sha512sum --check --quiet --strict -
        rm -rf "$deps.partial"
        "$sevenZip" x -y -o"$deps.partial" "$archive" > /dev/null
        mv "$deps.partial" "$deps"
        rm -f "$archive"
    )
}

# Configured once, which is the one moment the SDK is looked for: `NGX_ROOT` in the environment
# names the checkout, as CMake's own `find_package` reads it, and a build directory remembers what
# it found. `--clean-first` is never used here: it deletes files/lang/*.ts, which are source.
#
# **What the platform supplies.** Linux has recastnavigation and Google Test as packages, and
# ccache and mold to build with. Windows has upstream's vcpkg set, which carries neither of those
# nor SQLite, yaml-cpp or Qt: the first four CMake fetches, and the launcher stays unbuilt.
configureIfNeeded() {
    if [ -f "$build/CMakeCache.txt" ]; then
        return
    fi

    local platform=()
    if onWindows; then
        fetchWindowsDeps
        platform=(
            -DCMAKE_TOOLCHAIN_FILE="$deps/scripts/buildsystems/vcpkg.cmake"
            -DLuaJit_INCLUDE_DIR="$deps/installed/x64-windows/include/luajit"
            -DLuaJit_LIBRARY="$deps/installed/x64-windows/lib/lua51.lib"
            -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=OFF -DOPENMW_USE_SYSTEM_GOOGLETEST=OFF
            -DOPENMW_USE_SYSTEM_SQLITE3=OFF -DOPENMW_USE_SYSTEM_YAML_CPP=OFF
            -DBUILD_LAUNCHER=OFF
        )
    else
        platform=(
            -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=ON
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
            -DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=mold
        )
    fi

    cmake -S "$root" -B "$build" -G Ninja \
        -DOPENMW_RTX=ON \
        -DBUILD_OPENCS=OFF -DBUILD_WIZARD=OFF -DBUILD_ESSIMPORTER=OFF \
        -DBUILD_MWINIIMPORTER=OFF -DBUILD_OPENCS_TESTS=OFF \
        "${platform[@]}" \
        "${configure[@]}"

    if onWindows; then
        placeWindowsRuntime
    fi
}

# **What vcpkg's own copy step misses, placed the way upstream's MSVC script places it.** The
# toolchain copies each DLL a binary links beside it and stops there: MyGUI's sits a directory
# deeper than it looks and needs FreeType, which needs four more, and OSG's plugins are loaded by
# name at runtime from `osgPlugins-3.6.5/` beside the binary, which no link line names. The whole
# set, 84 MB, rather than a list that goes stale with the next tag.
placeWindowsRuntime() {
    local bin="$deps/installed/x64-windows/bin"
    cp "$bin"/*.dll "$bin/Release/MyGUIEngine.dll" "$build/"
    mkdir -p "$build/osgPlugins-3.6.5"
    cp "$bin"/osgPlugins-3.6.5/*.dll "$build/osgPlugins-3.6.5/"
}

describeFlavour "$flavour"
if onWindows; then
    activateMsvc
    # The tag upstream's own MSVC script reads the same way, and the set it names.
    # shellcheck disable=SC1091
    source "$root/CI/deps_versions.msvc.sh"
    deps="$root/deps/vcpkg-x64-windows-2022-${VCPKG_TAG:?}"
fi
configureIfNeeded

# **Run from the build directory**, because `--resources` defaults to `./resources`, and the tests
# that read game data resolve it the way the tool does.
# `-k 0` keeps ninja going past a failed object, so one run of a build that breaks names every
# object that broke — on a compiler this box does not have, that is the difference between one
# round trip through CI and five.
buildTargets() {
    cmake --build "$build" -j"$(nproc)" --target "$@" -- -k 0
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

# **The backend without the SDK, compiled the way a machine without one compiles it.** The backend
# alone, because that is the one library the `#ifdef` divides; the harness and the game link it
# either way. About a minute warm, in a subshell for the reason above.
compileWithoutDlss() {
    (
        describeFlavour nodlss
        configureIfNeeded
        buildTargets openmw-rtx-vulkan
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
            # The skipped line too: without a device every RTX test skips, and a log that says so is
            # the difference between a pass and a machine with nothing to test on.
            grep -E '^\[==========\] .* ran|^\[  PASSED  \]|^\[  SKIPPED \]' "$out/$shard.log" \
                | sed "s/^/shard $shard: /"
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
        # The flavour's own targets, or the ones the line names: CI compiles the backend alone in
        # the `nodlss` flavour, the way the gate does.
        buildTargets "${@:-${targets[@]}}"
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
        compileWithoutDlss
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
