# The Metal backend. Built only with -DOPENMW_RTX=ON and -DOPENMW_RTX_METAL=ON, and only on Apple.
#
# Everything Metal is Objective-C++ and stays inside the implementation: the one header here is
# plain C++ so the dispatcher can include it without the rest of the tree learning what an `id<>` is.

enable_language(OBJCXX)

find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

# Ninja needs the NEW behaviour to consume the depfile; the root project's minimum predates it.
if (POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
endif()

find_program(OPENMW_XCRUN NAMES xcrun REQUIRED
    DOC "xcrun, which is how the Metal toolchain is reached")

set(RTX_METAL_SHADERS
    shaders/visibility.metal
)

set(RTX_METAL_SHADER_OUTPUTS)
set(RTX_METALLIB_DIR "${RTX_RESOURCES_ROOT}/resources/rtx/shaders")

foreach (shader ${RTX_METAL_SHADERS})
    get_filename_component(name "${shader}" NAME_WE)
    set(air "${CMAKE_CURRENT_BINARY_DIR}/${name}.air")
    set(metallib "${RTX_METALLIB_DIR}/${name}.metallib")
    set(depfile "${CMAKE_CURRENT_BINARY_DIR}/${name}.d")

    # Two steps where the Vulkan side has one, because Metal separates compiling from archiving. The
    # depfile is what makes editing a shared header rebuild this as well as the SPIR-V.
    add_custom_command(
        OUTPUT "${metallib}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RTX_METALLIB_DIR}"
        COMMAND "${OPENMW_XCRUN}" -sdk macosx metal -std=metal4.0 -I "${RTX_SHADER_INCLUDE}"
                -MD -MF "${depfile}" -c "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" -o "${air}"
        COMMAND "${OPENMW_XCRUN}" -sdk macosx metallib "${air}" -o "${metallib}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
        DEPFILE "${depfile}"
        COMMENT "Compiling and archiving ${name}.metal"
        VERBATIM)

    list(APPEND RTX_METAL_SHADER_OUTPUTS "${metallib}")
endforeach()

add_custom_target(openmw-rtx-metal-shaders ALL DEPENDS ${RTX_METAL_SHADER_OUTPUTS} SOURCES ${RTX_METAL_SHADERS})

set(RTXMETAL_FILES
    metalrenderer.hpp
    metalrenderer.mm
)

source_group(components\\rtxmetal FILES ${RTXMETAL_FILES})

add_library(openmw-rtx-metal STATIC ${RTXMETAL_FILES})

target_link_libraries(openmw-rtx-metal
    PUBLIC openmw-rtx
    PRIVATE components ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})

# ARC, so the Objective-C objects a renderer holds are released with it and nothing here writes a
# retain or a release by hand.
target_compile_options(openmw-rtx-metal PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS} -fobjc-arc)

add_dependencies(openmw-rtx-metal openmw-rtx-metal-shaders)

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtx-metal PRIVATE --coverage)
    target_link_libraries(openmw-rtx-metal gcov)
endif()
