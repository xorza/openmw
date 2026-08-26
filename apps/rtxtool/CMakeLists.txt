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
    find.cpp
    find.hpp
    framing.cpp
    framing.hpp
    placement.cpp
    placement.hpp
    scene.cpp
    scene.hpp
    shot.cpp
    shot.hpp
    stagedworld.cpp
    stagedworld.hpp
    terrainstorage.cpp
    terrainstorage.hpp
    textures.cpp
    textures.hpp
    lighting.cpp
    lighting.hpp
    motion.hpp
    npc.cpp
    npc.hpp
    objectstorage.cpp
    objectstorage.hpp
    posedactors.cpp
    posedactors.hpp
    options.cpp
    options.hpp
    perfcontrol.cpp
    perfcontrol.hpp
    picture.cpp
    picture.hpp
    view.cpp
    view.hpp
    viewpoint.cpp
    viewpoint.hpp
    validationchoice.cpp
    validationchoice.hpp
    verify.cpp
    verify.hpp
    views.cpp
    views.hpp
    waterplane.cpp
    waterplane.hpp
    window.cpp
    window.hpp
    world.cpp
    world.hpp
)

source_group(apps\\rtxtool FILES ${RTXTOOL_FILES} main.cpp)

# A library so the tests can reach into it, the way apps/openmw builds openmw-lib.
add_library(openmw-rtxtool-lib STATIC ${RTXTOOL_FILES})

target_link_libraries(openmw-rtxtool-lib
    Boost::program_options
    SDL2::SDL2
    openmw-rtx
    openmw-rtx-backends
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

openmw_add_executable(openmw-rtxtool main.cpp)

target_link_libraries(openmw-rtxtool openmw-rtxtool-lib)

# The layers are on unless this is a Release build, the same condition the debug object names use.
# A frame that breaks a rule should say so the first time someone sees it rather than the first time
# someone thinks to ask: every one of this fork's own Vulkan bugs so far has been one the layers name
# outright once they are asked to look.
#
# On the executable rather than the library, because the default is `main.cpp`'s to apply and only
# the command line downstream of it ever sees one.
target_compile_definitions(openmw-rtxtool PRIVATE
                           $<$<NOT:$<CONFIG:Release>>:OPENMW_RTX_VALIDATION_BY_DEFAULT>)

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtxtool-lib PRIVATE --coverage)
    target_link_libraries(openmw-rtxtool-lib gcov)
endif()

