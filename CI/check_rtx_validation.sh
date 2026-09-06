#!/bin/bash -ex
# Drives a real game under the validation layers, with synchronization validation on, over the paths
# where a missing barrier would go unnoticed.
#
# **The layers are the barrier checker this tree already has.** `instance.cpp` wires up
# synchronization validation and the shader-access heuristic it needs to see a compute dispatch at
# all; `check_rtx_checks.sh` then turns the layers off, because a claim about the picture is not a
# claim about the driver. So nothing was running them over a frame. This is that gate.
#
# **Each verb reaches something the others do not.** `check` walks cells and traces frames, `shot`
# adds the readback a picture is written from, `map` and `doll` are offscreen traces with scenes of
# their own, and `bench` is the only one with a moving camera and so with cells arriving under load.
#
# The policy is to abort on the first error, so any message ends the run and this script with it. A
# run that asked for the layers and cannot have them now fails by name rather than reporting nothing.
#
# **It needs Morrowind installed and a device that can trace**, like `check_rtx_checks.sh`, so this
# is a gate somebody runs rather than one a push runs. It is slower than an unvalidated run by
# between a tenth and half the frame rate; that is the price of the answer.
#
# Arguments go to `check`, which is the verb that walks a list of places.
#
#   CI/check_rtx_validation.sh                    every verb, in build-debug
#   CI/check_rtx_validation.sh --views=vivec      a list of places of your own
#   BUILD=build-debug-asan CI/check_rtx_validation.sh

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD:-build-debug}"

cmake --build "$root/$build" -j"$(nproc)" --target openmw-rtxtool

# From the build directory, because --resources defaults to ./resources.
cd "$root/$build"

# GPU-assisted validation is off throughout: it instruments every shader, costs about half the frame
# rate, and catches a different class — what a shader does with its own arguments rather than what
# the queue does between two passes. `--gpu-validation` on one verb is how that is asked for.
layers=(--sync-validation --gpu-validation=false)

written="${TMPDIR:-/tmp}/openmw-rtx-validation"
mkdir -p "$written"

./openmw-rtxtool check "${layers[@]}" "$@"

# One frame written out, which is the trace, the upscaler, the curve and the read back off the
# device that a picture leaves through.
./openmw-rtxtool shot "${layers[@]}" --out="$written/shot.png"

# A tile traced straight down and read back, and a person traced against a scene built for them:
# both are offscreen paths with descriptor sets and targets the frame's own passes never touch.
./openmw-rtxtool map "${layers[@]}" --out="$written/map.png"
./openmw-rtxtool doll "${layers[@]}" --npc=fargoth --out="$written/doll.png"

# The moving camera, so cells arrive while frames are still in flight — the one place a resource
# retired too early shows up. Its own place rather than the argument's: a route is what makes cells
# arrive, and a still view would exercise none of that.
./openmw-rtxtool bench "${layers[@]}" --window=false --views=island-crossing --seconds=10
