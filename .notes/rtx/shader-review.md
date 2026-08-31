# RTX shader review — simplifications, deduplications, optimizations

Reviewed: every file under `components/rtxvulkan/shaders/` and the shared headers under
`components/rtx/shaders/`. Date: 2026-09-01, at commit d76a6e59eb.

The tree is in strong shape. The heavy duplications are already factored: one `RTX_RESOLVE`
candidate loop, one `rayAt`, one lamp record, one falloff, one Henyey-Greenstein, one bloom kernel
pair, and the specialization constants in `variants.glsl` already remove dead paths per frame kind.
The findings below are what is left. **An item is deleted from this file once it is applied or
rejected**, so what stands here is the open list and nothing else. Each plan step names its
verification. The working rule from CLAUDE.md holds: feature first, numbers second. Section B
changes no pixel and needs no bench. Sections C and D need a measurement.

## B. Duplicate loads and hoists — no pixel changes

**B4. Per-frame sky constants recomputed per ray.** `moonFace` derives, per ray inside a moon's
cone: `limb = sin(mAngularRadius)`, the sun's turn (`atan` plus `sin`/`cos` pair), the light
vector, and McEwen's phase polynomial (`sky.glsl:258-293`). All are functions of frame constants
only. Compute them on the host, and carry them in `MoonDisc` (`visibility.h`) as `mLimb`,
`mLightInFace`, `mLunarBlend`. `skyPatches` wants the same `mLimb` treatment
(`sky.glsl:219`). Small win — the miss path is cheap — but it is a pure consolidation and it
shortens the shader.

**B5. Shared terms in the terrain layer loop.** Each layer's `coneLod` recomputes `worldArea`,
`facing`, and `log2(coneWidth)` (`texturing.glsl:25-43`, called from `traversal.glsl:394`). Split
`coneLod` so the crossed/direction/coneWidth half is computed once per hit and only the
texel-area half runs per layer.

**B6. `atrous` loads the centre texel up to three times.** `atrous.comp:104,131,137`. Read the
centre once and use it for both the luminance and the fallback.

**Worth almost nothing, and the bench says why: the cascade does not run in the shipping
configuration.** With DLSS Ray Reconstruction on — the default — the upscaler denoises for itself
and `accumulate`/`atrous` are not dispatched at all. No `bench` run lists either among its GPU
timings. So this is unmeasurable except under `--upscale=off`, and it belongs with whatever else
touches that path rather than on its own.

Note also that the fuller form — seeding the sums with the centre and skipping `(0, 0)` in the
loop — reorders the summation and so is **not** pixel-identical, unlike the rest of section B.

## C. Gated or restructured work — small, needs a measurement

**C1. Skip the sun shadow ray under a heavy deck.** `gather` traces the sun ray and then
multiplies by `cloudShadow` (`shading.glsl:88-93`). Reorder: evaluate `cloudShadow` first and
skip the trace when `sunCosine * brightest(mSunIrradiance) * cloudShadow` falls under a floor of
about 1/4096 of the frame's scale. In overcast and storm weather this removes most sun rays.
The floor is a stated bias, the same shape as `FOG_SHAFT_FLOOR`. Verify with `shot` A/B on an
overcast camera: the pixel difference must sit under the display's quantization.

**C2. Fog volume: one lamp ray per stretch, not per slice.** `fogvolume.comp:181-185` builds a
fresh reservoir and traces one lamp visibility ray per slice — up to 64 short rays per column.
The sun already answers per stretch (8 rays for 64 slices), weighted by what each slice absorbed.
Give the lamps the same shape: accumulate one reservoir per stretch over its 8 slice positions,
trace once, and scale each slice's share by its own unshadowed lamp light. The 0.9 history and
the jitter already average the coarsening. Verify with `view` over a lantern in fog at night,
then `bench`.

**C3. Workgroup swizzle for the trace.** `visibility.comp` dispatches 8×8 groups in row-major
order. NVIDIA measures about 8% on ray-heavy compute from a Morton-style group reordering,
because neighbouring groups then share BVH and texture cache lines. This is a dispatch-side
remap (or a `gl_WorkGroupID` swizzle in the shader) with zero picture change. Try 16×8 as the
group size in the same experiment — the same guidance names it. Measure with `bench` on the
exteriors suite.

**C4. Half-float the G-buffer's wide channels.** `GBUFFER_RADIANCE` and `GBUFFER_GUIDE` are rgba32f
(`gbuffer.h:38,40`). NRD runs its whole pipeline in FP16 and asks for HDR inputs in [0, 250] so
that x² fits the format; the same argument holds here, and the separate rgba32f `sum` image keeps
the reference path exact.

**But the wavelet is not the reason any more.** It was written as one, and the bench says
`accumulate`/`atrous` never run under Ray Reconstruction (see B6). What is left is the traffic the
trace itself pays writing eight full-frame channels, and what the upscaler pays reading them —
which is real, since `upscale` is the largest GPU item in three of the four benched views.

So the target is the write side: `guide` to rgba16f (a normal and a roughness need nothing like
24 bits of mantissa), and `indirect` with it. Keep `direct` fp32 — the sun's disc rides in it at
1000. Verify with `verify --views=all`, expecting *small* differences rather than none, then
`bench` and watch `upscale` as well as `trace`.

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

Order: safe deletions first, then exact-equivalence changes, then gated savings, then measured
format work, then the two hardware features. Each step is one commit-sized change.

**The method that worked, for the steps below.** `verify --views=all` against a directory a
previous run wrote is the A/B: it renders all 17 views and prints `same` or the difference, which
settles "no pixel changed" across every path at once. For a path no view covers — rain and snow
sprites — `shot --weather=Rain` before and after does the same job. Take the baseline by rendering
with the *old* SPIR-V still in `build-release/resources`, or by stashing the shader change. Bench
on `build-release` only, twice: two runs agree to ±0.01 ms on GPU `trace`, so anything above 0.03
is real.

1. **B4** — host-computed moon/patch constants (`MoonDisc`/`SkyPatch` gain three fields, the
   shader loses the transcendentals). Verify: `verify`, then `bench` at dawn.
2. **B5** — `coneLod` split, hoisting `worldArea` and `facing` out of the terrain layer loop while
   leaving the final expression spelled exactly as it is, so the mip level cannot move.
3. **C1** — cloud-shadow gate before the sun ray. Verify: overcast `shot` A/B under display
   quantization, then `bench` in a storm.
4. **C3** — workgroup swizzle and 16×8 experiment. Verify: `bench` exteriors, keep only if it
   wins.
5. **C2** — fog-volume lamp rays per stretch. Verify: `view` of lantern fog at night, `bench`.
6. **C4** — rgba16f for `guide` and `indirect`. Verify: `verify` expecting small differences, then
   `bench` watching `upscale` as well as `trace`.
7. **D1** — per-mesh micromap tally on a canopy camera, to decide whether a finer cut is worth
   anything. Host measurement, no shader change.
8. **D2** — SER: first an Nsight divergence measurement on shoreline and interior cameras. Only
   if the numbers say the übershader loses real occupancy, prototype the pipeline port in the
   harness. Decide on the numbers, not on the guidance alone.
9. **B6, D3–D5** — hold until a measurement names them: the atrous centre read when anything
   runs that pass, the shading-map texture array when the albedo path tops a profile, the
   underwater volume when a submerged `bench` hurts, lamp presampling when a scene outgrows the
   walk.
