Delete an item when it is addressed. This file lists open findings only.

# Vulkan review: RTX 20-series, stability, performance

Reviewed 2026-09-06 against the working tree on `2812ec9342`, uncommitted suballocator included.
Scope: `components/rtxvulkan/`, its shaders' API surface, and the callers in `components/rtx/` and
`apps/openmw/mwrender/rtx/`.

Static review. No Turing hardware here, and no measurements were taken. Device facts come from the
Vulkan Hardware Database, from two reports read in full: an **RTX 2080 on 616.64, Vulkan 1.4.351**
(report 51568) and an **RTX 2060 on Linux, 590.48.1, Vulkan 1.4.325** (report 46422).

## What a Turing card already satisfies

**Every required extension and every required feature bit, on both reports.** `micromap`,
`rayTracingInvocationReorder`, `rayTracingPositionFetch`, `pipelineExecutableInfo`, `pushDescriptor`,
`maintenance5`, `scalarBlockLayout` — all `true`. `maxOpacity4StateSubdivisionLevel` is 12, the same
as Ada, so `SceneMicromaps` passes. Vulkan 1.4 is reported on Linux from driver 570.144. Opacity
micromaps run on pre-Ada hardware through the driver's own emulation, so the extension is real work
rather than a stub.

So the device baseline is not the problem people assume. Two things reject a Turing card, and both
are below.

---

## P1

- [ ] **The reordering-hint check refuses every card but Ada, for a feature that is off by
      default.** `physicaldevice.cpp:127` rejects a device whose
      `rayTracingInvocationReorderReorderingHint` is not `REORDER`. Both Turing reports say `0`
      (`NONE`). `Rtx::Reorder` defaults to `Off`, and `reorder.glsl:20` records that every reorder
      mode measured 7 to 25 percent slower — so the check refuses hardware over a call the renderer
      does not make.

      Drop the check. Report the hint instead, and refuse only a `--reorder` other than `off` on a
      device that answers `NONE`. `hitObjectTraceRayEXT` and `hitObjectExecuteShaderEXT` stay valid
      there: the feature bit is `true` and `reorderThreadEXT` becomes a no-op, which is exactly the
      "one path for every card" the posture asks for.

- [ ] **Host-visible video memory is 246 MiB on Turing, and every table, mesh and shader binding
      table is put in it.** `buffer.cpp:15` defines `hostWritten` as
      `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`. `blockedbuffer.cpp:33` puts all vertices and
      indices there, `tracepipeline.cpp:141` every SBT, `scenemicromaps.cpp:312` the micromap
      triangle arrays, `guitextures.cpp:142` the GUI arena, and `commands.cpp:249` even the transient
      source of `uploadBuffer`.

      The heap that carries that memory type is 257,949,696 bytes on the RTX 2060 and 224,395,264 on
      the RTX 2080 — a card without resizable BAR, which NVIDIA enables from the RTX 30 series only.
      Morrowind's geometry alone passes that. This is a hard failure in the middle of a cell arrival,
      not at startup.

      `hostWritten` has to become a policy rather than a memory-property constant: direct writes into
      video memory where the heap is large enough to hold the scene, a staged path into ordinary
      device-local memory where it is not, chosen once when the device is picked. Nothing above the
      allocator should have to know which was chosen.

- [ ] **`hasResizableBar` proves nothing it claims, and its rejection message names the wrong
      cause.** `physicaldevice.cpp:73` tests property bits only. A 246 MiB aperture carries the same
      three bits as a 16 GiB one, so the test passes on every card checked and the renderer starts
      believing it has what it does not. `physicaldevice.cpp:137` then blames firmware for a case it
      never reached.

      Test the heap's size, not the type's flags, and let the answer feed the policy above rather
      than a refusal.

- [ ] **The pipeline compile workers race the memory allocator.** `visibilitypass.cpp:256` starts a
      worker per core. Each builds a `TracePipeline`, whose constructor reaches
      `Buffer::hostWritten` at `tracepipeline.cpp:141` and so `MemoryAllocator::take` at
      `memory.cpp:142`. `memory.hpp:99` states the allocator is not thread-safe, and `take` mutates
      `mBlocks` — a `std::vector` that reallocates — and each block's `SpanAllocator`. `give` at
      `memory.cpp:206` is equally unguarded.

      This is new with the suballocator: `vkAllocateMemory` per resource was the driver's problem,
      and this bookkeeping is ours. Either lock the allocator or build the shader binding tables on
      the calling thread after the workers join. Keep the parallel compile.

- [ ] **Nothing the device writes is made visible to the host.** Three sites read mapped memory
      after a fence and no barrier: `image.cpp:252` for every screenshot, channel and map readback;
      `framering.cpp:117` for the hit counters; `scenebuffers.cpp:246` for the sprite bin report,
      which then sizes the next frame's tile list.

      A fence's access scope covers device access only. Add a
      `COPY / TRANSFER_WRITE → HOST / HOST_READ` dependency before the submit that the host waits
      on. Host-coherent memory removes the `vkInvalidateMappedMemoryRanges`, not the barrier.

- [ ] **A texture that fails half way submits commands naming an image it already destroyed.**
      `texture.cpp:176` creates the shading image after `uploadImage` has recorded the primary
      image's copy into the batch. If that allocation throws, `~Texture` never runs but `mImage`'s
      destructor does — and `Batch::~Batch` at `commands.cpp:173` then flushes the command buffer
      that still names the destroyed image.

      Make submission explicit. An abandoned batch must drop its recording rather than submit it.

## P2

- [ ] **The handover barriers cover storage reads and the consumers are sampling.**
      `gbuffer.cpp:238` exposes the trace's writes as `SHADER_STORAGE_READ` at the compute stage,
      and `dlsspass.cpp:59` asserts `SAMPLED_BIT` on every guide it hands NGX. `vulkanrenderer.cpp:1175`
      does the same for the NGX output, which `bloompass.cpp:26` then binds as a combined image
      sampler.

      Add `SHADER_SAMPLED_READ` to both, and give the NGX handover a stage scope that covers work
      whose stages we do not know. `GENERAL` layout is not a memory dependency.

- [ ] **A device that fails to finish construction is destroyed before its children.**
      `device.cpp:169` builds the memory allocator after the pipeline cache; the `catch` at
      `device.cpp:171` calls `vkDestroyDevice` and rethrows, and member unwinding then runs
      `~PipelineCache` — which reads the cache off the device — and `~MemoryAllocator`, whose blocks
      call `vkFreeMemory`, both on a destroyed handle.

      Reset both members inside the `catch`, before the device goes.

- [ ] **Wave spectra are replaced while the last frame may still be reading them.**
      `wavepass.cpp:150` assigns new amplitude and frequency buffers straight over the live ones, and
      `vulkanrenderer.cpp:733` calls it before the placement's resource wait. Latent only because
      `worldmirror.cpp:145` always hands over a default `SeaState`, so `describe` returns early after
      the first call. It fires the moment weather drives the wind.

      Bury the displaced buffers in the recording frame's graveyard, the way every other growth on
      this path already does.

- [ ] **Two renders from one placement write the sprite tables the first render is still tracing.**
      `renderer.hpp:642` promises a `renderFrame` independent of `placeScene`, with two frames in
      flight. `vulkanrenderer.cpp:1108` bins into `mWorldSlot`, and `scenebuffers.cpp:238` rewrites
      that slot's mapped sprite data. The `mReadBy` wait is in `placeScene` only, so a second render
      with no placement between reuses the slot. No current caller does it.

      Give the camera-dependent sprite resources frame ownership rather than placement ownership.

- [ ] **A minimised window creates a swapchain of zero extent.** `swapchain.cpp:135` accepts
      `currentExtent` as the surface reports it, and Wayland reports `0×0` while minimised.
      `swapchain.cpp:155` also asks for `TRANSFER_DST` and opaque composite alpha without reading
      `supportedUsageFlags` or `supportedCompositeAlpha`.

      Suspend recreation while the extent is zero, and check the two masks before creation.

- [ ] **Every swapchain rebuild orphans a set of command buffers.** `presenter.cpp:135` resets the
      pool and `presenter.cpp:162` allocates a fresh set. `vkResetCommandPool` resets buffers; it
      does not free them. Resize, vsync change and every out-of-date acquire adds another set for the
      presenter's life.

      Free the old buffers, or reuse them and only reallocate when the image count changes.

- [ ] **Validation can report clean because it never ran, and worker errors are dropped.**
      `instance.cpp:87` continues with a warning when the layer or debug-utils extension is missing,
      so `takeValidationErrors` returns nothing and a gate reads it as a pass. Separately,
      `vulkanrenderer.cpp:1511` collects only the calling thread's messages and then clears the whole
      log — so every error raised on a pipeline compile worker disappears.

      Make a run that asked for validation fail by name when it cannot have it, and drain all
      messages under the one lock.

- [ ] **Presentation resources are retired on a queue-idle rather than on a present.**
      `presenter.cpp:94` waits the device, then destroys the present semaphores and the swapchain.
      `mPresenting` belongs to the blit's submit, not to `vkQueuePresentKHR`, and an unextended idle
      wait does not prove the presentation engine is done. `VK_EXT_swapchain_maintenance1` is present
      on both Turing reports, so present fences are available on every card this targets.

- [ ] **The NGX capability parameter map is never destroyed.** `dlss.cpp:142` takes it with
      `GetCapabilityParameters` and `dlss.cpp:200` only calls `Shutdown1`. `DlssPass` already does
      the right thing at `dlsspass.cpp:134`, so the call is known here.

## P3 — performance, unmeasured

- [ ] **The allocator never gives a block back, and never asks what it may have.** `memory.cpp:206`
      releases suballocation ranges only; blocks live until the device does. Pools are split by
      memory type and tiling, so free space in one cannot serve another, and `blockBytes` sizes from
      the static heap size with no `VK_EXT_memory_budget` query — the extension is present on both
      Turing reports. A 6 GiB card is where this stops being theoretical.

      Account reserved, live and budgeted bytes per heap, and retire an empty block incrementally
      rather than on a sweep.

- [ ] **Static acceleration structures are never compacted.** `sceneacceleration.cpp:328` asks for
      fast trace and data access, and update only for deforming meshes, but never
      `ALLOW_COMPACTION`. No size query and no copy path exists. `mPositions` also keeps the build
      inputs of static meshes that position fetch could answer from the structure.

- [ ] **A shared dependency point emits one barrier command per image.** `gbuffer.cpp:218` and
      `gbuffer.cpp:238` each loop over 14 channels calling `Image::transition`, which emits one
      `vkCmdPipelineBarrier2` apiece — 28 commands a frame where 2 would do. `fogvolume.cpp:183`
      loops over 10 more.

      Fill a fixed-size barrier array and emit one dependency per handover.

- [ ] **Streaming and the interface drain the frame pipeline.** `vulkanrenderer.cpp:594` finishes
      every frame before extending the world, offscreen placement uses `submitAndWait` at
      `vulkanrenderer.cpp:720`, and the GUI trace drains again at `vulkanrenderer.cpp:1307`. Cells
      arrive while the game runs, so each is a stall a player feels.

      The waits cannot simply go: the texture set is shared and is not update-after-bind. Move that
      ownership first, then enqueue the independent work into the ordered submission path.

- [ ] **Creating one pipeline cache deletes every other one.** `pipelinecache.cpp:224` removes each
      regular file in the directory whose name starts with the RTX prefix. Cache names already carry
      hardware, driver and shader identity, so alternating devices or shader builds pays a cold
      compile every time — and "every card from Turing up" makes alternating devices a normal case
      rather than a curiosity.

## Two facts worth writing down before Turing is claimed

1. **`VK_EXT_ray_tracing_invocation_reorder` sets a driver floor.** The RTX 2060 on 590.48.1 exposes
   only the `NV` spelling; the `EXT` one first appears around 595 on Turing and 582 on Ada. A user on
   the 580 branch is refused today, for the feature that is off by default. Accepting the `NV`
   extension as an alias, or dropping the hit-object path when nothing reorders, both remove that.

2. **Opacity micromaps are emulated below Ada.** They work and they can still pay, but the Ada
   speedup is not transferable. Measure the micromap path against plain any-hit traversal on Turing
   before assuming it is the faster of the two.

Sources: [Vulkan Hardware Database](https://vulkan.gpuinfo.org/), reports 51568 and 46422;
[Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html);
[NVIDIA Opacity Micro-Map SDK integration guide](https://github.com/NVIDIA-RTX/OMM/blob/main/docs/integration_guide.md);
[NVIDIA Vulkan dos and don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/);
[NVIDIA RTX ray tracing best practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).
