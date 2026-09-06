#!/usr/bin/env bash
# Asserts that a walk draws the same frames twice.
#
#   repeatable.sh                       # the default walk, six seconds
#   repeatable.sh --views=balmora       # somewhere else
#   repeatable.sh --seconds=20          # for longer
#   repeatable.sh --build=build-release # against another build
#
# Anything else goes to `openmw-rtxtool bench`, except the three this sets itself: `--views`,
# `--upscale` and `--filter`. Naming one of those twice is what `bench` refuses.
#
# **Two processes and not two stops of one.** A second stop starts from the world the first one
# left, so the two cannot be compared frame for frame. What this asks is whether a run of the binary
# is a function of the binary, and only a second run of it answers that.
#
# **A walk and not a still, because a still passed through both of the defects this catches.** A
# camera that stands still was exactly reproducible while `osg::FrameStamp`'s reference time aged
# OpenMW's caches by the wall, and again while MyGUI aged the hit overlay by one. Both moved the
# picture only where the frame moved.
#
# **The upscaler and the denoiser are off**, for the reason `verify` states: Ray Reconstruction is
# temporal and carries state nothing below it can hold still. What is asserted here is that the
# trace repeats, which is what a reconstruction is fed and what every one of these defects moved.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug"
place=()
length=()
extra=()

for arg in "$@"; do
    case "$arg" in
        --build=*) build="$root/${arg#*=}" ;;
        --views=*|--suite=*) place+=("$arg") ;;
        --seconds=*|--frames=*) length+=("$arg") ;;
        *) extra+=("$arg") ;;
    esac
done

if [ ${#place[@]} -eq 0 ]; then
    place=(--views=one-cell-walk)
fi

# Long enough to reach the frames where a wall clock first shows, and short enough to run twice
# without anybody minding.
if [ ${#length[@]} -eq 0 ]; then
    length=(--seconds=6)
fi

tool="$build/openmw-rtxtool"
if [ ! -x "$tool" ]; then
    echo "no $tool: build it first" >&2
    exit 1
fi

out="$(mktemp -d)"

# **Run from the build directory**, because `--resources` defaults to `./resources`.
for run in 1 2; do
    (cd "$build" && "$tool" bench "${place[@]}" "${length[@]}" --window=false --upscale=off --filter=false \
        --hashes="$out/$run.txt" "${extra[@]}") > "$out/$run.log" 2>&1 || {
        echo "the run itself failed, see $out/$run.log:" >&2
        tail -20 "$out/$run.log" >&2
        exit 1
    }
done

if cmp -s "$out/1.txt" "$out/2.txt"; then
    echo "repeatable: $(wc -l < "$out/1.txt") frames, identical over two runs"
    rm -rf "$out"
    exit 0
fi

# **Kept where they are**, because the two files are what somebody now has to read.
echo "NOT repeatable: $(diff "$out/1.txt" "$out/2.txt" | grep -c '^<') of $(wc -l < "$out/1.txt") frames differ" >&2
diff "$out/1.txt" "$out/2.txt" | head -10 >&2
echo "both runs are in $out" >&2
exit 1
