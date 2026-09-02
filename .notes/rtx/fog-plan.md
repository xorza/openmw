# The fog volume: a thread to a froxel, and lamps that do not boil

Two complaints, one fix. `shader-review.md` §7 asks whether the one-thread-per-column shape of the
volume pass is worth splitting. The night fog in Balmora boiled under the street lamps. The split is
what the noise fix needed, and the noise fix is what made the split worth doing.

**Done, and then done again.** This file is the record: what was measured, what was built, the four
things that were wrong along the way, and the three more that the rings around the eye turned out to
be. `fogscatter.comp`, `fogintegrate.comp`, `fogdepth.comp` and `Rtx::FogVolume` carry the reasons
that belong beside the code.

## 1. What was wrong

### 1.1 The pass was never the cost

`build-release/openmw-rtxtool shot --upscale=off --repeat=32`, 1920×1080, GPU zone medians:

| view                   | air (ms) | frame (ms) |
|------------------------|---------:|-----------:|
| balmora-fog-night      |    0.250 |       5.15 |
| balmora, noon          |    0.232 |       5.84 |
| seyda-neen-ship-dawn   |    0.380 |       7.43 |

The review's own threshold is "a few tenths of a millisecond, leave it". The debug build with the
validation layers on reads 2.3 ms for the same pass, which is where the item got its size.

### 1.2 The lamp-lit air was the noisiest thing in the frame

One frame with no history against a 200-frame reference, linear radiance, luminance per pixel:

| pixels by reference luminance | count   | relative RMS | p90 of relative error |
|-------------------------------|--------:|-------------:|----------------------:|
| 0.001 – 0.005 (dark ground)   | 736,384 |        0.176 |                 0.289 |
| 0.005 – 0.020 (fog, sky)      | 996,535 |        0.058 |                 0.071 |
| 0.020 – 0.100 (lamp-lit fog)  |   9,297 |    **0.474** |             **0.722** |
| sky band, rows 20–120         | 110,000 |        0.036 |                       |

The mean absolute difference between neighbouring 8×8 blocks correlated at 0.63 both ways: the error
was painted in froxel-sized blocks, each either fully lit or fully dark.

### 1.3 Why

**The visibility was asked at a point that was not where the light is.** A column was cut into eight
stretches of eight slices. One probe per stretch, drawn anywhere along it, weighed the lamps that
reach *the probe*, traced one ray, and multiplied the sum of every lamp at every slice of the
stretch by the answer. At 500 units the stretch is 1,406 units long against a lamp reach of 128 to
512, so a probe outside every reach held nothing — and `lampVisible` answers one for an empty
reservoir, because its own caller multiplies that by a radiance of nought. Stored and applied, that
one lit the air in front of a lantern standing behind a wall.

**The lamp's irradiance was point-sampled** in a froxel 4 × 4 × 125 units, where the uniform path
already had `falloffAlong` to integrate it.

**One depth jitter served all 64 slices** of a column, so a column's error was coherent in depth.

**The thread shape** was the review's own point and mattered least at 0.24 ms — but a ray per froxel
in a per-column thread is 64 serial rays, so the estimator could not be fixed without it.

Not the cause: the reservoir's choice of lamp is exact in expectation when the weights are the
shares of the sum, and the history weight cannot average away a coin that flips a whole term.

### 1.4 And nothing tested it

`litThroughFog` sets `mFogUniform = 1`, so every lamp and lid test exercised the closed form. The
volume was reached only by the coverage and wind tests, and neither has a light in it.

## 2. What the field does

Wronski 2014 and Hillaire 2015: scatter per froxel over the whole grid, integrate front to back per
column, jitter inside the froxel, reproject and average. The Last of Us Part II keeps volumetric
shadows deliberately soft, so an edge in the air is a gradient wider than the grid's Nyquist. RTX
Remix, the ray-traced one: a grid at 1/16 of the frame with 48 slices, RIS over 8 candidates per
froxel, **one visibility ray per froxel**, temporal reuse, a spatial Gaussian of radius 2 to 4, a
firefly clamp. Volumetric ReSTIR reuses reservoirs across froxels and frames — it buys selection
quality, which was already exact here, so it is not needed.

Every one of those is a per-froxel quantity. Nothing in the field asks one point to answer for
eight.

## 3. What was built

**`fogscatter.comp`, a thread to a froxel**, `8 × 8 × 4`. Two rays per froxel decide it: the
column's *middle* ray carries the lamp integral, because what a lamp delivers is smooth and wants no
jitter; the *jittered* ray carries the field read, the water test and the origin of every shadow
ray. Every lamp reaching the froxel is integrated over it in closed form and offered to a reservoir
with its own share of that sum as its weight, so `sum × visibility` carries the whole sum's
expectation; `aimLampFrom` then moves the one ray to leave from the jittered point. One sun ray, one
moon ray. What it stores: the sky and moons with the extinction, filtered; the sun's transport with
the lamps' seeing beside it, filtered; and what the lamps deliver, **not** filtered, so a flame
flickers in the air on the frame it flickers on the wall.

**`fogintegrate.comp`, a thread to a column.** Sixty-four slices, front to back, five image reads
and multiply-adds. It reads the seeing over a separable `1 2 1` tent across the screen — four
bilinear taps on a texel's corners — because a shadow edge in the air is nought on one side and one
on the other, and nothing else the volume holds needs it.

**`lampsInAir`** in `lib/fog.glsl` is the one walk of the light grid over a stretch of a ray, with
`LAMPS_BY_ABSORPTION` for the closed form's whole-ray reservoir and `LAMPS_BY_LENGTH` for a froxel's
own. **`lib/froxel.glsl`** is the grid's geometry, so a shader that only asks where a slice starts
does not pull in a phase function and a ray query to find out.

**`Rtx::FogVolume`** gained one image for the lamps, bindings laid out sampled-then-storage, and a
`scattered` barrier between the two dispatches. The set stays pushed across them.

**`fogdepth.comp`, a thread to a column, before both** (§4.5). One ray down the column's own
jittered ray, and what it stops at is what every froxel of the column keeps its draws short of.

**`FogSlice`** in `lib/froxel.glsl` (§4.6): a slice is a sample at its own middle and the air between
two slices is the line between them, which the integrate pass steps through in two halves and the
trace steps through again from the last edge a pixel passed to where its surface stands. Two more
images carry what each slice comes to once every filter is applied, so the trace reads six taps and
applies no filter of its own.

## 4. Four things that were wrong on the way

Each was found by a measurement, and each is written up beside the code that carries it.

1. **The volume's temporal filter did not exist outside a filtered frame.** One `mHistoryStale`
   served the denoisers and the volume, and only a denoiser took it down — so every `shot`, every
   `verify` and every test that measures radiance told the volume its history was lost and never
   said otherwise. `VulkanRenderer::mAirStale` is the second flag, spent by the trace, which runs
   every frame.
2. **A froxel with no lamp stored full sight of one.** `lampVisible` answers one for an empty
   reservoir; stored, that one was read by the neighbours the tent averages over, and a lantern
   behind a lid lit the air in front of it after all. Three bytes of leak, and the test caught it.
3. **The reprojection read half way between a slice's edges, not half way through it.** The depth
   curve is quadratic, so those are different distances, and a slice near the eye asked for a fifth
   of a texel past its own centre. `froxelMiddle` is the fix.
4. **A gate on the froxel's own extinction painted the grid's shells on the ground.** Skipping the
   rays where the coverage band is clear looked free — the integrator weighs such a froxel by
   nothing — but it made the froxel store an answer on the frames it holds air and hold or drop it
   on the frames it does not. Which of the two it did turned on where inside itself the sample fell,
   that excursion is the stride, and the stride grows with depth: concentric rings around the eye
   that no length of reference washed out. The gate is gone and every froxel asks every question.

5. **The slice a surface stands in was sampled on both sides of it.** The volume is filled before
   anything is traced, so a froxel the ground or the sea stands inside drew half its samples from
   inside the ground — where the field says whatever it says — or under the water, where there is
   no fog at all. The pixel then read the accumulation between that slice's two edges, and what it
   got was the slice's optical depth laid out as though the surface were not there. Measured as the
   slope of the optical depth along the ground binned by where inside its slice the surface stood,
   from the near edge to the far: 0.40, 0.49, 0.73, 0.88, 1.08, 1.23, 1.30, 1.42, 1.34, 0.96 of the
   local mean — a sawtooth with the slices' own period, which is the rings. Three things were tried
   against it that were not it: a monotone cubic in place of the sampler's line, the field's level
   taken from the sample's own depth rather than the slice's stride, and the field read at level
   nought. Only the last removed the rings, and only because it changed the picture by forty per
   cent everywhere else. `fogdepth.comp` finds the column's surface and the scatter pass draws its
   samples short of it.
6. **And a constant over a slice draws the slices.** With the samples in the air, the same
   measurement read 0.51, 0.91, 1.16, 1.19, 1.27, 1.27, 1.17, 1.06, 0.78, 0.39 — a bump in every
   slice, because the cubic then in the reader put each slice's optical depth in its middle; the
   sampler's line put it in a corner at every edge instead. Either is a pattern at the slices'
   period. `FogSlice` is the shape that has none: a sample at each slice's middle and the line
   between two, integrated by the pass and by the pixel alike. The profile reads 1.03, 0.99, 1.00,
   1.00, 1.01, 1.01, 1.01, 1.01, 0.99, 0.99 — a spread of 0.012 against 0.339.
7. **A froxel the surface barely enters converged once in twenty frames**, because a sample that
   landed past the surface was thrown away and the froxel kept its first draw. The sample is now
   drawn inside the air the froxel holds, so every frame is a draw. And the flat continuation the
   integrate pass carries past the surface reaches one half-slice and no further: carried over
   every slice behind a wall, it laid the lit air beside a lantern over the whole of the fog behind
   the wall it hung on, for every pixel beside the wall's silhouette that saw past it, in blocks
   eight pixels wide.

**And the tent covers the density and the sky's scattering, not the seeing alone**, and leaves out
every neighbour whose surface stands short of the slice. The coverage band is the same shape as a
shadow's edge — a bank is there or it is not — so a froxel on a bank's edge held a third of a coin
after ten frames, painted at eight pixels; the tent takes that to 0.0101 from 0.0148 of the optical
depth at the froxel's own scale.

## 5. What it comes to

**The noise**, same measurement as §1.2:

| pixels by reference luminance | before | after |
|-------------------------------|-------:|------:|
| 0.001 – 0.005 (dark ground)   |  0.176 | 0.116 |
| 0.005 – 0.020 (fog, sky)      |  0.058 | 0.030 |
| 0.020 – 0.100 (lamp-lit fog)  |  0.474 | **0.056** |
| p90 of that band              |  0.722 | **0.046** |
| 8×8 block correlation         | 0.63 / 0.63 | −0.05 / 0.07 |

The block structure is gone rather than reduced.

**The flicker**, from `theVolumeSettlesTheAirUnderALampRatherThanFlickeringBlockByBlock`: a settled
pixel of lamp-lit air moves 0.4% of its own value between frames. With the history switched off the
same estimator moves 5.9%.

**The rings**, from `theVolumeLightsTheAirUpToASurfaceWhereverInASliceItStands`: the volume's lit
air in front of a wall agrees with the closed form's within five per cent with the wall half way, nine
tenths and a tenth of the way through a slice, where the estimator sampled on both sides of the wall
lost seven, eight and eleven per cent of it at those three depths.

**The cost**, GPU zone medians of `shot --upscale=off --repeat=32` on a warm card, the same views as
§1.1. The depth pass sits inside the `air` zone, and the froxels behind a surface trace nothing, which
is what pays for it:

| view                 | air, split (§3) | air now | column, split | column now |
|----------------------|----------------:|--------:|--------------:|-----------:|
| balmora-fog-night    |           0.35 |    0.19 |         0.065 |      0.088 |
| seyda-neen-ship-dawn |           0.65 |    0.45 |             — |      0.097 |

The split's own figures were taken on a loaded machine and are the shape of that change rather than
numbers to quote; the column pass grew by the tent over two images and the two slice stores, and the
air pass shrank by every ray a froxel behind the ground no longer casts.

**`verify --views=all`**: the five interiors and `vivec-canalworks` are unchanged, as they must be —
a room reads the closed form and dispatches no volume. Every exterior differs.

## 6. Left undone

- **The sun and the moons get a ray per froxel**, where the review's arithmetic suggested sharing
  one across a stretch through shared memory. Measured at a quarter of a millisecond, that is not
  worth the machinery yet; if a frame ever needs it back, `8 × 8 × 8` workgroups with the stretch's
  ray traced by one thread of each eight is the shape.
- **`FOG_VOLUME_HISTORY` stays at 0.9**, which is what UE and Frostbite ship. Nothing seen so far
  asks for less.
- **The tent is `1 2 1` and fixed.** Remix adapts its radius on how settled a froxel is. There is no
  measurement here that asks for it.
- **The field's level is chosen by the froxel's depth and blurs the field across the screen by as
  much.** A froxel is four units wide where it is a hundred deep, and the chain's levels are
  isotropic, so a bank's edge across the screen is softened to the slice's stride. The march did
  the same. What would fix it is a level from the column's width with the depth integrated by more
  draws, and nothing seen so far asks for it.
- **A column's surface is one ray, and the block it stands for is sixty-four pixels.** A froxel
  behind the surface on the frames the ray hits an edge keeps what it held on the frames it missed,
  which is the right air for the pixels beside the edge and takes ten frames to settle after the
  eye moves. Nothing seen so far shows it.

## 7. Sources

- Wronski, *Volumetric Fog*, SIGGRAPH 2014 — <https://bartwronski.com/publications/>
- Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015 —
  <https://www.ea.com/frostbite/news/physically-based-unified-volumetric-rendering-in-frostbite>
- *Volumetric Effects of The Last of Us: Part Two*, SIGGRAPH 2020 —
  <https://dl.acm.org/doi/fullHtml/10.1145/3388767.3407393>
- NVIDIA RTX Remix, Volumetrics —
  <https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.4.0-0/docs/runtimeinterface/renderingtab/remix-runtimeinterface-rendering-volumetrics.html>
- Lin, Wyman, Yuksel, *Fast Volume Rendering with Spatiotemporal Reservoir Resampling*, TOG 2021 —
  <https://graphics.cs.utah.edu/research/projects/volumetric-restir/>
- Epic, *Volumetric Fog in Unreal Engine* —
  <https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine>
