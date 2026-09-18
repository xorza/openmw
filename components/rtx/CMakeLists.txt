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
    cellgrid.cpp
    cellgrid.hpp
    cellplacer.cpp
    cellplacer.hpp
    cellreader.cpp
    cellreader.hpp
    cellring.cpp
    cellring.hpp
    cellsupply.cpp
    cellsupply.hpp
    cloudshell.cpp
    cloudshell.hpp
    colour.cpp
    colour.hpp
    compositequeue.cpp
    compositequeue.hpp
    contract.hpp
    debuglines.hpp
    deformertable.cpp
    deformertable.hpp
    emitterresolver.cpp
    emitterresolver.hpp
    error.hpp
    extractionstats.cpp
    extractionstats.hpp
    fogbuilder.cpp
    fogbuilder.hpp
    frameclock.hpp
    frameimage.cpp
    frameimage.hpp
    framespend.hpp
    frameworld.cpp
    frameworld.hpp
    groundreader.cpp
    groundreader.hpp
    held.cpp
    held.hpp
    instancerecord.cpp
    instancerecord.hpp
    lightbuilder.cpp
    lightbuilder.hpp
    lightgrid.cpp
    lightgrid.hpp
    material.hpp
    materialresolver.cpp
    materialresolver.hpp
    materialtable.cpp
    materialtable.hpp
    meantexels.cpp
    meantexels.hpp
    memoryreport.cpp
    memoryreport.hpp
    mesh.hpp
    meshreader.cpp
    meshreader.hpp
    meshresolver.cpp
    meshresolver.hpp
    meshtable.cpp
    meshtable.hpp
    mipchain.cpp
    mipchain.hpp
    mirroridentity.hpp
    monitor.hpp
    moonbuilder.cpp
    moonbuilder.hpp
    namedenum.hpp
    nightsky.cpp
    nightsky.hpp
    nodekind.cpp
    nodekind.hpp
    offscreentrace.cpp
    offscreentrace.hpp
    ownedtexture.cpp
    ownedtexture.hpp
    parallel.hpp
    placementtable.cpp
    placementtable.hpp
    poseupdate.hpp
    prepared.hpp
    reconstruction.hpp
    renderer.hpp
    residency.hpp
    ripple.hpp
    runs.cpp
    runs.hpp
    scenedesc.cpp
    scenedesc.hpp
    sceneextractor.cpp
    sceneextractor.hpp
    sceneuploader.cpp
    sceneuploader.hpp
    scratch.hpp
    shading.cpp
    shading.hpp
    shadingmap.cpp
    shadingmap.hpp
    shapefold.cpp
    shapefold.hpp
    skybuilder.cpp
    skybuilder.hpp
    skylight.cpp
    skylight.hpp
    slot.hpp
    slots.cpp
    slots.hpp
    sprite.hpp
    spritelight.cpp
    spritelight.hpp
    spritelistsize.cpp
    spritelistsize.hpp
    stepped.hpp
    surface.cpp
    surface.hpp
    templatewalk.cpp
    templatewalk.hpp
    terraincomposite.cpp
    terraincomposite.hpp
    texels.cpp
    texels.hpp
    texturebuilder.cpp
    texturebuilder.hpp
    texturedata.cpp
    texturedata.hpp
    texturetable.cpp
    texturetable.hpp
    viewscene.cpp
    viewscene.hpp
    walk.hpp
    wavecascade.cpp
    wavecascade.hpp
    wavespectrum.cpp
    wavespectrum.hpp
    worker.hpp
    worlddescent.hpp
)

# Structures and constants shared verbatim with the shader language, through `#ifdef __cplusplus`.
# They stay with the core rather than following the GLSL, because the host reads them as well.
set(RTX_SHADER_HEADERS
    shaders/accumulate.h
    shaders/atrous.h
    shaders/bindings.h
    shaders/bloom.h
    shaders/camera.h
    shaders/colour.h
    shaders/composite.h
    shaders/exposure.h
    shaders/fogvolume.h
    shaders/gbuffer.h
    shaders/glare.h
    shaders/hosttypes.h
    shaders/line.h
    shaders/look.h
    shaders/portable.h
    shaders/probe.h
    shaders/ripple.h
    shaders/scene.h
    shaders/shadingmap.h
    shaders/skinning.h
    shaders/sky.h
    shaders/spritebin.h
    shaders/spritelight.h
    shaders/spriteshade.h
    shaders/stress.h
    shaders/tone.h
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

openmw_rtx_target(openmw-rtx)
