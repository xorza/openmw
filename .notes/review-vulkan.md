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

So the device baseline is not the problem people assume, and nothing below rejects a Turing card any
more. What is left is defects the hardware does not decide.

---

## P2

- [ ] **Presentation resources are retired on a queue-idle rather than on a present.**
      `presenter.cpp:94` waits the device, then destroys the present semaphores and the swapchain.
      `mPresenting` belongs to the blit's submit, not to `vkQueuePresentKHR`, and an unextended idle
      wait does not prove the presentation engine is done. `VK_EXT_swapchain_maintenance1` is present
      on both Turing reports, so present fences are available on every card this targets.

## P3 — performance, unmeasured

- [ ] **A block is sized from the heap and not from the budget.** `blockBytes` takes a sixteenth of
      the heap's static size, so a card most of whose memory another process holds is asked for a
      block that cannot fit — and `take` has no smaller second try before it gives up.

- [ ] **Static acceleration structures are never compacted.** `sceneacceleration.cpp:328` asks for
      fast trace and data access, and update only for deforming meshes, but never
      `ALLOW_COMPACTION`. No size query and no copy path exists. `mPositions` also keeps the build
      inputs of static meshes that position fetch could answer from the structure.

- [ ] **Streaming and the interface drain the frame pipeline.** `vulkanrenderer.cpp:594` finishes
      every frame before extending the world, offscreen placement uses `submitAndWait` at
      `vulkanrenderer.cpp:720`, and the GUI trace drains again at `vulkanrenderer.cpp:1307`. Cells
      arrive while the game runs, so each is a stall a player feels.

      The waits cannot simply go: the texture set is shared and is not update-after-bind. Move that
      ownership first, then enqueue the independent work into the ordered submission path.

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
