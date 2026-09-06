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

## P3 — performance, unmeasured

- [ ] **Static acceleration structures are never compacted.** `sceneacceleration.cpp` asks for fast
      trace and data access, and update only for deforming meshes, but never `ALLOW_COMPACTION`.
      `scene` reports 226 MiB of structures at `island-crossing`, which is what a saving would come
      out of.

      **Measured, and it is worth doing.** The flag and the size query landed, so `scene` and `bench`
      now print the pair:

          seyda-neen-ship   121.6 MiB structures, 118.0 of them would compact to  46.6   — 60% off
          island-crossing   227.4 MiB structures, 145.6 of them would compact to  56.6   — 61% off

      `ALLOW_COMPACTION` itself cost 0.7 MiB of 120.9, which is 0.6%.

      **What is left is the copy, and it is a two-frame state machine.** A compacted size is known
      only after the build has run, so: read the query on a later frame, allocate the compacted
      room, copy with `MODE_COMPACT_KHR`, swap the handle and the address, mark every instance that
      names the mesh changed so the top level is rebuilt from the new address, and bury the original
      — while whatever frame is in flight still traces the old one. That is the whole of the risk.

      `mPositions` also keeps the build inputs of static meshes that position fetch could answer
      from the structure.

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
