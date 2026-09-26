# What this fork adds to the test binaries, listed here so that upstream's list stays upstream's.
# Paths are relative to `apps/components_tests`, which is where this is included from.
#
# Two binaries. `components-tests` holds what runs on any machine; `rtx-gpu-tests` holds what
# opens a device, and fails outright where there is none, so a run on a box without a driver
# cannot pass by skipping half the suite. `RTX_TEST_SUPPORT` is compiled into both and
# `RTX_GPU_TEST_SUPPORT` into the second alone, and two more lists at the end name the tests that
# take the flags differently.
set(RTX_TEST_FILES
    rtx/alphaimage.cpp
    rtx/bluenoise.cpp
    rtx/brdf.cpp
    rtx/camera.cpp
    rtx/cellgrid.cpp
    rtx/cellring.cpp
    rtx/cloudshell.cpp
    rtx/colour.cpp
    rtx/compositequeue.cpp
    rtx/dispatch.cpp
    rtx/exposure.cpp
    rtx/extractor/fixture.hpp
    rtx/extractor/lights.cpp
    rtx/extractor/materials.cpp
    rtx/extractor/particles.cpp
    rtx/extractor/retire.cpp
    rtx/extractor/skinning.cpp
    rtx/extractor/stats.cpp
    rtx/extractor/walk.cpp
    rtx/fogbuilder.cpp
    rtx/formats.cpp
    rtx/frameimage.cpp
    rtx/framesampling.cpp
    rtx/frameworld.cpp
    rtx/groundreader.cpp
    rtx/halfstep.cpp
    rtx/hitrecords.cpp
    rtx/instance.cpp
    rtx/instancerecord.cpp
    rtx/latencypacer.cpp
    rtx/lightbuilder.cpp
    rtx/lightgrid.cpp
    rtx/memoryreport.cpp
    rtx/meshreader.cpp
    rtx/mipchain.cpp
    rtx/mirroridentity.cpp
    rtx/monitor.cpp
    rtx/moonbuilder.cpp
    rtx/namedenum.cpp
    rtx/nodekind.cpp
    rtx/offscreentrace.cpp
    rtx/pacedmodes.cpp
    rtx/pacing.cpp
    rtx/parallel.cpp
    rtx/physicaldevice.cpp
    rtx/pixelgrid.cpp
    rtx/reconstruction.cpp
    rtx/refusals.cpp
    rtx/requirements.cpp
    rtx/result.cpp
    rtx/runs.cpp
    rtx/scenedesc.cpp
    rtx/sceneuploader.cpp
    rtx/shaderdirectory.cpp
    rtx/shading.cpp
    rtx/shadingmap.cpp
    rtx/shapefold.cpp
    rtx/sharedtexture.cpp
    rtx/skybuilder.cpp
    rtx/skylight.cpp
    rtx/slots.cpp
    rtx/sourcetree.cpp
    rtx/specularalbedo.cpp
    rtx/spirvdigest.cpp
    rtx/spirvpin.cpp
    rtx/spritelight.cpp
    rtx/spritelistsize.cpp
    rtx/stepped.cpp
    rtx/sun.cpp
    rtx/surface.cpp
    rtx/tangent.cpp
    rtx/templatewalk.cpp
    rtx/texels.cpp
    rtx/texturebuilder.cpp
    rtx/wavecascade.cpp
    rtx/wavespectrum.cpp
    rtx/worker.cpp
    rtxtool/benchrecord.cpp
    rtxtool/benchrun.cpp
    rtxtool/benchspec.cpp
    rtxtool/blockfile.cpp
    rtxtool/cameratrack.cpp
    rtxtool/cardwatch.cpp
    rtxtool/compare.cpp
    rtxtool/contactsheet.cpp
    rtxtool/drivercache.cpp
    rtxtool/film.cpp
    rtxtool/framehashes.cpp
    rtxtool/frametimes.cpp
    rtxtool/gpuclock.cpp
    rtxtool/options.cpp
    rtxtool/run.cpp
    rtxtool/scenedigest.cpp
    sky/skyclock.cpp
    sky/sundisc.cpp
    sky/timeofday.cpp
)

set(RTX_TEST_SUPPORT
    rtx/support/countingrenderer.hpp
    rtx/support/death.hpp
    rtx/support/displaycurve.hpp
    rtx/support/fakeland.hpp
    rtx/support/fallbackseed.cpp
    rtx/support/geometry.hpp
    rtx/support/graph.hpp
    rtx/support/graphlight.hpp
    rtx/support/guiquad.hpp
    rtx/support/halfstep.hpp
    rtx/support/heldimages.hpp
    rtx/support/instanceobstacle.cpp
    rtx/support/instanceobstacle.hpp
    rtx/support/layers.hpp
    rtx/support/lobeintegrals.hpp
    rtx/support/spritelightbake.cpp
    rtx/support/spritelightbake.hpp
    rtx/support/statistics.hpp
    rtx/support/testcamera.hpp
    rtx/support/testtexture.hpp
    rtx/support/wavemoments.hpp
)

set(RTX_GPU_TEST_SUPPORT
    rtx/support/device/harness.cpp
    rtx/support/device/harness.hpp
    rtx/support/device/heldsubmit.cpp
    rtx/support/device/heldsubmit.hpp
    rtx/support/device/memorylimits.cpp
    rtx/support/device/memorylimits.hpp
    rtx/support/device/readback.cpp
    rtx/support/device/readback.hpp
    rtx/support/device/texturepasses.hpp
)

set(RTX_GPU_TEST_FILES
    rtx/bloompass.cpp
    rtx/bottomlevelstore.cpp
    rtx/buffer.cpp
    rtx/commands.cpp
    rtx/computepipeline.cpp
    rtx/device.cpp
    rtx/digestpass.cpp
    rtx/dlss.cpp
    rtx/framering.cpp
    rtx/frames.cpp
    rtx/gputimer.cpp
    rtx/groundcompositepass.cpp
    rtx/guipass.cpp
    rtx/guitextures.cpp
    rtx/memory.cpp
    rtx/mipchainpass.cpp
    rtx/pinnedarithmetic.cpp
    rtx/pipelinecache.cpp
    rtx/probe.cpp
    rtx/readstamp.cpp
    rtx/ripplepass.cpp
    rtx/shadingpass.cpp
    rtx/skinpass.cpp
    rtx/slottable.cpp
    rtx/spritelightpass.cpp
    rtx/spritepasses.cpp
    rtx/stresspass.cpp
    rtx/structurestorage.cpp
    rtx/texturearray.cpp
    rtx/tracepipeline.cpp
    rtx/visibility/filter.cpp
    rtx/visibility/fixture.hpp
    rtx/visibility/fog.cpp
    rtx/visibility/frame.cpp
    rtx/visibility/framecost.cpp
    rtx/visibility/kernels.cpp
    rtx/visibility/light.cpp
    rtx/visibility/sea.cpp
    rtx/visibility/sky.cpp
    rtx/visibility/sprites.cpp
    rtx/visibility/surfaces.cpp
    rtx/visibility/water.cpp
    rtx/wavefield.cpp
    rtx/waveline.cpp
    rtx/wavepass.cpp
)


# What is read off a fifo, where the platform has one.
set(RTX_TEST_FILES_FIFO rtxtool/perffifo.cpp)
if (NOT WIN32)
    list(APPEND RTX_TEST_FILES ${RTX_TEST_FILES_FIFO})
endif()

# Reads a NIF through upstream's loader, whose headers are not warning-free under the extra
# checks, so it takes the errors alone.
set(RTX_TEST_FILES_UPSTREAM
    rtx/nifsurface.cpp
)

# This fork's tests of what a build without the ray tracer compiles as well, and the allocation
# counter they read. They stand in upstream's own list, so that build runs them too, and are named
# here for the fork's flags.
set(RTX_TEST_FILES_EITHER
    misc/frameclock.cpp
    rtx/support/allocations.cpp
    rtx/support/allocations.hpp
    sceneutil/paintedtexture.cpp
    terrain/refstack.cpp
)
