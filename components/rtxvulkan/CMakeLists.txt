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

# NVIDIA's NGX SDK, which is what DLSS is. Prebuilt, licensed by NVIDIA rather than by this tree,
# and 750 MB — so it is named rather than vendored, submoduled or fetched. `extern/` is for source
# this build compiles, and none of that is true of a static library and a set of signed feature
# libraries.
#
# **`DLSS_SDK_DIR` in the environment is the default, the way `VULKAN_SDK` is for
# `find_package(Vulkan)`.** One checkout serves every build directory here and the reference
# implementation beside it, which is why the shared name is the environment's rather than this
# project's. `-DOPENMW_DLSS_SDK=` still wins over both.
if (DEFINED ENV{DLSS_SDK_DIR})
    set(DLSS_SDK_DEFAULT "$ENV{DLSS_SDK_DIR}")
else()
    set(DLSS_SDK_DEFAULT "${OpenMW_SOURCE_DIR}/.refs/dlss")
endif()

set(OPENMW_DLSS_SDK "${DLSS_SDK_DEFAULT}" CACHE PATH
    "NVIDIA's NGX SDK, for OPENMW_RTX_DLSS. Defaults to $DLSS_SDK_DIR, then to .refs/dlss")

if (OPENMW_RTX_DLSS)
    set(DLSS_LIB "${OPENMW_DLSS_SDK}/lib/Linux_x86_64")

    # **A hard error and not a quiet disable.** Compiling the feature out would put "is DLSS really
    # here" in a definition only this directory can see, which every caller would then have to guess
    # at. Asking for it and not having it is one message instead.
    #
    # **The tag is named and not left to the default branch.** What is linked here is a prebuilt
    # library and what ships beside it is signed, so "whatever was newest that day" is not a version
    # anything can be compared against.
    if (NOT EXISTS "${DLSS_LIB}/libnvsdk_ngx.a")
        message(FATAL_ERROR
            "OPENMW_RTX_DLSS is on by default and needs NVIDIA's NGX SDK, and there is none at "
            "${DLSS_LIB}.\n"
            "Clone it with: "
            "git clone --depth 1 --branch v310.7.0 https://github.com/NVIDIA/DLSS.git .refs/dlss\n"
            "or set DLSS_SDK_DIR in the environment, or point -DOPENMW_DLSS_SDK= at a copy.")
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
    shaders/fogdepth.comp
    shaders/fogintegrate.comp
    shaders/fogscatter.comp
    shaders/gui.frag
    shaders/gui.vert
    shaders/histogram.comp
    shaders/micromap.comp
    shaders/morph.comp
    shaders/skin.comp
    shaders/spriterects.comp
    shaders/spriteruns.comp
    shaders/spritestarts.comp
    shaders/tone.comp
    shaders/visibility.rahit
    shaders/visibility.rgen
    shaders/visibility.rmiss
    shaders/visibilitysurface.rchit
    shaders/visibilityterrain.rchit
    shaders/visibilitywater.rchit
    shaders/wavecompose.comp
    shaders/waveform.comp
    shaders/waveline.comp
)

# The device-behaviour probes, compiled only where something is going to run them. Nothing the
# renderer draws reaches these: they state an assumption about the hardware so that it can fail as a
# test rather than as a rendering mystery.
if (BUILD_COMPONENTS_TESTS)
    list(APPEND RTX_SHADERS shaders/probe.comp shaders/traceprobe.rgen shaders/traceprobe.rmiss)
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

# What the trace's five shaders are made of. Included rather than compiled — each has its own entry
# point and these have none, and glslc's depfile is what makes editing one rebuild every shader that
# reads it. Named here so an IDE lists them beside the shaders they belong to, which is the only
# thing the build wants them for.
set(RTX_SHADER_LIB
    shaders/lib/bindings.glsl
    shaders/lib/bloom.glsl
    shaders/lib/fog.glsl
    shaders/lib/footprint.glsl
    shaders/lib/frame.glsl
    shaders/lib/geometry.glsl
    shaders/lib/hitstage.glsl
    shaders/lib/lights.glsl
    shaders/lib/payload.glsl
    shaders/lib/random.glsl
    shaders/lib/reorder.glsl
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
    framering.cpp
    framering.hpp
    frameslots.hpp
    gbuffer.cpp
    gbuffer.hpp
    gputimer.cpp
    gputimer.hpp
    graphicspipeline.cpp
    graphicspipeline.hpp
    graveyard.cpp
    graveyard.hpp
    guipass.cpp
    guipass.hpp
    guitextures.cpp
    guitextures.hpp
    image.cpp
    image.hpp
    instance.cpp
    instance.hpp
    memory.cpp
    memory.hpp
    micromappass.cpp
    micromappass.hpp
    owned.hpp
    physicaldevice.cpp
    physicaldevice.hpp
    placing.hpp
    pipelinecache.cpp
    pipelinecache.hpp
    pipelinelayout.cpp
    pipelinelayout.hpp
    presenter.cpp
    presenter.hpp
    requirements.cpp
    requirements.hpp
    result.cpp
    result.hpp
    sceneacceleration.cpp
    sceneacceleration.hpp
    scenebuffers.cpp
    scenebuffers.hpp
    scenemicromaps.cpp
    scenemicromaps.hpp
    setlayout.cpp
    setlayout.hpp
    shadermodule.cpp
    shadermodule.hpp
    skinpass.cpp
    skinpass.hpp
    spritebinpass.cpp
    spritebinpass.hpp
    skintables.cpp
    skintables.hpp
    slottable.hpp
    specialization.hpp
    structurestorage.cpp
    structurestorage.hpp
    swapchain.cpp
    swapchain.hpp
    texture.cpp
    texture.hpp
    tonepass.cpp
    tonepass.hpp
    tracepipeline.cpp
    tracepipeline.hpp
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
