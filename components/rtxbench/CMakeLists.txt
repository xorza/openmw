# The instruments a measured run is taken with, shared by both hosts.
#
# **Here rather than in the harness, because the game measures itself.** A row of the game's report
# and a row of the harness's only mean something beside each other, so what a run's length is
# written as, what a place came to, how it is printed and how it is recorded are one statement —
# and the clock the card was at and the fifo perf listens on are the same instruments whichever
# binary is holding them.
#
# It knows nothing about a world, a cell or a view. What a run visits is its caller's.

set(RTXBENCH_FILES
    benchrecord.cpp
    benchrecord.hpp
    benchspec.cpp
    benchspec.hpp
    framehashes.cpp
    framehashes.hpp
    gpuclock.cpp
    gpuclock.hpp
    perfcontrol.cpp
    perfcontrol.hpp
)

source_group(components\\rtxbench FILES ${RTXBENCH_FILES})

add_library(openmw-rtx-bench STATIC ${RTXBENCH_FILES})

# `PUBLIC`, because `benchrecord.hpp` names `Rtx::FrameTimes`, `Rtx::SceneStats` and the three
# reconstruction enums: every consumer needs the core's include paths.
target_link_libraries(openmw-rtx-bench PUBLIC openmw-rtx)

target_compile_options(openmw-rtx-bench PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtx-bench PRIVATE --coverage)
    target_link_libraries(openmw-rtx-bench gcov)
endif()
