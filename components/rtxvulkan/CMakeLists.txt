# The Vulkan backend. Built only with -DOPENMW_RTX=ON.
#
# Everything here is Vulkan's: the device, its allocations, its acceleration structures and its
# passes. What is true of the content rather than of the API lives in `openmw-rtx`, which this links
# and which links no graphics API at all.

# Ninja needs the NEW behaviour to consume glslc's depfile; the root project's minimum predates it.
if (POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
endif()

find_package(Vulkan REQUIRED)

# NVIDIA's NGX SDK, which is what DLSS is. Public, and 750 MB, so it is pointed at rather than
# vendored: clone it once with
#
#   git clone --depth 1 https://github.com/NVIDIA/DLSS.git .refs/dlss
#
# or set OPENMW_DLSS_SDK to wherever it already is.
set(OPENMW_DLSS_SDK "${OpenMW_SOURCE_DIR}/.refs/dlss" CACHE PATH
    "NVIDIA's NGX SDK, for OPENMW_RTX_DLSS")

if (OPENMW_RTX_DLSS)
    set(DLSS_LIB "${OPENMW_DLSS_SDK}/lib/Linux_x86_64")

    # **A hard error and not a quiet disable.** Compiling the feature out would put "is DLSS really
    # here" in a definition only this directory can see, which every caller would then have to guess
    # at. Asking for it and not having it is one message instead.
    if (NOT EXISTS "${DLSS_LIB}/libnvsdk_ngx.a")
        message(FATAL_ERROR
            "OPENMW_RTX_DLSS is on by default and needs NVIDIA's NGX SDK, and there is none at "
            "${DLSS_LIB}.\n"
            "Clone it with: git clone --depth 1 https://github.com/NVIDIA/DLSS.git .refs/dlss\n"
            "or point -DOPENMW_DLSS_SDK= at a copy.")
    endif()
endif()

find_program(OPENMW_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}" REQUIRED
    DOC "glslc, from shaderc — compiles the renderer's GLSL to SPIR-V")
find_program(OPENMW_SPIRV_VAL NAMES spirv-val REQUIRED
    DOC "spirv-val, from SPIRV-Tools — validates every module the build produces")

set(RTX_SHADERS
    shaders/accumulate.comp
    shaders/atrous.comp
    shaders/bloomdown.comp
    shaders/bloomup.comp
    shaders/composite.comp
    shaders/exposure.comp
    shaders/fogvolume.comp
    shaders/gui.frag
    shaders/gui.vert
    shaders/histogram.comp
    shaders/tone.comp
    shaders/visibility.comp
    shaders/wavecompose.comp
    shaders/waveform.comp
    shaders/waveline.comp
)

# The device-behaviour probes, compiled only where something is going to run them. Nothing the
# renderer draws reaches these: they state an assumption about the hardware so that it can fail as a
# test rather than as a rendering mystery.
if (BUILD_COMPONENTS_TESTS)
    list(APPEND RTX_SHADERS shaders/probe.comp)
endif()

# Compiling and validating in one custom command is deliberate: an invalid module must fail the
# build, never the frame. -MD gives ninja the include graph so editing a shared header rebuilds every
# shader that includes it.
set(RTX_SHADER_OUTPUTS)
set(RTX_SPIRV_DIR "${RTX_RESOURCES_ROOT}/resources/rtx/shaders")

# -g always: source correlation in Nsight is what makes a shader debuggable, and the debug info sits
# in the module rather than costing anything at runtime.
set(RTX_GLSLC_FLAGS -g $<IF:$<CONFIG:Debug>,-O0,-O>)

foreach (shader ${RTX_SHADERS})
    get_filename_component(name "${shader}" NAME)
    set(spv "${RTX_SPIRV_DIR}/${name}.spv")
    set(depfile "${CMAKE_CURRENT_BINARY_DIR}/${name}.d")

    add_custom_command(
        OUTPUT "${spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RTX_SPIRV_DIR}"
        COMMAND "${OPENMW_GLSLC}" ${RTX_GLSLC_FLAGS} --target-env=vulkan1.4 -I "${RTX_SHADER_INCLUDE}"
                -MD -MF "${depfile}" -o "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
        # --scalar-block-layout because the shared C++/GLSL headers rely on the feature the
        # device enables; without the flag the validator applies std430 straddling rules.
        COMMAND "${OPENMW_SPIRV_VAL}" --target-env vulkan1.4 --scalar-block-layout "${spv}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
        DEPFILE "${depfile}"
        COMMENT "Compiling and validating ${name}"
        VERBATIM)

    list(APPEND RTX_SHADER_OUTPUTS "${spv}")
endforeach()

# What `visibility.comp` is made of. Included rather than compiled — there is one entry point over
# all of them, and glslc's depfile is what makes editing one rebuild it. Named here so an IDE lists
# them beside the shader they belong to, which is the only thing the build wants them for.
set(RTX_SHADER_LIB
    shaders/lib/bindings.glsl
    shaders/lib/bloom.glsl
    shaders/lib/fog.glsl
    shaders/lib/footprint.glsl
    shaders/lib/geometry.glsl
    shaders/lib/lights.glsl
    shaders/lib/random.glsl
    shaders/lib/reproject.glsl
    shaders/lib/sea.glsl
    shaders/lib/shading.glsl
    shaders/lib/sky.glsl
    shaders/lib/sprites.glsl
    shaders/lib/texturing.glsl
    shaders/lib/traversal.glsl
    shaders/lib/underwater.glsl
    shaders/lib/variants.glsl
    shaders/lib/water.glsl
)

add_custom_target(openmw-rtx-vulkan-shaders ALL DEPENDS ${RTX_SHADER_OUTPUTS}
                  SOURCES ${RTX_SHADERS} ${RTX_SHADER_LIB})

set(RTX_VULKAN_FILES
    accumulatepass.cpp
    accumulatepass.hpp
    atrouspass.cpp
    atrouspass.hpp
    blockedbuffer.cpp
    blockedbuffer.hpp
    bloompass.cpp
    bloompass.hpp
    buffer.cpp
    buffer.hpp
    commands.cpp
    commands.hpp
    compositepass.cpp
    compositepass.hpp
    computepipeline.cpp
    computepipeline.hpp
    device.cpp
    device.hpp
    exposurepass.cpp
    exposurepass.hpp
    fogtile.cpp
    fogtile.hpp
    fogvolume.cpp
    fogvolume.hpp
    frameslots.hpp
    slottable.hpp
    gbuffer.cpp
    gbuffer.hpp
    graphicspipeline.cpp
    graphicspipeline.hpp
    guipass.cpp
    guipass.hpp
    guitextures.cpp
    guitextures.hpp
    gputimer.cpp
    gputimer.hpp
    graveyard.cpp
    graveyard.hpp
    hostbuffer.cpp
    hostbuffer.hpp
    image.cpp
    image.hpp
    instance.cpp
    instance.hpp
    memory.cpp
    memory.hpp
    physicaldevice.cpp
    physicaldevice.hpp
    presenter.cpp
    presenter.hpp
    pipelinecache.cpp
    pipelinecache.hpp
    requirements.cpp
    requirements.hpp
    result.cpp
    result.hpp
    sceneacceleration.cpp
    sceneacceleration.hpp
    scenebuffers.cpp
    setlayout.cpp
    setlayout.hpp
    scenebuffers.hpp
    shadermodule.cpp
    shadermodule.hpp
    structurestorage.cpp
    structurestorage.hpp
    swapchain.cpp
    swapchain.hpp
    texture.cpp
    texture.hpp
    tonepass.cpp
    tonepass.hpp
    validation.cpp
    validation.hpp
    visibilitypass.cpp
    visibilitypass.hpp
    vulkanrenderer.cpp
    vulkanrenderer.hpp
    wavepass.cpp
    wavepass.hpp
)

# Only where the SDK is there to compile against: these are nothing but NGX, so a build without
# DLSS leaves them out rather than hollowing them with a guard.
if (OPENMW_RTX_DLSS)
    list(APPEND RTX_VULKAN_FILES dlss.cpp dlss.hpp dlsspass.cpp dlsspass.hpp ngx.hpp)
endif()

source_group(components\\rtxvulkan FILES ${RTX_VULKAN_FILES})

add_library(openmw-rtx-vulkan STATIC ${RTX_VULKAN_FILES})

target_link_libraries(openmw-rtx-vulkan PUBLIC Vulkan::Vulkan openmw-rtx PRIVATE SDL2::SDL2 components)

target_compile_options(openmw-rtx-vulkan PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})

add_dependencies(openmw-rtx-vulkan openmw-rtx-vulkan-shaders)

# Object names cost one call per object created, never per frame, and an unreadable capture is a
# debugging session that does not happen.
target_compile_definitions(openmw-rtx-vulkan PUBLIC $<$<NOT:$<CONFIG:Release>>:OPENMW_RTX_DEBUG_NAMES>)

target_compile_definitions(openmw-rtx-vulkan PUBLIC OPENMW_RTX_SHADER_DIR="${RTX_SPIRV_DIR}")

if (OPENMW_RTX_DLSS)
    # System, because the SDK's headers are somebody else's and this fork's warnings are errors:
    # `NVSDK_NGX_Parameter` has virtual methods and no virtual destructor, and its helpers use zero
    # as a null pointer. Neither is ours to fix.
    target_include_directories(openmw-rtx-vulkan SYSTEM PRIVATE "${OPENMW_DLSS_SDK}/include")

    # The static half is the SDK's own; the shared halves are the feature libraries it loads at
    # runtime, and NGX looks for those in the application's folder unless it is told otherwise.
    target_link_libraries(openmw-rtx-vulkan PRIVATE "${DLSS_LIB}/libnvsdk_ngx.a" ${CMAKE_DL_LIBS})
    target_compile_definitions(openmw-rtx-vulkan PUBLIC OPENMW_RTX_DLSS
                               PRIVATE OPENMW_RTX_NGX_FEATURES="${DLSS_LIB}/rel")
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtx-vulkan PRIVATE --coverage)
    target_link_libraries(openmw-rtx-vulkan gcov)
endif()
