# RTX shader review — simplifications, deduplications, optimizations

Reviewed: every file under `components/rtxvulkan/shaders/` and the shared headers under
`components/rtx/shaders/`. Date: 2026-09-01, at commit d76a6e59eb.

The tree is in strong shape. The heavy duplications are already factored: one `RTX_RESOLVE`
candidate loop, one `rayAt`, one lamp record, one falloff, one Henyey-Greenstein, one bloom kernel
pair, and the specialization constants in `variants.glsl` already remove dead paths per frame kind.
**This file lists open work and nothing else** — an item applied or ruled out is deleted, not
marked. Each plan step names its verification. The working rule from CLAUDE.md holds: feature
first, numbers second. Section A is what bounds a frame today. Section B changes no pixel.
Sections C and D need a measurement.

## A. What bounds a frame today

**A1. The exteriors are CPU-bound, and it is the scene extractor re-deriving a scene that did not
change.** Measured at the shoreline, 1920x1080, quality upscale:

| config | frame | wait on GPU | CPU walk |
|---|---|---|---|
| default | 7.14 | 0.00 | 5.62 |
| `--people=0` | 5.58 | 1.75 | 3.01 |

**Not cell loading.** Crossings are counted separately by the bench, and `walk`'s *best* frame is
4.33 ms — every frame pays it. Nor is it camera motion: it is the same with a still camera.

Two costs, and they separate cleanly. The cell's animated residents are 2.6 ms of the 5.6, which is
skinned geometry that genuinely has to be re-read each frame. What is left with them held still is
**3.0 ms to conclude that nothing moved**: 65.9% of the profile is `SceneExtractor::extractWorld`
→ `walk` → `MirrorTraversal::apply`, with `osg::Group::traverse` at 11.6% self,
`SceneExtractor::addDrawable` at 10.5%, `resolveMesh` at 7.1%, `retire` at 4.2%, and about **17% of
the frame inside `hashtable_policy.h`** — the per-drawable "do I already know this?" lookups.

**The incremental design already works at the level it was built for**, which is content:
`ExtractionStats::mMeshesReused` counts exactly those lookups succeeding, and nothing is re-added.
What has no fast path is one level up — the walk descends the whole graph and asks per drawable,
where for a static subtree the answer is "unchanged" for the whole subtree at once. The pose
numbers the extractor already keeps (`sceneextractor.hpp`, "the rule that they only ever go up")
are the place to hang that.

**For shader work in the meantime, bench with `--people=0`.** It puts the frame back on the GPU —
wait goes from 0.00 to 1.75 ms — so a change to the trace shows up in frame time instead of being
hidden behind the walk. Its own help says it is "what a profiling run should hold still".

## B. Cleanups that change no pixel

**B6. `atrous` loads the centre texel up to three times.** `atrous.comp:104,131,137`. Read the
centre once and use it for both the luminance and the fallback.

**Worth almost nothing, and the bench says why: the cascade does not run in the shipping
configuration.** With DLSS Ray Reconstruction on — the default — the upscaler denoises for itself
and `accumulate`/`atrous` are not dispatched at all. No `bench` run lists either among its GPU
timings. So this is unmeasurable except under `--upscale=off`, and it belongs with whatever else
touches that path rather than on its own.

Note also that the fuller form — seeding the sums with the centre and skipping `(0, 0)` in the
loop — reorders the summation and so is **not** pixel-identical.

## C. Gated or restructured work — small, needs a measurement

**Neither of these can be measured by the suite as it stands**, which is the first thing to fix if
either is taken up. `default` and `exteriors` are clear-weather noon and dawn; `interiors` have no
fog volume dispatched at all. C1 pays only under a heavy overcast and C2 only where lamps light
banked air, so both need a bench view that has one. Adding it is a line in `views.cfg` and a name
in `benches.cfg`, and without it a "no change" result would say nothing.

**C1. Skip the sun shadow ray under a heavy deck.** `gather` traces the sun ray and then
multiplies by `cloudShadow` (`shading.glsl:88-93`). Reorder: evaluate `cloudShadow` first — it is
evaluated unconditionally either way, so the reorder is free — and skip the trace when
`sunCosine * brightest(mSunIrradiance) * cloudShadow` falls under a floor.

**Expect little, and know why before spending the time.** `cloudShadow` is
`exp(-CLOUD_SHADOW_DEPTH * max(alpha - mCover, 0) * mOpacity)`, and it is measured against the
sheet's *own mean* — so half of every sheet returns exactly one by construction, and the floor at
`alpha = 1` is `exp(-4)`, about 0.018 rather than something vanishing. The gate therefore fires
only where a dense texel meets a grazing cosine. Interiors and nights are already gone: `HAS_SUN`
specializes them out. Measure on an overcast view before writing the floor.

**C2. Fog volume: one lamp ray per stretch, not per slice.** `fogvolume.comp:181-185` builds a
fresh reservoir and traces one lamp visibility ray per slice — up to 64 short rays per column.
The sun already answers per stretch (8 rays for 64 slices), weighted by what each slice absorbed.
Give the lamps the same shape: accumulate one reservoir per stretch over its 8 slice positions,
trace once, and scale each slice's share by its own unshadowed lamp light. The 0.9 history and
the jitter already average the coarsening.

**Not pixel-identical, unlike everything applied so far.** It correlates eight slices onto one
shadow answer, so it is a change to the estimator and has to be judged as a picture rather than
checked with `verify`. The pass it moves is `air`, which measures 0.32–0.44 ms in the two
exteriors that dispatch one.

## D. Architecture candidates — larger, research-backed

**D1. Micromap resolve rate — measure before touching.** Opacity micromaps already ship
(`components/rtx/micromap.{hpp,cpp}`, `microtriangles.hpp`, `alphabounds.hpp`, attached in
`sceneacceleration.cpp`), and every cutout instance carries one. What `shot` reports at Seyda
Neen is **3.14% opaque, 3.07% transparent, 93.79% still asking** — by triangle area, so nearly
all of the cutout surface still reaches `RTX_RESOLVE` and pays its fetch.

That may be the honest answer rather than a defect. The bake caps at `sSubdivisionCeiling = 5`
against `sTexelsPerMicrotriangle = 16`, and the header argues both: a finer cut subdivides inside
the compressor's own gradient and resolves nothing. Morrowind's cutouts are nets, grates and
small fronds, where the mask boundary genuinely crosses most triangles.

So measure first. Report the tally per mesh on a canopy-heavy camera and ask whether the
unresolved area concentrates in a few large-triangle meshes — where a finer cut would pay — or
spreads evenly, where it would not. Only then consider level 6 for the meshes that earn it. This
is host measurement work, not shader work.

**D2. Shader Execution Reordering — evaluate, do not assume.** The trace is one übershader in
compute, and NVIDIA's guidance calls that the anti-pattern for divergent shading: ray-query
compute cannot reorder, while an RT pipeline with SER (`VK_NV/EXT_ray_tracing_invocation_reorder`)
buys 11–24% in shipping path tracers and up to 2× in heavy scenes. Against that: this frame's
divergence is modest — water clusters spatially, `variants.glsl` already specializes the frame
kind, and current frame times sit near 7 ms. The trace also reports a register count of 96 across
every variant, which is a healthy occupancy figure and not the picture of a shader starved by its
own state. The port is large (ray queries → pipelines, payload design, SBTs). Order it after C3,
and prototype in the harness first: measure warp occupancy and divergence with Nsight on a
shoreline camera before writing any pipeline code. A cheaper middle step, if the measurement says
shorelines hurt: classify water pixels and shade them in a second small dispatch.

**D3. Shading-map lookup as a texture array.** `paintedLight` (`texturing.glsl:68-84`) does a
hand-rolled wrapping bilinear over an SSBO — four loads plus arithmetic on the hottest sampler
path, per albedo fetch. A `SHADING_EXTENT²` layer per texture in one `r16f` sampler2DArray with
repeat addressing replaces it with one hardware fetch. Host-side change in the RTX-owned loader.

**D4. Underwater column volume — only if it measures.** A submerged frame pays `waterColumn` per
pixel: up to 8 caustic reads and 8 shadow rays each. The froxel argument that moved the fog
applies unchanged. Do not build it until a `bench` under the surface shows the cost — underwater
is rare, but it is full-screen when it happens.

**D5. Lamp presampling (ReGIR-shape) — only if it measures.** The `weighLamps` walk is bounded by
`LAMPS_AT_A_POINT = 256`, and real cells hold tens. If a lamp-dense scene ever measures the walk,
the standard answer is per-cell presampling per frame (a small reservoir per grid cell, refreshed
once), which every shading point then draws from in O(1). Ray Tracing Gems II ch. 23 is the
reference. Not worth the state until a scene asks for it.

## E. What the research says this tree already does right

- Shadow rays terminate on first hit, and translucents never confirm — matches the
  `ACCEPT_FIRST_HIT_AND_END_SEARCH` guidance.
- Specialization constants instead of uniform branches — matches the compile-time ray-flag /
  dead-code guidance for inline ray tracing.
- One ray query live at a time, small state across the query — matches the register-pressure
  guidance for `rayQueryEXT` objects.
- Bindless with `nonuniformEXT` on the access chain, not the argument — matches, and the comment
  in `texturearray.glsl` documents the failure mode the guidance warns about.
- Ray-cone texture LOD in compute (no derivatives) — matches Akenine-Möller's formulation, which
  is the standard answer.
- Demodulated indirect, albedo multiplied back after the filter — matches NRD/SVGF practice.
- Blue noise across the screen with low-discrepancy time sequences, and unbiased RIS with the
  reservoir rule stated once — current best practice for 1-spp direct light.

Sources:
- [Best Practices for Using NVIDIA RTX Ray Tracing (Updated)](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/)
- [Tips and Tricks: Ray Tracing Best Practices](https://developer.nvidia.com/blog/rtx-best-practices/)
- [Khronos: SER — VK_EXT_ray_tracing_invocation_reorder](https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder)
- [NVIDIA SER whitepaper](https://d29g4g2dyqv443.cloudfront.net/sites/default/files/akamai/gameworks/ser-whitepaper.pdf)
- [VK_EXT_opacity_micromap proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_opacity_micromap.html)
- [NVIDIA OMM SDK](https://github.com/NVIDIA-RTX/OMM)
- [NRD — input range and FP16 pipeline](https://github.com/NVIDIA-RTX/NRD)
- [Rendering Many Lights with Grid-Based Reservoirs (RTG II ch. 23)](https://cwyman.org/papers/rtg2-manyLightReGIR.pdf)
- [Khronos: Vulkan Ray Tracing Best Practices for Hybrid Rendering](https://www.khronos.org/blog/vulkan-ray-tracing-best-practices-for-hybrid-rendering)

## F. Ordered plan

Ordered by what a step is worth against what it costs to be sure of: the thing that bounds a
frame first, then exact-equivalence cleanups, then the measurements two gated savings need before
they can be judged, then the hardware features. Each step is one commit-sized change.

**Measure the ceiling before writing the thing.** Gut the function — return a constant from the
top of it — build, and bench. That is the most the real change could ever save, for minutes of
work. It has already ruled out a half-float G-buffer and settled that no sky-disc work is worth
moving for speed, both of which read as obvious wins on the page.

**Expect no time from stating a repeated computation once, and do it anyway.** `glslc -O` inlines
through these small functions and removes some of the repeat itself, so a dedupe of pure maths
measures flat — and it can still shift which contraction the optimizer picks, which shows up in
`verify` as one step of 255 on a handful of pixels. That is a re-baseline, not a regression: take
the new frames as the reference and carry on. A fact used seven times is written once because that
is what this tree does, not because the compiler needed the help.

**A value that has to be read from a buffer is the compiler's blind spot.** It cannot hoist
`sin(someUniform)` out of a per-ray path, so anything derived from a frame constant belongs on the
host, in the field itself — `SkyPatch::mLimb` and `CloudDeck::mBearing` are that.

**How to check a step.** `verify --views=all` against a directory a
previous run wrote is the A/B: it renders all 17 views and prints `same` or the difference, which
settles "no pixel changed" across every path at once. For a path no view covers — rain and snow
sprites — `shot --weather=Rain` before and after does the same job. Take the baseline by rendering
with the *old* SPIR-V still in `build-release/resources`, or by stashing the shader change. Bench
on `build-release` only, twice.

**Bench on a cold machine, and interleave the two builds.** Two runs agree to ±0.01 ms on GPU
`trace` from cold, which is what makes a 0.03 ms difference readable — but after an hour of
building and rendering the same view drifts 16% upward and the exteriors go first. A before/after
pair taken an hour apart says nothing. Keep both SPIR-V builds and alternate them
(`cp` into `build-release/resources/rtx/shaders/`, no rebuild needed), or compare only runs taken
back to back.

1. **A1** — profile the CPU walk at a shoreline. It is 5.7 ms of a 7.2 ms frame, and it outranks
   every GPU item here for frame time.
2. **A bench view under a heavy overcast, and one of lamps in banked air at night**, without which
   C1 and C2 cannot be measured at all.
3. **C1** — cloud-shadow gate before the sun ray, only once step 2 exists.
4. **C2** — fog-volume lamp rays per stretch, likewise, and judged as a picture rather than by
   `verify`.
5. **D1** — per-mesh micromap tally on a canopy camera, to decide whether a finer cut is worth
   anything. Host measurement, no shader change.
6. **D2** — SER: first an Nsight divergence measurement on shoreline and interior cameras. Only
   if the numbers say the übershader loses real occupancy, prototype the pipeline port in the
   harness. Decide on the numbers, not on the guidance alone.
7. **B6, D3–D5** — hold until a measurement names them: the atrous centre read when anything
   runs that pass, the shading-map texture array when the albedo path tops a profile, the
   underwater volume when a submerged `bench` hurts, lamp presampling when a scene outgrows the
   walk.
