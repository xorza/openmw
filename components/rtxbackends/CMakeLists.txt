# The one place that knows which backends this build has.
#
# It exists so nothing else needs an `#ifdef` to ask: the core declares `createRenderer` and cannot
# link a backend without a cycle, and every consumer — the harness, the tests, the game — wants a
# renderer rather than a choice.

set(RTXBACKENDS_FILES
    renderer.cpp
)

source_group(components\\rtxbackends FILES ${RTXBACKENDS_FILES})

add_library(openmw-rtx-backends STATIC ${RTXBACKENDS_FILES})

target_link_libraries(openmw-rtx-backends PUBLIC openmw-rtx PRIVATE SDL2::SDL2)

target_compile_options(openmw-rtx-backends PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})

if (OPENMW_RTX_VULKAN)
    target_link_libraries(openmw-rtx-backends PUBLIC openmw-rtx-vulkan)
    target_compile_definitions(openmw-rtx-backends PRIVATE OPENMW_RTX_VULKAN)
endif()

if (OPENMW_RTX_METAL)
    target_link_libraries(openmw-rtx-backends PUBLIC openmw-rtx-metal)
    target_compile_definitions(openmw-rtx-backends PRIVATE OPENMW_RTX_METAL)
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtx-backends PRIVATE --coverage)
    target_link_libraries(openmw-rtx-backends gcov)
endif()
