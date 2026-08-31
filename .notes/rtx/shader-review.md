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

**B1. The shadow-ray candidate loop loads each instance and material twice.** In `RTX_RESOLVE`
with `seeThrough=true` (`traversal.glsl:187-195`), `candidateIsSeenThrough` loads `instances[i]`
and `materials[m]`, and then `candidateTransmittance` or `alphaPasses` loads both again. This is
the sun-through-foliage path, the hottest candidate loop in the frame, and the micromap tally in
D1 says 93.79% of cutout area still reaches it. Merge the three helpers into one candidate resolve
that loads instance and material once and returns either "passed", "blocked", or a transmittance.
The eye path (`seeThrough=false`) folds as before.

**B2. The shore probe resolves a full material for a distance-only answer.** `water.glsl:263-265`
traces straight down, and only `bed.mDistance` is read. `trace()` resolves the bed's albedo,
which for terrain is a layer loop with mask reads. Two fixes compose:
- Clamp the probe's `tmax` to `WATER_SHORE_FADE + WATER_BIAS`. The smoothstep saturates at
  `WATER_SHORE_FADE`, so a miss on the short ray gives `shore = 1.0` exactly. The traversal
  becomes near-free for every pixel of open water. Zero picture change, by construction.
- Add a distance-only trace variant (traversal plus cutout test, no material resolve) and use it
  here. The bed trace in `visibility.comp:167` must keep the full resolve — it shades.

**B3. Per-sprite work that is per-emitter.** In `spritesAlong`:
- `textureSize(textures[emitter.mTexture], 0)` runs per sprite (`sprites.glsl:358`). Hoist it into
  the `held` block beside `oriented` and `width`.
- The self-shade mean — `textureQueryLevels` plus the coarsest-level fetch
  (`sprites.glsl:492-494`) — depends only on the emitter. Hoist it into the `held` block.
- `henyeyGreenstein(SMOKE_ANISOTROPY, dot(toSun, direction)) / INV_FOUR_PI`
  (`sprites.glsl:463`) is constant per pixel. Hoist it above the loop.

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
centre once, seed the sums with it, and skip `(0,0)` in the loop. Micro, but free.

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

**C4. Half-float the filter's images.** `GBUFFER_RADIANCE` and `GBUFFER_GUIDE` are rgba32f
(`gbuffer.h:38,40`). The wavelet reads 25 taps of source, guide, and depth per pixel per level,
five levels deep — this is the bandwidth-bound part of the frame. NRD runs its whole pipeline in
FP16 and asks for HDR inputs in [0, 250] so that x² fits the format. The same argument holds
here: the indirect channel is demodulated radiance, the accumulator's outlier clamp already
bounds settled pixels, and the separate rgba32f `sum` image keeps the reference path exact.
Convert `indirect`, the accumulator's colour/moments history, and the atrous ping-pong to
rgba16f. Keep `direct` fp32 for now — the sun's disc rides in it at 1000. Guide can go rgba16f
outright, or octahedral-normal rg16f later. Verify with `shot` image diffs day and night, then
`bench`.

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

1. **B1** — single-load candidate resolve in `RTX_RESOLVE`. Verify: `shot` on a foliage camera,
   identical hit fraction and image.
2. **B2** — shore probe: clamp `tmax` to the fade band, add the distance-only trace. Verify:
   `shot` on a shoreline camera, identical image, note the frame-time delta.
3. **B3** — sprite loop hoists. Verify: `shot` through rain and a campfire.
4. **B5 + B6** — `coneLod` split and the atrous centre read. Verify: `shot` on terrain.
5. **B4** — host-computed moon/patch constants (`MoonDisc`/`SkyPatch` gain three fields, the
   shader loses the transcendentals). Verify: `shot` at night, both moons up.
6. **C1** — cloud-shadow gate before the sun ray. Verify: overcast `shot` A/B under display
   quantization, then `bench` in a storm.
7. **C3** — workgroup swizzle and 16×8 experiment. Verify: `bench` exteriors, keep only if it
   wins.
8. **C2** — fog-volume lamp rays per stretch. Verify: `view` of lantern fog at night, `bench`.
9. **C4** — fp16 for indirect, histories, atrous ping-pong, guide. Verify: `shot` diffs day,
   night, underwater; then `bench`. Keep `direct` and `sum` fp32.
10. **D1** — per-mesh micromap tally on a canopy camera, to decide whether a finer cut is worth
    anything. Host measurement, no shader change.
11. **D2** — SER: first an Nsight divergence measurement on shoreline and interior cameras. Only
    if the numbers say the übershader loses real occupancy, prototype the pipeline port in the
    harness. Decide on the numbers, not on the guidance alone.
12. **D3–D5** — hold until a measurement names them: shading-map texture array when the albedo
    path tops a profile, underwater volume when a submerged `bench` hurts, lamp presampling when
    a scene outgrows the walk.
