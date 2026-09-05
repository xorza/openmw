set(RTXTOOL_FILES
    actor.cpp
    actor.hpp
    bench.cpp
    bench.hpp
    benchsuite.cpp
    benchsuite.hpp
    cellchoice.cpp
    cellchoice.hpp
    cellscene.cpp
    cellscene.hpp
    contactsheet.cpp
    contactsheet.hpp
    content.cpp
    content.hpp
    exteriorindex.cpp
    exteriorindex.hpp
    find.cpp
    find.hpp
    framerequest.cpp
    framerequest.hpp
    framing.cpp
    framing.hpp
    lighting.cpp
    lighting.hpp
    motion.hpp
    npc.cpp
    npc.hpp
    objectstorage.cpp
    objectstorage.hpp
    options.cpp
    options.hpp
    parsefloat.cpp
    parsefloat.hpp
    picture.cpp
    picture.hpp
    placement.cpp
    placement.hpp
    posedactors.cpp
    posedactors.hpp
    scene.cpp
    scene.hpp
    scenedigest.cpp
    scenedigest.hpp
    stagedworld.cpp
    stagedworld.hpp
    terrainstorage.cpp
    terrainstorage.hpp
    textures.cpp
    textures.hpp
    validationchoice.cpp
    validationchoice.hpp
    verbs.cpp
    verbs.hpp
    verify.cpp
    verify.hpp
    view.cpp
    view.hpp
    viewpoint.cpp
    viewpoint.hpp
    views.cpp
    views.hpp
    waterplane.cpp
    waterplane.hpp
    window.cpp
    window.hpp
    world.cpp
    world.hpp
    worldclock.cpp
    worldclock.hpp
)

source_group(apps\\rtxtool FILES ${RTXTOOL_FILES} main.cpp)

# A library so the tests can reach into it, the way apps/openmw builds openmw-lib.
add_library(openmw-rtxtool-lib STATIC ${RTXTOOL_FILES})

target_link_libraries(openmw-rtxtool-lib
    Boost::program_options
    SDL2::SDL2
    openmw-rtx
    openmw-rtx-backends
    openmw-rtx-bench
    openmw-rtx-vulkan
    components
)

target_compile_options(openmw-rtxtool-lib PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})

# Boost's `typed_value::notify` assigns through a notifier pointer that is null unless one was
# asked for, and GCC 16 reports the assignment as a potential null dereference — inside
# `<vector>`, from an instantiation this file triggers, so a system-header exemption does not
# reach it. Boost's code, on the same footing as `extern/`; the flag comes off for this one file
# rather than for the target.
set_source_files_properties(options.cpp PROPERTIES COMPILE_OPTIONS -Wno-null-dereference)

# Named viewpoints. Copied here because this is the only thing that reads them, and beside the
# backend's shaders under resources/rtx so one --resources finds both.
copy_resource_file("${OpenMW_SOURCE_DIR}/files/rtx/views.cfg" "${RTX_RESOURCES_ROOT}" "resources/rtx/views.cfg")

# Which of those a profiling run visits, and in what order.
copy_resource_file("${OpenMW_SOURCE_DIR}/files/rtx/benches.cfg" "${RTX_RESOURCES_ROOT}" "resources/rtx/benches.cfg")

# **The harness links the game, because it drives one.** A run stands in the world the player
# stands in — its cells read by `MWWorld::Scene`, its people dressed by `NpcAnimation`, its sky
# reported by `MWWorld::WeatherManager` — so a picture taken here and a picture played are one
# picture.
#
# **Only the executable links it.** `openmw-rtxtool-lib` stays engine-free, which is what lets
# `components-tests` keep linking it without pulling a whole game into a unit test binary — so
# `hosted.cpp` is built here rather than into the library beside everything else.
#
# `apps/openmw` is added after this directory, and a target named in `target_link_libraries` is
# resolved when the build files are generated rather than when the line is read — so the order is
# free and only whether the target exists at all matters.
if (BUILD_OPENMW)
    openmw_add_executable(openmw-rtxtool main.cpp hosted.cpp)

    target_link_libraries(openmw-rtxtool openmw-rtxtool-lib openmw-lib)

    target_compile_options(openmw-rtxtool PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtxtool-lib PRIVATE --coverage)
    target_link_libraries(openmw-rtxtool-lib gcov)
endif()

