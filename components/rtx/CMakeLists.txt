# The ray tracing renderer's API-neutral core. Built only with -DOPENMW_RTX=ON.
#
# **This library links no graphics API, and that is the point.** What belongs here is what is true
# of Morrowind's content, of light transport, or of what the scene *is* — written once and read by
# every backend. The linker is what keeps it that way: nothing here can include a Vulkan header,
# because it is not on the include path.
#
# **OpenSceneGraph is not that boundary and never was.** The scene arrives as an `osg::Node` graph
# and the maths types are OSG's, so half this library reads one; what a backend gets handed is
# `SceneDesc` either way. The PNG writer is here for the same reason: it is the one piece of this
# fork that needs an image library, OSG is the one the engine already has, and both the harness and
# the game want to write a frame to a file.

set(RTX_FILES
    alphaimage.cpp
    alphaimage.hpp
    bluenoise.cpp
    bluenoise.hpp
    camera.cpp
    camera.hpp
    cloudshell.cpp
    cloudshell.hpp
    colourblock.cpp
    colourblock.hpp
    compositequeue.cpp
    compositequeue.hpp
    distantland.cpp
    distantland.hpp
    distantlights.cpp
    distantlights.hpp
    emitterresolver.cpp
    emitterresolver.hpp
    error.hpp
    extractionstats.cpp
    extractionstats.hpp
    fogbuilder.cpp
    fogbuilder.hpp
    fognoise.cpp
    fognoise.hpp
    frameimage.cpp
    frameimage.hpp
    frametimes.cpp
    frametimes.hpp
    frameworld.cpp
    frameworld.hpp
    imageformat.cpp
    imageformat.hpp
    index.hpp
    instancerecord.cpp
    instancerecord.hpp
    lightbuilder.cpp
    lightbuilder.hpp
    lightgrid.cpp
    lightgrid.hpp
    meantexel.cpp
    meantexel.hpp
    meshinstance.hpp
    materialresolver.cpp
    materialresolver.hpp
    meshresolver.cpp
    meshresolver.hpp
    mirroridentity.hpp
    mirrorpass.hpp
    moonbuilder.cpp
    moonbuilder.hpp
    nodelibrary.hpp
    nightsky.cpp
    nightsky.hpp
    offscreentrace.cpp
    offscreentrace.hpp
    placementtable.cpp
    placementtable.hpp
    png.cpp
    png.hpp
    posecull.hpp
    poseupdate.hpp
    reconstruction.hpp
    renderer.hpp
    reorder.hpp
    runlist.hpp
    scenedesc.cpp
    scenedesc.hpp
    sceneextractor.cpp
    sceneextractor.hpp
    sceneuploader.cpp
    sceneuploader.hpp
    shading.cpp
    shading.hpp
    shadingcache.hpp
    shadingmap.cpp
    shadingmap.hpp
    shapefold.cpp
    shapefold.hpp
    skybuilder.cpp
    skybuilder.hpp
    slotchanges.cpp
    slotchanges.hpp
    slotset.cpp
    slotset.hpp
    spanallocator.cpp
    spanallocator.hpp
    spritelight.cpp
    spritelight.hpp
    spritelistsize.cpp
    spritelistsize.hpp
    spriteshade.cpp
    spriteshade.hpp
    srgb.cpp
    srgb.hpp
    terraincomposite.cpp
    terraincomposite.hpp
    terrainresidency.cpp
    terrainresidency.hpp
    texelreader.cpp
    texelreader.hpp
    texturebuilder.cpp
    texturebuilder.hpp
    texturedata.hpp
    texturetable.cpp
    texturetable.hpp
    traversals.hpp
    wavecascade.cpp
    wavecascade.hpp
    wavespectrum.cpp
    wavespectrum.hpp
)

# Structures and constants shared verbatim with the shader language, through `#ifdef __cplusplus`.
# They stay with the core rather than following the GLSL, because the host reads them as well.
set(RTX_SHADER_HEADERS
    shaders/bindings.h
    shaders/colour.h
    shaders/atrous.h
    shaders/composite.h
    shaders/gbuffer.h
    shaders/look.h
    shaders/micromap.h
    shaders/portable.h
    shaders/scene.h
    shaders/skinning.h
    shaders/spritebin.h
    shaders/visibility.h
    shaders/wave.h
)

source_group(components\\rtx FILES ${RTX_FILES} ${RTX_SHADER_HEADERS})

add_library(openmw-rtx STATIC ${RTX_FILES} ${RTX_SHADER_HEADERS})

# **`PUBLIC`, because the interface is not self-contained.** `sceneextractor.hpp` takes an
# `osg::NodeVisitor` and an `osgParticle::ParticleSystem`, and `scenedesc.hpp` has always included
# `<osg/Vec3f>` — so every consumer needs the include paths this carries.
target_link_libraries(openmw-rtx PUBLIC components)

# **`PUBLIC`, because the constant it decides is in a header both hosts read.** The layers are on
# unless this is a Release build, the same condition the debug object names use, and
# `Rtx::sValidationByDefault` is where that lands. Here rather than on each host, so the game and the
# harness cannot come to disagree about when a frame is being checked.
target_compile_definitions(openmw-rtx PUBLIC
                           $<$<NOT:$<CONFIG:Release>>:OPENMW_RTX_VALIDATION_BY_DEFAULT>)

target_compile_options(openmw-rtx PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-rtx PRIVATE --coverage)
    target_link_libraries(openmw-rtx gcov)
endif()
