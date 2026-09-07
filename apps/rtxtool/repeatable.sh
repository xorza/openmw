#!/usr/bin/env bash
# Asserts that a walk is handed the same scene twice.
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
# **One pair gates and several read.** What is gated repeats exactly, so a pair that finds nothing
# has found nothing. The picture is the other way about — it moves on about three pairs in ten — so
# a reading of it from one pair is a coin flip, and `--pairs` is what buys enough of them.
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
# temporal and carries state nothing below it can hold still. What is left is what a reconstruction
# is fed, which is where every defect this has caught showed itself.
#
# **And Ray Reconstruction adds nothing of its own, which is worth stating because it looks as
# though it does.** Its history is recurrent, so one frame the trace drew differently reaches every
# frame after it and a run comes back disagreeing almost everywhere. It is faithful, not faulty, and
# what it is fed is what this turns the upscaler off to look at.
#
# **A table, and the scene columns decide the exit status.** A hashes file is a CSV with a header
# row and a row a frame: the picture, then a column for every part of the scene it was drawn from.
# What a run is *handed* is what this asserts, because that is what repeats exactly and what a
# regression has to keep.
#
# **The picture is reported and does not fail the run**, which is the other way round from how this
# started. The renderer carries a residual nobody hunts — `AGENTS.md` says why — of about three
# pairs in ten, always one part in 255, which no eight-bit hash can even see. A gate red for that is
# a gate nobody reads, and it was silent about the eighteen columns that are now exact.
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
columns=""

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

    if [ -n "$moved" ] && [ -z "$columns" ]; then
        columns="pair $pair: $moved"
    fi

    if [ "$pictures" -eq 0 ] && [ -z "$moved" ]; then
        echo "pair $pair of $pairs: $frames frames, identical"
        continue
    fi

    kept="$out"
    if [ -n "$moved" ]; then
        echo "pair $pair of $pairs: columns moved: $moved" >&2
    fi

    if [ "$pictures" -gt 0 ]; then
        echo "pair $pair of $pairs: $pictures of $frames pictures differ" >&2
    fi
done

if [ -z "$kept" ]; then
    echo "repeatable: $pairs pair(s), identical over every one"
    rm -rf "$out"
    exit 0
fi

# **Kept where they are**, because the files are what somebody now has to read.
echo "the runs are in $out" >&2

if [ -n "$columns" ]; then
    echo "NOT repeatable: the walk was handed two scenes — $columns" >&2
    exit 1
fi

echo "the scene repeats: every pair was handed one world; $worst pictures differ at worst"
exit 0
