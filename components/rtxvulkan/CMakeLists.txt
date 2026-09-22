# The Vulkan backend.
#
# Everything here is Vulkan's: the device, its allocations, its acceleration structures and its
# passes. What is true of the content rather than of the API lives in `openmw-rtx`, which this links
# and which links no graphics API at all.

# Ninja needs the NEW behaviour to consume glslc's depfile; upstream's policy version predates it.
cmake_policy(SET CMP0116 NEW)

# NVIDIA's NGX SDK, which is what DLSS is. Prebuilt, licensed by NVIDIA rather than by this tree,
# and 750 MB — so it is found rather than vendored, submoduled or fetched: `cmake/FindNGX.cmake`,
# pointed at a checkout by `NGX_ROOT` the way every package is. `extern/` is for source this build
# compiles, and none of that is true of a static library and a set of signed feature libraries.
#
# **A hard error and not a quiet disable.** Compiling the feature out would put "is DLSS really
# here" in a definition only this directory can see, which every caller would then have to guess
# at. Asking for it and not having it is one message instead.
#
# **The version is exact and not a floor.** What is linked here is a prebuilt library and what
# ships beside it is signed, so "whatever was newest that day" is not a version anything can be
# compared against; the feature libraries carry the tag in their file names, which is what the
# module reads.
if (OPENMW_RTX_DLSS)
    find_package(NGX 310.7.0 EXACT REQUIRED)
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
    shaders/digest.comp
    shaders/exposure.comp
    shaders/fogdepth.rgen
    shaders/fogintegrate.comp
    shaders/fogscatter.rgen
    shaders/groundcomposite.comp
    shaders/gui.frag
    shaders/gui.vert
    shaders/histogram.comp
    shaders/line.frag
    shaders/line.vert
    shaders/mipchain.comp
    shaders/morph.comp
    shaders/ripplecompose.comp
    shaders/ripplestep.comp
    shaders/shadingmap.comp
    shaders/shadingsum.comp
    shaders/skin.comp
    shaders/spritecomposite.rgen
    shaders/spritelight.comp
    shaders/spriterects.comp
    shaders/spriteruns.comp
    shaders/spriteshade.comp
    shaders/spriteshelter.rgen
    shaders/spritestarts.comp
    shaders/stress.comp
    shaders/sunglare.comp
    shaders/tone.comp
    shaders/visibility.rahit
    shaders/visibility.rgen
    shaders/visibility.rmiss
    shaders/visibilityhit.rchit
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
    shaders/lib/basis.glsl
    shaders/lib/bindings.glsl
    shaders/lib/bloom.glsl
    shaders/lib/fog.glsl
    shaders/lib/footprint.glsl
    shaders/lib/frame.glsl
    shaders/lib/froxel.glsl
    shaders/lib/geometry.glsl
    shaders/lib/ground.glsl
    shaders/lib/hitrecord.glsl
    shaders/lib/lights.glsl
    shaders/lib/medium.glsl
    shaders/lib/payload.glsl
    shaders/lib/pixels.glsl
    shaders/lib/random.glsl
    shaders/lib/records.glsl
    shaders/lib/reproject.glsl
    shaders/lib/runs.glsl
    shaders/lib/sea.glsl
    shaders/lib/shading.glsl
    shaders/lib/sky.glsl
    shaders/lib/spritelist.glsl
    shaders/lib/sprites.glsl
    shaders/lib/starfield.glsl
    shaders/lib/texturearray.glsl
    shaders/lib/texturing.glsl
    shaders/lib/traversal.glsl
    shaders/lib/underwater.glsl
    shaders/lib/variants.glsl
    shaders/lib/water.glsl
)

add_custom_target(openmw-rtx-vulkan-shaders ALL DEPENDS ${RTX_SHADER_OUTPUTS}
                  SOURCES ${RTX_SHADERS} ${RTX_SHADER_LIB})

set(RTX_VULKAN_FILES
    accelerationstructure.cpp
    accelerationstructure.hpp
    accumulatehistory.cpp
    accumulatehistory.hpp
    accumulatepass.cpp
    accumulatepass.hpp
    atrouspass.cpp
    atrouspass.hpp
    barriers.cpp
    barriers.hpp
    blockedbuffer.cpp
    blockedbuffer.hpp
    blocklist.hpp
    bloompass.cpp
    bloompass.hpp
    bottomlevelstore.cpp
    bottomlevelstore.hpp
    buffer.cpp
    buffer.hpp
    bufferusage.hpp
    commands.cpp
    commands.hpp
    compositepass.cpp
    compositepass.hpp
    computepipeline.cpp
    computepipeline.hpp
    createrenderer.cpp
    createrenderer.hpp
    descriptorsets.cpp
    descriptorsets.hpp
    device.cpp
    device.hpp
    devicescene.cpp
    devicescene.hpp
    digestpass.cpp
    digestpass.hpp
    dispatch.hpp
    displaychain.cpp
    displaychain.hpp
    exposurepass.cpp
    exposurepass.hpp
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
    groundcompositepass.cpp
    groundcompositepass.hpp
    guipass.cpp
    guipass.hpp
    guitextures.cpp
    guitextures.hpp
    handles.cpp
    handles.hpp
    image.cpp
    image.hpp
    imageuse.hpp
    instance.cpp
    instance.hpp
    latencypacer.cpp
    latencypacer.hpp
    linepass.cpp
    linepass.hpp
    mipchainpass.cpp
    mipchainpass.hpp
    memory.cpp
    memory.hpp
    owned.hpp
    pacedmodes.cpp
    pacedmodes.hpp
    physicaldevice.cpp
    physicaldevice.hpp
    pipeline.hpp
    pipelinecache.cpp
    pipelinecache.hpp
    placing.hpp
    presenter.cpp
    presenter.hpp
    presenttargets.cpp
    presenttargets.hpp
    readstamp.hpp
    requirements.cpp
    requirements.hpp
    result.cpp
    result.hpp
    retiring.hpp
    ripplepass.cpp
    ripplepass.hpp
    sceneacceleration.cpp
    sceneacceleration.hpp
    scenebuffers.cpp
    scenebuffers.hpp
    shadingpass.cpp
    shadingpass.hpp
    skinpass.cpp
    skinpass.hpp
    skintables.cpp
    skintables.hpp
    slottable.hpp
    spritebin.cpp
    spritebin.hpp
    spritelightpass.cpp
    spritelightpass.hpp
    spritepasses.cpp
    spritepasses.hpp
    stresspass.cpp
    stresspass.hpp
    structurebuild.cpp
    structurebuild.hpp
    structurestorage.cpp
    structurestorage.hpp
    sunglarepass.cpp
    sunglarepass.hpp
    swapchain.cpp
    swapchain.hpp
    texture.cpp
    texture.hpp
    timeline.cpp
    timeline.hpp
    tonepass.cpp
    tonepass.hpp
    tracechain.cpp
    tracechain.hpp
    tracepipeline.cpp
    tracepipeline.hpp
    tracerecording.hpp
    upscaler.hpp
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
# DLSS leaves them out rather than hollowing them with a guard. `upscaler.hpp` is the one seam the
# renderer reaches them through, and the build picks which of its two definitions links.
if (OPENMW_RTX_DLSS)
    list(APPEND RTX_VULKAN_FILES dlss.cpp dlss.hpp dlsspass.cpp dlsspass.hpp dlssupscaler.cpp dlssupscaler.hpp ngx.hpp)
else()
    list(APPEND RTX_VULKAN_FILES noupscaler.cpp)
endif()

openmw_rtx_library(openmw-rtx-vulkan ${RTX_VULKAN_FILES})

# Vulkan is private: a host reaches the backend through `createrenderer.hpp`, which names no
# Vulkan type, so nothing a host compiles needs the headers.
# The tests reach into the backend and link Vulkan themselves. Linux hides a breach of this, its
# SDK headers sitting under `/usr/include`, and Windows is where one fails.
target_link_libraries(openmw-rtx-vulkan PUBLIC openmw-rtx PRIVATE Vulkan::Vulkan SDL2::SDL2 components)

add_dependencies(openmw-rtx-vulkan openmw-rtx-vulkan-shaders)

# Vulkan Memory Allocator, the sub-allocator behind `memory.hpp`: one header, reached by
# `memory.cpp` and nothing else. The system's where a package is installed, which the declaration
# asks `find_package` for first, and the pinned release fetched into the build tree otherwise, the
# way `extern/` takes Bullet and MyGUI when the system has none. `SYSTEM`, because the library's
# warnings are not this tree's to fix; a package found is an imported target and system already.
include(FetchContent)
FetchContent_Declare(VulkanMemoryAllocator
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA512=563acbcd8912d10d92c23715eba7acf0e7c1683af36021f415b36f359c2cce065f3906e395c32282a8410ec5c8179fbcb6412935c6629a49357475d4b4410e2a
    SOURCE_DIR fetched/vma
    SYSTEM
    FIND_PACKAGE_ARGS CONFIG QUIET
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
target_link_libraries(openmw-rtx-vulkan PRIVATE GPUOpen::VulkanMemoryAllocator)

# Object names cost one call per object created, never per frame, and an unreadable capture is a
# debugging session that does not happen.
target_compile_definitions(openmw-rtx-vulkan PUBLIC $<$<NOT:$<CONFIG:Release>>:OPENMW_RTX_DEBUG_NAMES>)

if (OPENMW_RTX_DLSS)
    # The static half is the target; the shared half is the feature library it loads at runtime,
    # from the directory of the running executable (`dlss.cpp` says why that one), so a copy sits
    # beside every binary of the build and goes with the binaries an install ships. The SDK's own
    # name is kept, version and all: it is what NGX asks the directory for.
    target_link_libraries(openmw-rtx-vulkan PRIVATE NGX::NGX)
    target_compile_definitions(openmw-rtx-vulkan PUBLIC OPENMW_RTX_DLSS)
    get_filename_component(_ngx_feature_name "${NGX_FEATURE_LIBRARY}" NAME)
    copy_resource_file("${NGX_FEATURE_LIBRARY}" "${OpenMW_BINARY_DIR}" "${_ngx_feature_name}")
    install(FILES "${NGX_FEATURE_LIBRARY}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    unset(_ngx_feature_name)
endif()
