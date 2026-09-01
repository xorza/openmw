# RTX shader review — simplifications, deduplications, optimizations

Reviewed: every file under `components/rtxvulkan/shaders/` and the shared headers under
`components/rtx/shaders/`. Date: 2026-09-01, at commit d76a6e59eb; re-reviewed in full at
e8b073bcd2, which added section G — performance bought with small, named quality drops.

The tree is in strong shape. The heavy duplications are already factored: one `RTX_RESOLVE`
candidate loop, one `rayAt`, one lamp record, one falloff, one Henyey-Greenstein, one bloom kernel
pair, and the specialization constants in `variants.glsl` already remove dead paths per frame kind.
The re-review found no new correctness item and no new duplication worth a line.
**This file lists open work and nothing else** — an item applied or ruled out is deleted, not
marked. Each plan step names its verification. The working rule from CLAUDE.md holds: feature
first, numbers second. Section A is what bounds a frame today. Section B changes no pixel.
Sections C, D and G need a measurement.

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

**A2. A storm frame is bounded by the sprite march, and after the emitter hoist it still is.**
`balmora-storm-night`, 1920x1080 traced, `--people=0`: the trace is 4.24 ms, and a build whose
`spritesAlong` returns an empty layer at the top measures 2.44. So **1.87 ms — 43% of the trace —
is the march itself**, on the most expensive frame in the corpus.

What it is marching over is one emitter of 2,556 drops binned into 16-pixel tiles, so a pixel walks
a long tile list and pays a quad intersection, an `alpha` fold and one bindless `textureLod` for
each entry. Everything an emitter can answer once is already hoisted — the fog's field, the
orientation, the texture extent, and now the lamp sum.

**What is left is per sprite by nature, so the next question is how many sprites reach a pixel at
all rather than what each costs.** `SPRITE_TILE` is the only dial on that, and a coarser tile makes
the lists longer. Note also that gutting the `textureLod` to a constant is *not* a probe of it: the
alpha it returns is what rejects most of the list, so a constant alpha measures 6.50 ms — half again
the real build.

## B. Cleanups that change no pixel

**B6. `atrous` loads the centre texel up to three times.** `atrous.comp:104,128,137`. Read the
centre once and use it for both the luminance and the fallback.

**Worth almost nothing, and the bench says why: the cascade does not run in the shipping
configuration.** With DLSS Ray Reconstruction on — the default — the upscaler denoises for itself
and `accumulate`/`atrous` are not dispatched at all. No `bench` run lists either among its GPU
timings. So this is unmeasurable except under `--upscale=off`, and it belongs with whatever else
touches that path rather than on its own.

Note also that the fuller form — seeding the sums with the centre and skipping `(0, 0)` in the
loop — reorders the summation and so is **not** pixel-identical.

## C. Gated or restructured work — small, needs a measurement

**C2 is measured on `balmora-fog-night`**, in the `skies` suite, which is what a view fixing its own
`weather` is for. The suites either side of it say nothing about it — `default` and `exteriors` are
clear-weather noon and dawn, and `interiors` dispatch no fog volume at all.

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

**One number this file has already produced for it, and it says which rays a rate can save.**
`AMBIENT_EXTERIOR_RATE` draws half the exterior ambient rays and claimed 2.5 ms of a 4.6 ms
ceiling — 54%. `INDIRECT_LIGHT_RATE` draws half the bounce hits' sun and lamp rays and claims 0.13
of 0.71 — 18%. The difference is how long the dropped ray is. The ambient ray runs to `mFar` and is
nearly all empty traversal, so halving those halves the work the RT cores do whatever the warp is
doing; a shadow ray to a lamp in the same room ends almost at once, and there the warp runs on until
the lanes that kept their rays are finished. **A rate on a short ray buys a fifth of its gut, and a
rate on a long one buys half.** That is the divergence D2 is about, measured rather than argued.

**And a second number, which says the shape of the shader costs more than the work in it.** Moving
the sprites' lamp walk out of the per-sprite path and into the per-emitter block took
`balmora-storm-night`'s trace from 6.07 ms to 4.24. Deleting the same walk outright — no lamp ever
summed for a sprite, strictly less work than the hoist does — measured only 5.38. A shader that
carries a nested walk on a hot path is slower than the same shader doing that walk somewhere colder,
by three times what the walk itself costs. Whatever the mechanism, it is the übershader's structure
answering, and it is the strongest argument on this page for reading D2's numbers before dismissing
the port.

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

## G. Performance bought with small, named quality drops

**Where the rays go on a day exterior, so the items below have a denominator.** A near solid pixel
costs about five and a half rays: the primary, the eye hit's sun and lamp rays, the bounce ray, half
an ambient ray (`AMBIENT_EXTERIOR_RATE`) and half of the bounce hit's own sun and lamp pair
(`INDIRECT_LIGHT_RATE`). Past a cell it costs two — the bounce is not traced at all
(`BOUNCE_REACH`). A water pixel roughly doubles the near figure. None of these items is
pixel-identical — that is what they are — so `verify` is the wrong tool for all of them: bench
interleaved, and judge each as a picture. Delete an item when it is applied or ruled out, like
everything else here.

- [ ] **G2. Water: shade one far end per frame, drawn by Fresnel.** `shadeWater` traces a
  reflection and a refraction and *shades both* — each far hit pays a sun ray, a lamp ray and an
  ambient ray. The mix is `(1 - f) * refracted + f * reflected`, so shading only the side a draw
  picks (reflection with probability `f`) and using it unweighted is the same number in
  expectation, and about three rays cheaper per water pixel. Keep both *traces*: the reflection's
  hit feeds `WaterMirror` and the reflection motion vector, and the refraction's distance feeds the
  absorption — what is rated is only the far-end shading. The cost is noise that alternates between
  the specular and diffuse halves the upscaler splits by; judge on moving water at dusk, where the
  Fresnel mid-band is widest.

- [ ] **G3. Turn `AMBIENT_EXTERIOR_RATE` down a step.** The constant's own comment carries the
  measurement: removing the exterior ambient ray outright is worth 4.6 ms across the exteriors
  suite, and the current half rate has claimed 2.5 of it, leaving 2.1. A third instead of a half
  claims about 0.7 ms more, unbiased, for proportionally more variance in a term the filter already
  carries. One number, one bench, one picture check in the guild doorway where the fill matters
  most.

- [ ] **G4. The fog volume's two dials, after C2.** C2 (one lamp ray per stretch, not per slice)
  is the no-drop half and goes first. What remains is `FOG_SHADOW_RAYS` — the ported renderer's
  own table says four rays measure 0.0087 against eight's 0.0048, versus a ray-per-step reference
  — and `FOG_VOLUME_SCALE`, where a coarser column leans harder on the jitter-plus-history that
  already hides the grid. The whole pass is 0.32–0.44 ms today, so re-measure after C2 and only
  touch the dials if the pass still shows in the frame.

- [ ] **G6. Bake composites nearer (host dial, named here for completeness).** A near terrain hit
  shades its whole layer stack — a mask read and a texture fetch per layer, four or five deep —
  where a distant chunk reads one baked composite. `Rtx::sCompositeFrom` is the crossover. Moving
  it nearer trades mid-ground tiling sharpness for one fetch per hit; the right first step is a
  profile share for the layered path on a terrain-heavy camera, since D1's tally machinery already
  reports per-mesh numbers.

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

**Count how often a gate fires before believing the ceiling applies to it.** A gate on the sun's
shadow ray under a heavy deck was written and thrown away: the whole ray is worth 0.51 ms of the
overcast ship's 4.10, but `cloudShadow` is measured against the sheet's own mean, so half of every
sheet returns exactly one and the floor at an opaque texel is only `exp(-4)`. The gate fired on
**nine pixels of half a million** under the sky it was written for, measured flat, and left a branch
on the hottest path in the tree. A gut says what a change could save. It does not say how much of
the frame the change reaches.

**And a gut is only a probe where the constant it returns cannot change what runs after it.**
Replacing the sprite texel with a constant alpha measures the march at 6.50 ms against a real
4.31 — because the alpha is what rejects most of a tile's list, and a constant one lets every sprite
through.

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
2. **A2** — the sprite march is 1.87 ms of `balmora-storm-night`'s 4.24 ms trace, on the worst
   frame the corpus holds. Everything an emitter can answer once already is, so the question left
   is how many sprites reach a pixel rather than what each costs.
3. **C2** — fog-volume lamp rays per stretch, on `balmora-fog-night`, and judged as a picture
   rather than by `verify`. Then **G4**, re-measured on what C2 left.
4. **G2** — water's one-shaded-path draw, judged on moving water at dusk.
5. **G3** — the ambient rate step, one number against the measurement already in the constant's
   comment.
6. **D1** — per-mesh micromap tally on a canopy camera, to decide whether a finer cut is worth
   anything. Host measurement, no shader change.
7. **D2** — SER: first an Nsight divergence measurement on shoreline and interior cameras. Only
   if the numbers say the übershader loses real occupancy, prototype the pipeline port in the
   harness. Decide on the numbers, not on the guidance alone. Two measurements now argue for it
   rather than one — see the note under D2.
8. **B6, D3–D5, G6** — hold until a measurement names them: the atrous centre read when anything
   runs that pass, the shading-map texture array when the albedo path tops a profile, the
   underwater volume when a submerged `bench` hurts, lamp presampling when a scene outgrows the
   walk, and the composite crossover when the layered path shows in a profile.
