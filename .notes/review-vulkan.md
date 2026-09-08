Delete an item when it is addressed. This file lists open findings only.

# Vulkan review: RTX 20-series, stability, performance

Reviewed 2026-09-06 against `2812ec9342`, rechecked 2026-09-08 against `571c1a9c72`. Scope:
`components/rtxvulkan/`, its shaders' API surface, and the callers in `components/rtx/` and
`apps/openmw/mwrender/rtx/`.

No Turing hardware here. Device facts come from the Vulkan Hardware Database, from two reports read
in full: an **RTX 2080 on 616.64, Vulkan 1.4.351** (report 51568) and an **RTX 2060 on Linux,
590.48.1, Vulkan 1.4.325** (report 46422). Both carry every required extension and every required
feature bit, and `maxOpacity4StateSubdivisionLevel` 12 — the same as Ada — so the device baseline is
not what stops a Turing card.

---

- [ ] **Compaction reuses the loose room instead of returning it, and no route has been measured.**
      `place` copies a static structure tight and gives the room it stood in back, but a block goes
      to the device only when nothing is left in it. One block holds a whole cell, and `take` fills
      the first block with room — so the tight copies land back in the very block they are draining,
      and the reservation stays. Seyda Neen settles at 52.5 MiB of structures in 152.7 MiB reserved,
      where the same cell reserved 136.7 MiB before compaction.

      **Storage of its own for the meshes that refit does not fix this**, which an earlier note here
      claimed: the tight copies are static and would go back into the draining block all the same.

      What compaction is for is the high-water mark of a route, and that is a measurement rather
      than a design. `bench --views=island-crossing --seconds=10` against the 249.1 MiB the route
      reserved before compaction is the number that decides the item. Take it first.

- [ ] **Streaming and the interface drain the frame pipeline.** `vulkanrenderer.cpp:558` finishes
      every frame in flight before it extends the world, offscreen placement waits at `:684`, and the
      offscreen trace drains again at `:1307`. Cells arrive while the game runs, so each is a stall a
      player feels. The composite landing's own submit and fence are already gone — that batch rides
      the placement's submit.

      The waits cannot simply go: the texture set is shared and is not update-after-bind. Move that
      ownership first, then enqueue the independent work into the ordered submission path.

- [ ] **`VK_EXT_ray_tracing_invocation_reorder` sets a driver floor.** It is required at
      `requirements.cpp:28`. The RTX 2060 on 590.48.1 exposes only the `NV` spelling; the `EXT` one
      first appears around 595 on Turing and 582 on Ada. A user on the 580 branch is refused today,
      for a feature that is off by default because every mode of it measured slower.

      **Neither fix an earlier note here proposed is host-side.** The compiled `visibility.rgen`
      declares `SPV_EXT_shader_invocation_reorder` and `ShaderInvocationReorderEXT`, which only the
      `EXT` Vulkan extension provides, so accepting the `NV` spelling needs a second binary.
      `traceprobe.rgen` calls `reorderThreadEXT` outside any specialization constant, so building
      without the extension needs a second binary too. Both are the fallback path `AGENTS.md`
      forbids.

      So the question is whether the driver floor stands. If it does, the refusal should name the
      driver a Turing card needs rather than the extension it lacks.

- [ ] **Opacity micromaps are emulated below Ada.** They work and they can still pay, but the Ada
      speedup is not transferable. Measure the micromap path against plain any-hit traversal on
      Turing before assuming it is the faster of the two. The extension is required, so there is no
      second path to fall to.

Sources: [Vulkan Hardware Database](https://vulkan.gpuinfo.org/), reports 51568 and 46422;
[Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html);
[NVIDIA Opacity Micro-Map SDK integration guide](https://github.com/NVIDIA-RTX/OMM/blob/main/docs/integration_guide.md);
[NVIDIA Vulkan dos and don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/);
[NVIDIA RTX ray tracing best practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).
