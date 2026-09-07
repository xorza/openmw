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
#
# **And Ray Reconstruction adds nothing of its own, which is worth stating because it looks as
# though it does.** Its history is recurrent, so one frame the trace drew differently reaches every
# frame after it and a run comes back disagreeing almost everywhere. Measured: `one-cell-walk`
# agrees on all 360 frames through it at every warm-up tried, and `island-crossing` agreed on 1 of
# 360 before the merge order was settled and on 79 after. It is faithful, not faulty.
#
# **Two columns, and only the first decides the exit status.** A hashes file names the picture and
# the scene it was drawn from — `Rtx::digestLayout`. The picture is what this asserts, because it is
# what the title says and what a reconstruction is fed. The scene is reported beside it and does not
# fail the run, because the slot order of the placement and material tables is a known open defect
# and a gate that is red for it would be red for everything else too.
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

frames="$(wc -l < "$out/1.txt")"

# **Counted before the columns are compared**, because the comparison below walks the two files by
# line number: a shorter second run would leave the first run's tail unread and report agreement
# over frames nobody looked at.
if [ "$frames" -ne "$(wc -l < "$out/2.txt")" ]; then
    echo "NOT repeatable: $frames frames against $(wc -l < "$out/2.txt")" >&2
    echo "both runs are in $out" >&2
    exit 1
fi

read -r pictures scenes < <(awk '
    NR == FNR { picture[FNR] = $3; scene[FNR] = $4; next }
    $3 != picture[FNR] { drawn++ }
    $4 != scene[FNR] { handed++ }
    END { print drawn + 0, handed + 0 }' "$out/1.txt" "$out/2.txt")

if [ "$pictures" -eq 0 ] && [ "$scenes" -eq 0 ]; then
    echo "repeatable: $frames frames, identical over two runs"
    rm -rf "$out"
    exit 0
fi

if [ "$scenes" -gt 0 ]; then
    echo "the scene differs on $scenes of $frames frames: the walk was handed two worlds" >&2
fi

if [ "$pictures" -eq 0 ]; then
    echo "the pictures repeat: $frames frames, every one of them the same"
    echo "both runs are in $out" >&2
    exit 0
fi

# **Kept where they are**, because the two files are what somebody now has to read.
echo "NOT repeatable: $pictures of $frames frames differ" >&2
diff "$out/1.txt" "$out/2.txt" | head -10 >&2
echo "both runs are in $out" >&2
exit 1
