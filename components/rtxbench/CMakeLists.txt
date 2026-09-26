# The instruments a measured run is taken with, shared by both hosts.
#
# **Here rather than in the harness, because the game measures itself.** A row of the game's report
# and a row of the harness's only mean something beside each other, so what a run's length is
# written as, what a place came to, how it is printed and how it is recorded are one statement —
# and the clock the card was at and the fifo perf listens on are the same instruments whichever
# binary is holding them.
#
# It knows nothing about a world, a cell or a view. What a run visits is its caller's, and what it
# looks at — a scene's digest, a sheet of its textures — is a fact about the content rather than
# about whichever host staged it.

set(RTX_BENCH_FILES
    benchrecord.cpp
    benchrecord.hpp
    benchrun.cpp
    benchrun.hpp
    benchspec.cpp
    benchspec.hpp
    cameratrack.cpp
    cameratrack.hpp
    cardwatch.cpp
    cardwatch.hpp
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
    runrecord.cpp
    runrecord.hpp
    runsetup.hpp
    scenedigest.cpp
    scenedigest.hpp
)

openmw_rtx_library(openmw-rtx-bench ${RTX_BENCH_FILES})
openmw_rtx_expect_listed("${CMAKE_CURRENT_SOURCE_DIR}" MATCHING *.cpp *.hpp LISTED ${RTX_BENCH_FILES})

# `PUBLIC`, because `benchrecord.hpp` names `Rtx::SceneStats` and the three reconstruction enums,
# and `frametimes.hpp` names `Rtx::GpuSpan`: every consumer needs the core's include paths.
target_link_libraries(openmw-rtx-bench PUBLIC openmw-rtx)
