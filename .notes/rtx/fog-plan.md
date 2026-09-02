# Fog volume: scatter per froxel, and lamps that do not boil

Two complaints, one fix. `shader-review.md` §7 asks whether the one-thread-per-column shape of
`fogvolume.comp` is worth splitting. The night fog in Balmora boils under the street lamps. The
split is what the noise fix needs, and the noise fix is what makes the split worth doing.

## 1. Findings

### 1.1 The air pass is cheap as it stands

Measured with `build-release/openmw-rtxtool shot --upscale=off --repeat=32`, 1920×1080, GPU zone
medians:

| view                   | air (ms) | trace (ms) | frame (ms) |
|------------------------|---------:|-----------:|-----------:|
| balmora-fog-night      |    0.245 |      2.367 |      5.930 |
| balmora (noon, Clear)  |    0.232 |      2.295 |      5.836 |
| seyda-neen-ship-dawn   |    0.380 |      4.456 |      7.300 |

The review's own threshold is "a few tenths of a millisecond, leave it". On cost alone the split
is not worth a change. The debug build with the validation layers on reads 2.3 ms for the same
pass, which is the number that made the item look large.

### 1.2 The lamp-lit fog is the noisy part of the frame

One raw frame with no history (`--upscale=off --repeat=1`) against a 200-frame reference
(`--accumulate=200`), same view, linear radiance from `--dump`, luminance per pixel:

| pixels by reference luminance | count   | relative RMS error | p90 of relative error |
|-------------------------------|--------:|-------------------:|----------------------:|
| 0.001 – 0.005 (dark ground)   | 736,384 |              0.176 |                 0.289 |
| 0.005 – 0.020 (fog, sky)      | 996,535 |              0.058 |                 0.071 |
| 0.020 – 0.100 (lamp-lit fog)  |   9,297 |              0.474 |                 0.722 |
| sky band, rows 20–120         |  110,000|              0.036 |                       |

A crop around any lamp in the two frames shows the shape of the error. The glow around a lamp is
a field of 8×8-pixel blocks, and each block is either fully lit or fully dark. The reference is
smooth. To take the pair again: `shot --view=balmora-fog-night --upscale=off --exposure=8
--repeat=1 --dump=raw.f32`, then the same with `--accumulate=200 --dump=ref.f32`, four floats a
pixel at 1920×1080, and a numpy diff of the luminance.

### 1.3 Where the noise comes from, in order of size

**1. The visibility is asked at a point that is not where the light is.** A column is cut into
eight stretches of eight slices. One probe per stretch, at a jittered point anywhere along it,
weighs the lamps that reach *the probe*, traces one ray to one of them, and the result multiplies
the sum of every lamp at every slice of the stretch. At 500 units from the eye the stretch runs
from 469 to 1,875 units, which is 1,406 units, against a lamp reach of 128 to 512. So:

- When the probe lands outside every lamp's reach, `lampVisible` returns one and every lamp in
  the stretch is lit, occluded or not.
- When the probe lands under a different lamp's occluder, all eight slices go dark.
- The odds of the coin depend on where the probe fell, not on the lamp the slice is lit by.

The shader's comment defends this with "whether a lamp is seen changes slowly along a ray". Over
1,406 units past a lamp post, a doorway and a bridge parapet, it does not. The estimate is a
Bernoulli draw of the full lamp term. Its per-frame standard deviation is `S·sqrt(p(1-p))`, up to
half the lamp term. The history at 0.9 leaves `sqrt(0.1 / 1.9) = 0.23` of that, which is the
block pattern in the crops.

**2. The lamp's irradiance is point-sampled inside a froxel that is 29 to 1 long.** At 500 units
a froxel is 4.3 × 4.3 × 125 units. `lampsAt(position)` reads the inverse square at one jittered
point of those 125 units, so a lamp 50 units off the ray reads 2.5 times brighter at one end of
the froxel than at the other. The uniform path already integrates this in closed form
(`falloffAlong`). The volume samples it.

**3. One `alongJitter` for all 64 slices of a column.** The whole column moves together, so a
column's error is coherent along its depth and the blue noise across columns is the only
decorrelation.

**4. One thread per column** is latency-bound, and irrelevant at 0.24 ms. It matters because a
ray per froxel in a per-column thread is 64 serial rays per thread.

### 1.4 What is not the cause

- **The reservoir's choice of lamp.** With the selection weights equal to the shares in the sum,
  `S · V_c` with `P(c = j) = s_j / S` has expectation `Σ s_j V_j`, which is exact, and the only
  variance left is the visibility's own. The current code is biased and noisy because the weights
  are taken at the probe and the sum at the slice, two different points.
- **The history weight.** 0.9 is what UE ships and near what Frostbite ships. It cannot average
  away a coin that flips the whole term.
- **The blue noise and the R2 turn.** Sound, per the review.

### 1.5 A coverage gap

No test runs the volume with a lamp or with a sun ray. `litThroughFog` sets `mFogUniform = 1`,
so `aLampLightsTheAirItStandsIn…` and `aLidOverTheMarch…` exercise the closed form. The volume is
reached only by `theBankedFieldHoldsAsMuchAirAsAnEvenOne` and `theWindCarriesTheField…`, and
neither has a light in it. The estimator in §1.3 was never asserted on.

## 2. What the field does

- **Wronski 2014, Hillaire 2015.** Scatter per froxel over the whole grid, then integrate front to
  back per column. Jitter the sample inside its froxel and average with the reprojected history.
  Local lights are evaluated per froxel from their own falloff and a shadow map, so each froxel
  carries its own answer and nothing is shared down the column.
- **The Last of Us Part II** (SIGGRAPH 2020). Same grid. Volumetric shadows are made soft on
  purpose, so a shadow edge in the air is a gradient wider than the grid's Nyquist.
- **RTX Remix**, the ray-traced froxel volume in production: a grid at 1/16 of the frame with 48
  slices, RIS over 8 candidate lights per froxel, **one visibility ray per froxel**, temporal
  reuse of the reservoir, a spatial Gaussian filter of radius 2 to 4 froxels (σ 1.2), a firefly
  clamp, and accumulation up to 254 frames. Every one of those is a per-froxel quantity.
- **Volumetric ReSTIR** (Lin, Wyman, Yuksel 2021) reuses reservoirs across froxels and frames.
  It buys selection quality, and §1.4 says the selection is already exact here. Not needed.
- **UE5 volumetric fog.** History 0.9, per-frame sub-voxel jitter, no shadows from local lights
  by default, and a documented ghosting trail on fast lights.
- **`rtxmw`**, the reference this is ported from, leaves lamps in the fog unshadowed. This fork
  chose shadows, and `aLampLights…` asserts that a lamp behind a lid lights no air. Keep them.

The answer everyone converges on: **ask every question at the froxel, one ray per froxel, then
filter in space and time.** Nothing in the field asks one point to answer for eight.

## 3. The design

Two kernels in place of one.

### 3.1 `fogscatter.comp` — one thread per froxel

Workgroup `8 × 8 × 4` (columns × columns × slices). A froxel:

- **Two rays.** The column's *centre* ray, from `rayAt` at the block's centre, carries the lamp
  integral. The *jittered* ray, across the block and along the slice, carries the field read,
  the water check and the origins of the shadow rays. Jitter per froxel: the blue-noise tile read
  at `column + slice · offset`, the reservoir seeded by `pixelKey(column)` and the slice.
- **Extinction** at the jittered point, `spacing` = the stride, as today.
- **Lamps, exactly.** Walk the light-grid cells the froxel's `[behind, ahead]` spans, one or two,
  with the DDA `weighLampsAlong` already has. For every lamp: `falloffAlong` over the froxel's
  stride divided by the stride, which is the mean irradiance over the froxel. Sum them into
  `lampSum` (rgb). Offer each to a reservoir with *that same share* as its weight. One ray from
  the jittered point to the held lamp gives `V_lamp`. No ray when the extinction is zero or no lamp
  reaches.
- **Sun and moons.** One ray each per froxel from the jittered point, gated as today by
  `mShafts`, `mMoonlit`, and by extinction above zero. The sun's transport keeps its own channel.
- **History.** Reproject the froxel's jittered point as `fogVolumeWas` does. Mix the scatter
  (sky + moons in rgb, density in a) and the sunward (transport in rgb, `V_lamp` in a, which
  carries nothing today). **`lampSum` is not mixed.** It is exact every frame, so a flickering
  lantern flickers in the air on the same frame it flickers on the wall, and a glow moves with
  the camera with no trail.
- **Writes** scatter, sunward and lamps. Nothing integrated.

The estimator, stated in the shader: `inscatter = lampSum · V̄ / (4π)`, where `V̄` is the filtered
`V_lamp`. Expectation `Σ s_j V_j`, variance the visibility's alone.

### 3.2 `fogintegrate.comp` — one thread per column

Sixty-four serial slices. For each: read scatter and lamps at the froxel, read sunward over the
3×3 columns around it at the same slice and average them, form the in-scatter, run the
`weight = T · (1 − exp(−σ·Δ))` recurrence exactly as the current loop does, and store the two
integrated images. No ray, no lamp walk, no field read.

The 3×3 is Remix's spatial filter at its smallest radius, on the visibility-bearing channels only.
The lamp sum is not blurred, so a glow keeps its shape and only its shadowing softens, over 24
pixels. Not along depth: a froxel is already 125 units deep at 500 units.

A subgroup scan over the 64 slices replaces the serial loop only if the measured number says so.

### 3.3 Host

- `FogVolume` gains one image `mLamps` (`rgba16f`, no pair, +16.6 MB at 1080p), bindings 8
  (storage) and 9 (sampled), `describeLayout` and the descriptor writes grow to ten, `begin`
  discards it, and a new `scattered(commands)` barrier moves the written point pair and the lamps
  from storage write to sampled read between the two dispatches. `handOver` stays for the
  integrated pair.
- `VisibilityPass` gains `mIntegrateModule` and one `ComputePipeline` for it. The integrate kernel
  takes no variant: it reads images and nothing about the sun or the sea.
- `record` dispatches scatter over `columns × rows × 64` and integrate over `columns × rows`, in
  two zones, `air` and `column`.
- `lib/bindings.glsl` set 3 grows by the two lamp bindings, in the order `FogVolume` names them.
- `weighLampsAlong` is split. The per-cell, per-lamp `falloffAlong` integration becomes one
  helper that returns the mean irradiance and offers each lamp to a reservoir. `fogUniformAlong`
  calls it per cell stretch and weighs by `absorbed`, as now. The froxel calls it once. One
  statement of what a lamp puts into a stretch of air.
- `FOG_VOLUME_SLICES_PER_RAY` goes. `FOG_SHADOW_RAYS` stays for the closed form.

## 4. Steps

Each step builds, runs the fog tests, and can be looked at with `shot`.

1. **Tests first**, so the current shader is seen failing them.
   - **The lid over the far half.** Volume path via `mFogUniform = 0.999`, which is banked air
     even to 0.2 per cent (`resolve` reads `>= 1.0`). Black fog colour, a wall at a slice edge
     (`fogDepth(k / 64) · FOG_REACH`, say 469 units), a far lamp with flat falloff as in
     `aLampLights…`, and a lid over only the far half of the ray. Expected: the near half's air is
     lit and the far half's is not, `E / 4π · (1 − T_half)`, hand-computed. Tolerance: one froxel
     straddles the lid's edge. Then the same scene over 64 frames with `mFrame` advancing and the
     history kept, as `renderFiltered` does: the standard deviation of the centre pixel over the
     last 32 frames, bounded by `0.06 · S` from `0.5 · S · 0.23 / 3`. The current shader answers
     at random, because its one probe lands under the lid or past the wall.
   - **A near lamp.** Extend `aLampLights…` with a lamp 60 units off the ray, and the expectation
     as a host quadrature of `falloff` at 10⁴ steps against the shader's closed form.
2. **Extract the lamp helper.** `fogUniformAlong` returns the same bytes on every fog test.
3. **The mechanical split.** `mLamps`, bindings, barriers, the second pipeline, the two zones.
   The integrate kernel is the current accumulation loop lifted out. The scatter kernel keeps
   today's per-stretch arithmetic for this step, so `verify --against` a run from before the step
   shows nothing moved. Measure `air` + `column` against 0.245 ms.
4. **Per-froxel everything.** The centre-ray lamp integral, the reservoir with matched weights,
   one lamp ray, one sun ray, one moon ray, `V_lamp` in `sunward.a`, `lampSum` unmixed. The
   step-1 tests pass. `shot` the three views and look at the crops.
5. **The 3×3 read in the integrate kernel.** The temporal bound in step 1 holds. `view --frames`
   on `balmora-fog-night` for what a still cannot show.
6. **Measure.** `shot --repeat=32 --upscale=off` on the three views, then `bench` on
   `balmora-fog-night` and `balmora`, into `.notes/bench.txt`. Sun rays went from 8 to 64 per
   sunward column: if they cost more than 0.3 ms on `seyda-neen-ship-dawn`, gate them per froxel
   on the sun's phase at that column rather than per stretch, and measure again.
7. **`FOG_VOLUME_HISTORY`.** Leave at 0.9 unless a moving `view` shows a trail behind a lamp.
   With `lampSum` fresh, the trail can only be in the shadowing.

## 5. Verification

- `build-debug/components-tests --gtest_filter='RtxVisibilityTest.*'` after each step, the fog
  tests in `apps/components_tests/rtx/visibility/fog.cpp` first. The per-frame allocation test
  must not see the second dispatch allocate.
- `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh`.
- `verify` before step 3 and after each of steps 3, 4 and 5, `--against` the previous.
- The numbers in §1.1 and §1.2 taken again with the same commands.

## 6. Cost and risk

- **Rays.** Worst case three per froxel, each gated: no lamp ray where no lamp reaches or the
  air is empty, no sun ray off the sunward part of the frame, none of either where the banked
  field is clear, which is a third of it (the band is zero below 0.45 of a field with spread
  0.1204, so 34 per cent). Expected +0.2 to +0.5 ms at 1080p. Measured at step 6.
- **Memory.** +16.6 MB.
- **Ghosting.** Less than today: the glow is exact each frame, only its shadow term is averaged.
- **A rebuilt light buffer** changes lamp order. Nothing stored is a lamp index, so nothing
  breaks. `historyLost` already voids the reprojection on a rebuild.
- **A froxel across a wall** is lit by lamps on both sides, as today. The volume cannot know the
  surface, and the trace's trilinear read at the hit distance is what bounds it.

## 7. Sources

- Wronski, *Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering*,
  SIGGRAPH 2014 — <https://bartwronski.com/publications/>
- Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015 —
  <https://www.ea.com/frostbite/news/physically-based-unified-volumetric-rendering-in-frostbite>
- *Volumetric Effects of The Last of Us: Part Two*, SIGGRAPH 2020 —
  <https://dl.acm.org/doi/fullHtml/10.1145/3388767.3407393>
- NVIDIA RTX Remix, Volumetrics settings —
  <https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.4.0-0/docs/runtimeinterface/renderingtab/remix-runtimeinterface-rendering-volumetrics.html>
- Lin, Wyman, Yuksel, *Fast Volume Rendering with Spatiotemporal Reservoir Resampling*, TOG 2021 —
  <https://graphics.cs.utah.edu/research/projects/volumetric-restir/>
- Epic, *Volumetric Fog in Unreal Engine* —
  <https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine>
