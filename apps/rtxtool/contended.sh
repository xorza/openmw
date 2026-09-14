#!/usr/bin/env bash
# Asserts that a walk repeats while another process shares the card.
#
#   contended.sh                        # repeatable.sh, beside a second harness looping the ship
#   contended.sh --pairs=3              # anything else goes to repeatable.sh
#   contended.sh --build=build-release  # both processes from another build
#
# **A second process, because that is what the fault needed.** A ray query inside a compute
# dispatch answered differently from run to run — a candidate counted twice or not at all — only
# while another process was on the card, and never inside a launch. `repeatable.sh` on a quiet card
# read 0 of 70 pairs on a tree that read 3 of 4 pairs beside a game, so a count taken alone says
# nothing about that class of fault. `.notes/bench.txt` has the runs.
#
# **The load is the harness itself**, looping a still on the ship: a second Vulkan context, a
# trace and a full upscale a frame, which is the shape of the game that found it. Every process
# here runs under `timeout`, so a hang ends the run rather than the machine.
#
# **How long it takes.** A pair under the load is about two and a half minutes; `--pairs=2` fits in
# five.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-debug"

for arg in "$@"; do
    case "$arg" in
        --build=*) build="$root/${arg#*=}" ;;
    esac
done

tool="$build/openmw-rtxtool"
if [ ! -x "$tool" ]; then
    echo "no $tool: build it first" >&2
    exit 1
fi

# **Run from the build directory**, because `--resources` defaults to `./resources`. One bench at a
# time, each killed after a hundred seconds, the loop after an hour whatever the gate below did.
# In a process group of its own, so that the exit below reaches the bench under the loop and not
# only the loop — `timeout` moves itself into a group of its own unless told `--foreground`, and
# the inner one is told, so the bench stays in the outer one's.
setsid timeout -k 5 3600 bash -c 'cd "$1"; while true; do
    timeout --foreground -k 5 100 ./openmw-rtxtool bench --views=seyda-neen-ship --seconds=30 \
        --window=false --validation=false > /dev/null 2>&1 || true
done' _ "$build" &
load=$!

# And waited for, since a bench takes a moment to give the device back: the next thing on the card
# should not start beside a process still leaving it.
stopLoad() {
    kill -- -"$load" 2>/dev/null || true
    wait "$load" 2>/dev/null || true
    for _ in $(seq 1 100); do
        kill -0 -- -"$load" 2>/dev/null || break
        sleep 0.1
    done
}
trap stopLoad EXIT

"$root/apps/rtxtool/repeatable.sh" "$@"
