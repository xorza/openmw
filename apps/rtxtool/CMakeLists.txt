set(RTXTOOL_FILES
    run.cpp
    run.hpp
    options.cpp
    options.hpp
    verbs.cpp
    verbs.hpp
    verify.cpp
    verify.hpp
)

# What drives the game once one is hosted: built into the executable alone, for the reason below.
set(RTXTOOL_HOSTED_FILES
    hosted.cpp
    session.cpp
    session.hpp
    stopwriter.cpp
    stopwriter.hpp
)

# The run reads the clock through `mwworld/datetimemanager.hpp`, which is not warning-free under
# `OPENMW_RTX_COMPILE_OPTIONS`, so this one is compiled with upstream's flags.
set(RTXTOOL_HOSTED_FILES_UPSTREAM
    session.cpp
)

source_group(apps\\rtxtool FILES ${RTXTOOL_FILES} ${RTXTOOL_HOSTED_FILES} main.cpp)

# A library so the tests can reach into it, the way apps/openmw builds openmw-lib.
add_library(openmw-rtxtool-lib STATIC ${RTXTOOL_FILES})

target_link_libraries(openmw-rtxtool-lib
    Boost::program_options
    SDL2::SDL2
    openmw-rtx
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

# The keys a watched run answers to, as a data directory of the harness's own: `hosted.cpp` adds it
# and names the content file wherever there is a window, and the played game names neither.
foreach(script rtxtool.omwscripts scripts/rtx/keys.lua scripts/rtx/sky.lua)
    copy_resource_file("${OpenMW_SOURCE_DIR}/files/rtx/vfs/${script}" "${RTX_RESOURCES_ROOT}" "resources/rtx/vfs/${script}")
endforeach()

# **The harness links the game, because it drives one.** A run stands in the world the player
# stands in — its cells read by `MWWorld::Scene`, its people dressed by `NpcAnimation`, its sky
# reported by `MWWorld::WeatherManager` — so a picture taken here and a picture played are one
# picture.
#
# **Only the executable links it.** `openmw-rtxtool-lib` stays engine-free, which is what lets
# `components-tests` keep linking it without pulling a whole game into a unit test binary — so
# the hosted files, which read the world and feed the renderer, are built here rather than into
# the library beside everything else.
#
# `apps/openmw` is added after this directory, and a target named in `target_link_libraries` is
# resolved when the build files are generated rather than when the line is read — so the order is
# free and only whether the target exists at all matters.
if (BUILD_OPENMW)
    openmw_add_executable(openmw-rtxtool main.cpp ${RTXTOOL_HOSTED_FILES})

    target_link_libraries(openmw-rtxtool openmw-rtxtool-lib openmw-lib)

    set(RTXTOOL_STRICT_FILES main.cpp ${RTXTOOL_HOSTED_FILES})
    list(REMOVE_ITEM RTXTOOL_STRICT_FILES ${RTXTOOL_HOSTED_FILES_UPSTREAM})
    set_source_files_properties(${RTXTOOL_STRICT_FILES} PROPERTIES COMPILE_OPTIONS "${OPENMW_RTX_COMPILE_OPTIONS}")
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtxtool-lib PRIVATE --coverage)
    target_link_libraries(openmw-rtxtool-lib gcov)
endif()

