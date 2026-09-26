# The instruments a measured run is taken with, shared by both hosts: what a run's length is
# written as, the frame times and how they are summarised, the card's clock and who else held it,
# the fifo perf listens on, the driver's cache, and what a run looks at — a scene's digest, a
# frame's hashes, a sheet of the textures.
#
# **Here rather than in the harness, because the game measures itself.** A frame time the game
# reports and one the harness reports only mean something beside each other where one statement
# makes both, and the clock and the fifo are the same instruments whichever binary holds them.
#
# It knows nothing about a world, a cell or a view. What a run visits, what a place came to and how
# a run is recorded are the harness's (`apps/rtxtool/model`); what this looks at is a fact about the
# content rather than about whichever host staged it.

set(RTX_BENCH_FILES
    benchspec.cpp
    benchspec.hpp
    cardwatch.cpp
    cardwatch.hpp
    contactsheet.cpp
    contactsheet.hpp
    drivercache.cpp
    drivercache.hpp
    framehashes.cpp
    framehashes.hpp
    frametimes.cpp
    frametimes.hpp
    gpuclock.cpp
    gpuclock.hpp
    nvml.cpp
    nvml.hpp
    scenedigest.cpp
    scenedigest.hpp
)

openmw_rtx_library(openmw-rtx-bench ${RTX_BENCH_FILES})
openmw_rtx_expect_listed("${CMAKE_CURRENT_SOURCE_DIR}" MATCHING *.cpp *.hpp LISTED ${RTX_BENCH_FILES})

# `PUBLIC`, because `frametimes.hpp` names `Rtx::GpuSpan` and `scenedigest.hpp` the scene: every
# consumer needs the core's include paths.
target_link_libraries(openmw-rtx-bench PUBLIC openmw-rtx)
