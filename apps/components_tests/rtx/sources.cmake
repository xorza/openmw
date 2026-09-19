# What this fork adds to the test binaries, listed here so that upstream's list stays upstream's.
# Paths are relative to `apps/components_tests`, which is where this is included from.
#
# Three lists, because there are two binaries. `components-tests` holds what runs on any machine;
# `rtx-gpu-tests` holds what opens a device, and fails outright where there is none, so a run on
# a box without a driver cannot pass by skipping half the suite. The support list is compiled into
# both.
set(RTX_TEST_FILES
    rtx/alphaimage.cpp
    rtx/bluenoise.cpp
    rtx/cellgrid.cpp
    rtx/cellring.cpp
    rtx/cloudshell.cpp
    rtx/colour.cpp
    rtx/compositequeue.cpp
    rtx/dispatch.cpp
    rtx/extractor/fixture.hpp
    rtx/extractor/lights.cpp
    rtx/extractor/materials.cpp
    rtx/extractor/particles.cpp
    rtx/extractor/retire.cpp
    rtx/extractor/skinning.cpp
    rtx/extractor/stats.cpp
    rtx/extractor/walk.cpp
    rtx/fogbuilder.cpp
    rtx/frameimage.cpp
    rtx/frameoptions.cpp
    rtx/frameworld.cpp
    rtx/graphlight.hpp
    rtx/groundreader.cpp
    rtx/instancerecord.cpp
    rtx/lightbuilder.cpp
    rtx/lightgrid.cpp
    rtx/meshreader.cpp
    rtx/mipchain.cpp
    rtx/mirroridentity.cpp
    rtx/monitor.cpp
    rtx/moonbuilder.cpp
    rtx/nodekind.cpp
    rtx/offscreentrace.cpp
    rtx/parallel.cpp
    rtx/physicaldevice.cpp
    rtx/reconstruction.cpp
    rtx/requirements.cpp
    rtx/result.cpp
    rtx/runs.cpp
    rtx/scenedesc.cpp
    rtx/sceneuploader.cpp
    rtx/shading.cpp
    rtx/shadingmap.cpp
    rtx/shapefold.cpp
    rtx/skybuilder.cpp
    rtx/skylight.cpp
    rtx/slots.cpp
    rtx/sourcetree.cpp
    rtx/spritelight.cpp
    rtx/spritelistsize.cpp
    rtx/stepped.cpp
    rtx/sun.cpp
    rtx/surface.cpp
    rtx/templatewalk.cpp
    rtx/texels.cpp
    rtx/texturebuilder.cpp
    rtx/wavecascade.cpp
    rtx/wavespectrum.cpp
    rtx/worker.cpp
    rtxbench/benchrecord.cpp
    rtxbench/benchrun.cpp
    rtxbench/benchspec.cpp
    rtxbench/framehashes.cpp
    rtxbench/frametimes.cpp
    rtxbench/gpuclock.cpp
    rtxbench/runrecord.cpp
    rtxbench/scenedigest.cpp
    rtxtool/compare.cpp
    rtxtool/options.cpp
    rtxtool/run.cpp
    sky/skyclock.cpp
    sky/sundisc.cpp
    sky/timeofday.cpp
)

set(RTX_TEST_SUPPORT
    rtx/allocations.cpp
    rtx/allocations.hpp
    rtx/fallbackseed.cpp
    rtx/geometry.hpp
    rtx/guiquad.hpp
    rtx/layers.hpp
    rtx/statistics.hpp
    rtx/testtexture.hpp
    rtx/wavemoments.hpp
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
    rtx/harness.cpp
    rtx/harness.hpp
    rtx/memory.cpp
    rtx/mipchainpass.cpp
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
if (NOT WIN32)
    list(APPEND RTX_TEST_FILES rtxbench/perffifo.cpp)
endif()

# Reads a NIF through upstream's loader, whose headers are not warning-free under the extra
# checks, so it takes the errors alone.
set(RTX_TEST_FILES_UPSTREAM
    rtx/nifsurface.cpp
)
