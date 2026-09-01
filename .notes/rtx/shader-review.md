# RTX shader review — simplifications, deduplications, optimizations

Reviewed: every file under `components/rtxvulkan/shaders/` and the shared headers under
`components/rtx/shaders/`. Date: 2026-09-01, at commit d76a6e59eb; re-reviewed in full at
e8b073bcd2, and again after the sprite binning was fixed.

The tree is in strong shape. The heavy duplications are already factored: one `RTX_RESOLVE`
candidate loop, one `rayAt`, one lamp record, one falloff, one Henyey-Greenstein, one bloom kernel
pair, and the specialization constants in `variants.glsl` already remove dead paths per frame kind.
No re-review has found a correctness item or a duplication worth a line.
**This file lists open work and nothing else** — an item applied or ruled out is deleted, not
marked. Each plan step names its verification. The working rule from CLAUDE.md holds: feature
first, numbers second. Section A is what bounds a frame today. Section B changes no pixel.
Sections D and G need a measurement.

## A. What bounds a frame today

**A1. The exteriors are CPU-bound, and Vivec is where that is not close.** Measured over the
`exteriors` suite at 1920x1080, quality upscale, everything on. The GPU costs about 4 ms at every
one of the seven; what separates them is the host.

| view | frame | wait on GPU | walk | place |
|---|---|---|---|---|
| vivec | 12.43 | **0.00** | 5.42 | 1.55 |
| seyda-neen-ship | 6.01 | 2.36 | 2.61 | 0.68 |
| balmora | 5.38 | 1.10 | 3.28 | 0.74 |
| seyda-neen-shore | 5.34 | 1.93 | 2.68 | 0.45 |
| ald-ruhn | 5.29 | 0.81 | 3.16 | 0.92 |
| sadrith-mora | 5.13 | 0.89 | 2.87 | 0.95 |
| dagon-fel | 4.94 | 3.02 | 1.23 | 0.33 |

**Vivec never waits on the device.** Six of the seven have a millisecond or more of wait in them
and would take a GPU saving; Vivec would take none, and it is twice the frame of any of the others.
So it is the camera a CPU change is measured at, and the shoreline — where the doc used to take
this number — is the wrong one.

**Two costs, and neither is cell loading.** Crossings are counted separately by the bench.

**A1a. The walk is 27% of Vivec's CPU, and most of it concludes that nothing moved.**
`MirrorTraversal::apply(osg::Transform&)` carries 27.5% of the profile, with `osg::Group::traverse`
at 7.8% self, `RigGeometry::cull` at 5.8%, `addDrawable` at 2.6% and `resolveMesh` at 2.1%. Holding
the residents still (`--people=0`) takes the walk from 5.42 ms to 3.28.

**The incremental design already works at the level it was built for**, which is content:
`ExtractionStats::mMeshesReused` counts the per-drawable lookups succeeding, and nothing is
re-added. What has no fast path is one level up — the walk descends the whole graph and asks per
drawable, where for a static subtree the answer is "unchanged" for the whole subtree at once. The
pose numbers the extractor already keeps (`sceneextractor.hpp`, "the rule that they only ever go
up") are the place to hang that.

**The obstacle is the sweep and not the walk.** `retire` drops everything the epoch did not stamp,
so a subtree skipped wholesale is a subtree retired wholesale. Whatever skips a subtree has to
stamp its placements at the same time, and `mPlacementsReached` has to agree — that is the design,
and it is what makes this a commit of its own rather than an early return.

**A1b. The emitters cost a third of Vivec's CPU on the host, and 24 of them are the whole of it.**
Not the trace and not the walk: `SpriteShade::shade` carries 26.4% of the profile and
`SpriteTiles::rebuild` another 9.0% self, for 24 emitters holding 2,328 live particles.

`SpriteShade` splats one antialiased disc per sprite per light into a 32×32 grid — 4,656 discs a
frame — and `sLargestInCells = 8` puts a typical disc at about 250 cells, so the frame lays down
over a million cells. The square root is already off the inside of the disc (`layDown` says so),
which took Vivec's frame from 13.5 ms to 11.7 and left the scattered adds themselves as the cost.
What is left to try, in order of how much it gives up: a coarser `sLargestInCells`, which is
straight quality; laying a run's discs down as spans rather than cells; and asking whether a
column's layer count has to be recomputed from nothing every frame at all.

`SpriteTiles::rebuild` is the offsets array, which is one entry per tile whatever the sprites do —
8,160 of them cleared, prefix-summed and written to the device every frame — plus the scatter that
fills the runs. `SPRITE_TILE` is the dial and it is measured: see `scene.h`.

**For shader work in the meantime, bench with `--people=0`.** Its own help says it is "what a
profiling run should hold still".

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

## D. Architecture candidates — larger, research-backed

**D2. Shader Execution Reordering — evaluate, do not assume.** The trace is one übershader in
compute, and NVIDIA's guidance calls that the anti-pattern for divergent shading: ray-query
compute cannot reorder, while an RT pipeline with SER (`VK_NV/EXT_ray_tracing_invocation_reorder`)
buys 11–24% in shipping path tracers and up to 2× in heavy scenes. Against that: this frame's
divergence is modest — water clusters spatially, `variants.glsl` already specializes the frame
kind, and current frame times sit near 5 ms out of doors. The trace also reports a register count
of 96 across every variant, which is a healthy occupancy figure and not the picture of a shader
starved by its own state. The port is large (ray queries → pipelines, payload design, SBTs).
Prototype in the harness first: measure warp occupancy and divergence with Nsight on a shoreline
camera before writing any pipeline code. A cheaper middle step, if the measurement says shorelines
hurt: classify water pixels and shade them in a second small dispatch.

**Three measurements now argue for reading its numbers rather than dismissing the port.**

*One says which rays a rate can save.* `AMBIENT_EXTERIOR_RATE` draws half the exterior ambient rays
and claimed 2.5 ms of a 4.6 ms ceiling — 54%. `INDIRECT_LIGHT_RATE` draws half the bounce hits' sun
and lamp rays and claims 0.13 of 0.71 — 18%. The difference is how long the dropped ray is. The
ambient ray runs to `mFar` and is nearly all empty traversal, so halving those halves the work the
RT cores do whatever the warp is doing; a shadow ray to a lamp in the same room ends almost at
once, and there the warp runs on until the lanes that kept their rays are finished.

*The second says the shape of the shader costs more than the work in it.* Moving the sprites' lamp
walk out of the per-sprite path and into the per-emitter block took `balmora-storm-night`'s trace
from 6.07 ms to 4.24. Deleting the same walk outright — no lamp ever summed for a sprite, strictly
less work than the hoist does — measured only 5.38. A shader that carries a nested walk on a hot
path is slower than the same shader doing that walk somewhere colder, by three times what the walk
itself costs.

*The third says a rate stops paying once every warp still has a lane that kept its ray.* See the
warp note in section F: two separate attempts at buying rays with a draw have now measured nothing
at all against ceilings of 2.1 ms and 1.0 ms.

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

**Read the warp note in section F before writing one of these.** An item whose saving is a per-lane
draw between two branches has already measured nothing twice, and neither attempt was cheap to
undo.

- [ ] **G4. The fog volume's two dials.** The lamp rays are per stretch now, which took the `air`
  pass from 0.32 ms to 0.24 and dropped the estimator's variance to nothing. What remains is
  `FOG_SHADOW_RAYS` — the ported renderer's own table says four rays measure 0.0087 against eight's
  0.0048, versus a ray-per-step reference — and `FOG_VOLUME_SCALE`, where a coarser column leans
  harder on the jitter-plus-history that already hides the grid. The whole pass is under a quarter
  of a millisecond, so it has to show in a frame before either dial is worth turning. Both are a
  whole-warp saving rather than a per-lane one, which is what makes them worth trying at all.

- [ ] **G6. Bake composites nearer (host dial, named here for completeness).** A near terrain hit
  shades its whole layer stack — a mask read and a texture fetch per layer, four or five deep —
  where a distant chunk reads one baked composite. `Rtx::sCompositeFrom` is the crossover. Moving
  it nearer trades mid-ground tiling sharpness for one fetch per hit; the right first step is a
  profile share for the layered path on a terrain-heavy camera.

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
- [NRD — input range and FP16 pipeline](https://github.com/NVIDIA-RTX/NRD)
- [Rendering Many Lights with Grid-Based Reservoirs (RTG II ch. 23)](https://cwyman.org/papers/rtg2-manyLightReGIR.pdf)
- [Khronos: Vulkan Ray Tracing Best Practices for Hybrid Rendering](https://www.khronos.org/blog/vulkan-ray-tracing-best-practices-for-hybrid-rendering)

## F. Ordered plan

Ordered by what a step is worth against what it costs to be sure of: the thing that bounds a
frame first, then exact-equivalence cleanups, then the hardware features. Each step is one
commit-sized change.

**Measure the ceiling before writing the thing.** Gut the function — return a constant from the
top of it — build, and bench. That is the most the real change could ever save, for minutes of
work. It has already ruled out a half-float G-buffer and settled that no sky-disc work is worth
moving for speed, both of which read as obvious wins on the page.

**A saving a warp does not agree on is not a saving.** Thirty-two lanes run one instruction stream,
so work skipped by a per-lane coin flip is still done by the warp whenever any lane keeps it — and
at a rate of a half or a third, every warp keeps it. Two items were written, measured interleaved
and thrown away on exactly this: taking `AMBIENT_EXTERIOR_RATE` from a half to a third came back
0.09 to 0.12 ms *slower* at three of seven exteriors and flat at the other four, against a 2.1 ms
ceiling; and shading one water far end per pixel drawn by Fresnel measured 2.79 ms of trace against
2.78 over five interleaved rounds, against a 1.02 ms ceiling the gut had shown. Before writing one
of these, ask what fraction of warps would skip *every* lane. A saving whose branch is decided by
the geometry — a whole emitter rejected, a whole tile empty, a whole stretch answered once — is a
different thing and does pay: the sprite capsule bound took the storm's trace from 4.09 ms to 2.59
that way.

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

**How to check a step.** `verify --views=all` against a directory a previous run wrote is the A/B:
it renders all 17 views and prints `same` or the difference, which settles "no pixel changed" across
every path at once. For a path no view covers — rain and snow sprites — `shot --weather=Rain`
before and after does the same job. Take the baseline by rendering with the *old* SPIR-V still in
`build-release/resources`, or by stashing the shader change. Bench on `build-release` only, twice.

**Bench on a cold machine, and interleave the two builds.** Two runs agree to ±0.01 ms on GPU
`trace` from cold, which is what makes a 0.03 ms difference readable — but after an hour of
building and rendering the same view drifts 16% upward and the exteriors go first. A before/after
pair taken an hour apart says nothing. Keep both SPIR-V builds and alternate them
(`cp` into `build-release/resources/rtx/shaders/`, no rebuild needed), or compare only runs taken
back to back. Where a change is host-side as well, keep two executables in `build-release` itself —
the harness reads `defaults.bin` from beside itself and will not run from anywhere else.

1. **A1b** — the emitters' host work at Vivec, which is a third of the CPU of the only exterior in
   the corpus that never waits on the device. The disc splat first, then `SpriteTiles`' per-tile
   arrays.
2. **A1a** — the subtree fast path in the mirror walk, with the sweep's stamping solved alongside
   it. 5.4 ms of Vivec's 12.4, and the largest single item on this page.
3. **G4** — the fog volume's two remaining dials, but only once the pass shows in a frame.
4. **D2** — SER: first an Nsight divergence measurement on shoreline and interior cameras. Only
   if the numbers say the übershader loses real occupancy, prototype the pipeline port in the
   harness. Decide on the numbers, not on the guidance alone.
5. **B6, D3–D5, G6** — hold until a measurement names them: the atrous centre read when anything
   runs that pass, the shading-map texture array when the albedo path tops a profile, the
   underwater volume when a submerged `bench` hurts, lamp presampling when a scene outgrows the
   walk, and the composite crossover when the layered path shows in a profile.
