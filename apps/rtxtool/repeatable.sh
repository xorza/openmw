#!/usr/bin/env bash
# Asserts that a walk draws the same frames twice.
#
#   repeatable.sh                       # the default walk, six seconds
#   repeatable.sh --views=balmora       # somewhere else
#   repeatable.sh --seconds=20          # for longer
#   repeatable.sh --build=build-release # against another build
#   repeatable.sh --pairs=3             # three pairs, and the spread of them
#
# Anything else goes to `openmw-rtxtool bench`, except the three this sets itself: `--views`,
# `--upscale` and `--filter`. Naming one of those twice is what `bench` refuses.
#
# **Pairs and not a pair, wherever the answer is being read rather than gated.** Every defect this
# has caught so far shows on some pairs and not others, so one pair is a coin flip and a conclusion
# drawn from one is a conclusion drawn twice. `--pairs=3` costs three times a run and settles it.
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
# **A table, and only the picture decides the exit status.** A hashes file is a CSV with a header
# row and a row a frame: the picture, then a column for every part of the scene it was drawn from.
# The picture is what this asserts, because it is what the title says and what a reconstruction is
# fed. The parts are reported beside it and do not fail the run, because the slot order of the
# material and texture tables is a known open defect and a gate that is red for it would be red for
# everything else too.
#
# **Naming the columns that moved is the point of the table.** "The scene differs on 64 frames" is
# where a bisection used to start, and every step of it cost a rebuild and a run for one reading.
# The columns answer it from the two files a single pair already wrote.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug"
place=()
length=()
extra=()
pairs=1

for arg in "$@"; do
    case "$arg" in
        --build=*) build="$root/${arg#*=}" ;;
        --pairs=*) pairs="${arg#*=}" ;;
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
worst=0
kept=""

for pair in $(seq 1 "$pairs"); do
    # **Run from the build directory**, because `--resources` defaults to `./resources`.
    for run in 1 2; do
        (cd "$build" && "$tool" bench "${place[@]}" "${length[@]}" --window=false --upscale=off --filter=false \
            --hashes="$out/$pair-$run.csv" "${extra[@]}") > "$out/$pair-$run.log" 2>&1 || {
            echo "the run itself failed, see $out/$pair-$run.log:" >&2
            tail -20 "$out/$pair-$run.log" >&2
            exit 1
        }
    done

    # **Counted before the columns are compared**, because the comparison below walks the two files
    # by line number: a shorter second run would leave the first run's tail unread and report
    # agreement over frames nobody looked at. The header row is not a frame.
    frames="$(( $(wc -l < "$out/$pair-1.csv") - 1 ))"
    second="$(( $(wc -l < "$out/$pair-2.csv") - 1 ))"

    if [ "$frames" -ne "$second" ]; then
        echo "NOT repeatable: $frames frames against $second" >&2
        echo "the runs are in $out" >&2
        exit 1
    fi

    read -r pictures moved < <(awk -F, '
        FNR == 1 { if (NR == 1) { columns = NF; for (c = 1; c <= NF; ++c) name[c] = $c } next }
        NR == FNR { for (c = 3; c <= columns; ++c) was[FNR "," c] = $c; next }
        { for (c = 3; c <= columns; ++c) if ($c != was[FNR "," c]) differing[c]++ }
        END {
            printf "%d ", differing[3] + 0
            for (c = 4; c <= columns; ++c)
                if (differing[c] > 0) printf "%s=%d ", name[c], differing[c]
            printf "\n"
        }' "$out/$pair-1.csv" "$out/$pair-2.csv")

    if [ "$pictures" -gt "$worst" ]; then
        worst="$pictures"
    fi

    if [ "$pictures" -eq 0 ] && [ -z "$moved" ]; then
        echo "pair $pair of $pairs: $frames frames, identical"
        continue
    fi

    kept="$out"
    if [ "$pictures" -eq 0 ]; then
        echo "pair $pair of $pairs: $frames frames, every picture the same; columns moved: $moved" >&2
    else
        echo "pair $pair of $pairs: $pictures of $frames pictures differ${moved:+; columns moved: $moved}" >&2
    fi
done

if [ -z "$kept" ]; then
    echo "repeatable: $pairs pair(s), identical over every one"
    rm -rf "$out"
    exit 0
fi

# **Kept where they are**, because the files are what somebody now has to read.
echo "the runs are in $out" >&2

if [ "$worst" -eq 0 ]; then
    echo "the pictures repeat: every pair drew the same frames"
    exit 0
fi

echo "NOT repeatable: the worst pair differs on $worst pictures" >&2
exit 1
