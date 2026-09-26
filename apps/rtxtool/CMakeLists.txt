# The harness: what drives a real game to a place and measures, writes or checks what the
# renderer drew there. A library the tests reach into, and one executable that links the game.

set(RTXTOOL_FILES
    compare.cpp
    compare.hpp
    film.cpp
    film.hpp
    instruments/cardwatch.cpp
    instruments/cardwatch.hpp
    instruments/contactsheet.cpp
    instruments/contactsheet.hpp
    instruments/drivercache.cpp
    instruments/drivercache.hpp
    instruments/framehashes.cpp
    instruments/framehashes.hpp
    instruments/frametimes.cpp
    instruments/frametimes.hpp
    instruments/gpuclock.cpp
    instruments/gpuclock.hpp
    instruments/nvml.cpp
    instruments/nvml.hpp
    instruments/scenedigest.cpp
    instruments/scenedigest.hpp
    model/benchrecord.cpp
    model/benchrecord.hpp
    model/benchrun.cpp
    model/benchrun.hpp
    model/benchspec.cpp
    model/benchspec.hpp
    model/blockfile.cpp
    model/blockfile.hpp
    model/cameratrack.cpp
    model/cameratrack.hpp
    model/runrecord.cpp
    model/runrecord.hpp
    options.cpp
    options.hpp
    run.cpp
    run.hpp
    verbs.cpp
    verbs.hpp
)

# What drives the game once one is hosted: built into the executable alone, for the reason below.
set(RTXTOOL_HOSTED_FILES
    cameradriver.cpp
    cameradriver.hpp
    hosted.cpp
    measurer.cpp
    measurer.hpp
    session.cpp
    session.hpp
    stager.cpp
    stager.hpp
    standingnote.cpp
    standingnote.hpp
    stopwriter.cpp
    stopwriter.hpp
)

source_group(apps\\rtxtool FILES ${RTXTOOL_HOSTED_FILES} main.cpp)

# A library so the tests can reach into it, the way apps/openmw builds openmw-lib.
openmw_rtx_library(openmw-rtxtool-lib ${RTXTOOL_FILES})
openmw_rtx_expect_listed("${CMAKE_CURRENT_SOURCE_DIR}" MATCHING *.cpp *.hpp
    LISTED ${RTXTOOL_FILES} ${RTXTOOL_HOSTED_FILES} main.cpp)

# `PUBLIC`, because the interface names all of them: `options.hpp` takes Boost's option maps,
# `run.hpp` an `SDLUtil` mode, and the fork's own types throughout.
target_link_libraries(openmw-rtxtool-lib PUBLIC
    Boost::program_options
    SDL2::SDL2
    openmw-rtx
    openmw-rtx-vulkan
    components
)

# Boost's `typed_value::notify` assigns through a notifier pointer that is null unless one was
# asked for, and GCC 16 reports the assignment as a potential null dereference — inside
# `<vector>`, from an instantiation this file triggers, so a system-header exemption does not
# reach it. Boost's code, on the same footing as `extern/`; the flag comes off for this one file
# rather than for the target.
if (CMAKE_CXX_COMPILER_ID STREQUAL GNU)
    set_source_files_properties(options.cpp PROPERTIES COMPILE_OPTIONS -Wno-null-dereference)
endif()

# Named viewpoints, and which of them a profiling run visits in what order. Copied here because
# this is the only thing that reads them, and beside the backend's shaders under resources/rtx so
# one --resources finds both.
copy_all_resource_files("${OpenMW_SOURCE_DIR}/files/rtx" "${OPENMW_RESOURCES_ROOT}" "resources/rtx" "views.cfg;benches.cfg")

# The keys a watched run answers to, as a data directory of the harness's own: `hosted.cpp` adds it
# and names the content file wherever there is a window, and the played game names neither.
copy_all_resource_files("${OpenMW_SOURCE_DIR}/files/rtx/vfs" "${OPENMW_RESOURCES_ROOT}" "resources/rtx/vfs"
                        "rtxtool.omwscripts;scripts/rtx/keys.lua;scripts/rtx/sky.lua")

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

    # The stager and the camera read the clock through `mwworld/datetimemanager.hpp`, which is not
    # warning-free under the extra checks, so those two take the errors alone.
    openmw_rtx_sources(main.cpp ${RTXTOOL_HOSTED_FILES} ERRORS_ONLY stager.cpp cameradriver.cpp)
endif()

