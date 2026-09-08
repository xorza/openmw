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

- [ ] **Opacity micromaps are emulated below Ada, and the two legs have never been timed.** They
      work and they can still pay, but the Ada speedup is not transferable.

      **`--micromaps=false` is the other leg.** It sets `RendererOptions::mMicromaps`, and with it
      off nothing is baked and every cutout reaches the any-hit. At Seyda Neen the report goes from
      1369 micromapped and 23.4 MiB of micromaps to none, and the picture is the same either way —
      `RtxMicromapBakeTest` asserts that nothing is held and `RtxMicromapPictureTest` that a
      micromapped card traces as the any-hit traces it. So `bench --micromaps=true` against
      `--micromaps=false` over the same views is the measurement, on any card.

      **What it could change is dropping micromaps for every card**, and not adding a path for
      Turing. The extension is required and `AGENTS.md` keeps no second path, so a micromap that
      loses is an argument about which single path the tree keeps.

      Take it on an idle card, warm, with the legs interleaved. Turing is the card the item is
      really about, and there is none here.

Sources: [Vulkan Hardware Database](https://vulkan.gpuinfo.org/), reports 51568 and 46422;
[Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html);
[NVIDIA Opacity Micro-Map SDK integration guide](https://github.com/NVIDIA-RTX/OMM/blob/main/docs/integration_guide.md);
[NVIDIA Vulkan dos and don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/);
[NVIDIA RTX ray tracing best practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).
