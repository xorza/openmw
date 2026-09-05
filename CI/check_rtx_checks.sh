#!/bin/bash -ex
# Asks every claim this fork makes about what the renderer is handed and what it draws, of a real
# game, at every place of a suite.
#
# **These were tests against a world of the harness's own.** That world read its cells by hand,
# dressed its people by rules of its own and derived its sky from the content files, so a claim
# proved there was a claim about a world nobody plays. `openmw-rtxtool check` asks the same claims
# of the world a player stands in.
#
# **It needs Morrowind installed and a device that can trace**, which no continuous-integration
# runner here has — so this is a gate somebody runs on a machine with both, not one a push runs.
# Where either is missing the harness says so and this exits non-zero, which is right: a check that
# could not be asked is not a check that passed.
#
#   CI/check_rtx_checks.sh                       the default suite, in build-debug
#   CI/check_rtx_checks.sh --suite=interiors
#   BUILD=build-release CI/check_rtx_checks.sh   the build a number would be quoted from

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD:-build-debug}"

cmake --build "$root/$build" -j"$(nproc)" --target openmw-rtxtool

# From the build directory, because --resources defaults to ./resources.
cd "$root/$build"

# **The layers off, because a check is about the picture and not about the driver.** They cost
# between a tenth and half the frame rate and change nothing a claim here reads.
exec ./openmw-rtxtool check --validation=false "$@"
