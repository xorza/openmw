Delete an item when it is addressed. The list below holds open findings only.

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

## Open

- [ ] **The compaction copy.** `ALLOW_COMPACTION` and the size query landed, and the pair they print
      says the copy is worth building: **118.0 MiB would compact to 46.6 at Seyda Neen, and 145.6 to
      56.6 on the route — 60% off**, against 0.6% of the structures spent on the flag itself. What
      is left is the two-frame half. Read the query on a later frame, allocate the tight room, copy
      with `MODE_COMPACT_KHR`, swap the handle and the address, mark every instance that names the
      mesh changed so the top level is rebuilt from the new address, and bury the original while the
      frame in flight still traces it. `MODE_COMPACT_KHR` appears nowhere in the tree.

- [ ] **A static mesh keeps its positions after its structure is built.** `mPositions` is a build
      input and a pose's destination and nothing else — `sceneacceleration.hpp` says so, because a
      hit reads its vertices back out of the structure through position fetch. A mesh that never
      refits has no second reader, so its run could go once the build has read it.

- [ ] **`SlotBlocks::reserve` grows every copy to the whole scene.** `sceneacceleration.cpp:187`
      reserves the scene's positions in both copies, and `:201` writes copy 1 only for a mesh that
      deforms — 138 drawables of 2800 at Seyda Neen when it was last measured, and about 21 MiB of
      video memory bought for nothing. A compact second table needs its own offset space, and a site
      that mixed the two would alias silently, so it wants the mesh index rather than a raw offset at
      every call site. `SceneBuffers::mNormalTable` is not this: a hit reads a normal out of the
      frame's own copy, so every copy there owes every mesh.

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
      for a feature that is off by default. Accepting the `NV` extension as an alias, or dropping the
      hit-object path when nothing reorders, both remove that.

- [ ] **Opacity micromaps are emulated below Ada.** They work and they can still pay, but the Ada
      speedup is not transferable. Measure the micromap path against plain any-hit traversal on
      Turing before assuming it is the faster of the two. The extension is required, so there is no
      second path to fall to.

## Settled — do not propose these again

- **A Vulkan proxy.** A call-forwarding layer catches "you called the wrong function", and every
  finding here was "the right function, with the wrong scope, at the wrong time, or against the wrong
  memory". `Owned<>` already covers handle lifetime, and more Vulkan surface is the opposite of what
  `AGENTS.md` asks for.
- **A render graph.** The passes are a fixed sequence decided at build time. Automatic pass ordering
  is the part of a graph that does not pay here.
- **A resource state tracker behind a `Use` enum.** The forty-seven transition sites mostly encode a
  deliberate scope with its reason written beside it — `GBuffer::begin` sources at `ALL_COMMANDS` and
  says why a discard from `TOP_OF_PIPE` would buy a torn frame. An enum would flatten those or need a
  case each. Both things that justified it, the sampled-read class and the batching, were had without
  it through `Buffer::orderForHostRead` and `Barriers`.
- **Hand-written barrier validation.** `CI/check_rtx_validation.sh` drives the layer, which knows
  several hundred rules more than we would write.
- **A `Residency` the device profile chooses between.** The copies past the first are written by the
  skinning shader, and an arrival write is a load-path event, so there is no choice to make: every
  blocked table is device memory, staged through the batch a load already records, on every card.
- **A second code path for Turing.** `AGENTS.md` forbids it, and the profile changes numbers and
  residency rather than the shape of a frame.

Sources: [Vulkan Hardware Database](https://vulkan.gpuinfo.org/), reports 51568 and 46422;
[Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html);
[NVIDIA Opacity Micro-Map SDK integration guide](https://github.com/NVIDIA-RTX/OMM/blob/main/docs/integration_guide.md);
[NVIDIA Vulkan dos and don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/);
[NVIDIA RTX ray tracing best practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).
