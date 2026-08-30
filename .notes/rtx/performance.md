# RTX performance — 2026-08-30

What the renderer costs today, where the time goes, and the order to attack it in. Measured on
`1ce513e58a` with a clean tree. Delete a step when it lands. Rewrite the tables when the frame
changes shape.

## 1. Method

- **Build.** `build-release/` — Release, `-O3 -g1 -fno-omit-frame-pointer`, `OPENMW_RTX_BENCH=ON`.
  Made by `apps/rtxtool/release.sh build`.
- **Numbers.** `openmw-rtxtool bench --validation=false --window=false` for a distribution over 600
  frames after 60 of warm-up. `openmw-rtxtool shot --repeat=100` for one frame's best of 100, which
  is the number an A/B is read from. The in-game bench through `OPENMW_RTX_BENCH=600:120
  release.sh game`.
- **What a bench row now carries.** The CPU frame split three ways — `walk` is the graph walk the
  harness does for the game, `wait` is the CPU standing still for the device and `place` is the
  renderer being told what moved. `gpu ms` carries `blas` on the frames a cell arrived. Each place
  names the hour it stood at, and the clock and throttle reason the card held as it ended.
- **CPU profile.** `apps/rtxtool/profile.sh`, which is `perf record -e task-clock -F 5999
  --call-graph fp` over the measured frames only. The reports are in `build-release/perf/`.
- **GPU profile.** The renderer's own timestamp zones, and `nsys` for the timeline — Nsight Systems
  is installed and the driver lets a non-root user profile
  (`NVreg_RestrictProfilingToAdminUsers=0`, `/etc/modprobe.d/nvidia-profiling.conf`). `ncu` is not
  installed, so what a pass costs *inside* the trace kernel is measured by removing it: edit one
  constant or one early return in the shader, rebuild `openmw-rtx-vulkan-shaders`, run `shot`,
  revert. That is how §3.2 was taken. Every such edit was reverted and the tree is clean.

**The GPU is power-capped under load.** `nvidia-smi` during a bench reports
`clocks_throttle_reasons.sw_power_cap = Active` at 1770–1875 MHz and 65–68 °C. Cool, the card runs
2040–2325 MHz. So a bench median moves ±7 % run to run (ship at the target: 16.06, 16.86, 18.31,
16.63 ms in four runs), and the `upscale` zone — the same work every frame — is a clock proxy:
it read 5.06 to 7.18 ms across one A/B series. **Every GPU millisecond below is at about 1.8 GHz.**
`shot --repeat=100` best-of-100 holds to ±2.5 % (base trace 7.52–7.89 ms over ten repeats) and is
what the A/B tables use. A bench comparison is only readable when the effect is over 10 %, or when
it is read as the ratio `trace / upscale`.

**The in-game numbers before this file were taken through the validation layers.** `[RTX]
validation` was a setting, the user config had it on, and nothing said so where the numbers were
read — so every in-game figure in `.notes/bench.txt` has the layers in it and none of them
compares with a row here. The setting is gone: the build decides, a Release build never loads
them, and `Rtx::sValidationByDefault` is the one place that says so. The row in §2.3 was taken
after that, from `build-release/`.

## 2. Where the frame stands

### 2.1 Harness, camera still

`bench --suite=default`. Milliseconds. The GPU row is the median of each zone.

| place | output / traced | frame med | p99 | wait | place | GPU | fps | 1 % low |
|---|---|---:|---:|---:|---:|---|---:|---:|
| ship | 1920×1080 / 1280×720, quality | 7.57 | 12.0 | 4.17 | 0.52 | trace 3.87, upscale 2.25, tlas 0.26, waves 0.23, refit 0.15, bloom 0.12 | 132 | 83 |
| guild | same | 8.05 | 11.3 | 6.64 | 0.24 | trace 3.03, upscale 2.26, tlas 0.18, bloom 0.12, refit 0.10 | 124 | 89 |
| ship | **3840×2160 / 1920×1080, performance** | 16.06 | 23.0 | 12.39 | 0.52 | trace 8.72, upscale 5.06, bloom 0.47, tone 0.31, tlas 0.28, exposure 0.28, waves 0.24, composite 0.20, refit 0.16 | 62 | 43 |
| guild | same | 17.05 | 22.0 | 15.60 | 0.25 | trace 8.69, upscale 5.78, bloom 0.47, tone 0.31, exposure 0.28, composite 0.21, tlas 0.19, refit 0.10 | 59 | 45 |
| ship | 3840×2160 native, upscale off | 53.0 | 59.4 | 47.6 | 0.55 | trace 36.4, filter 10.6, accumulate 2.3, composite 0.84 | 19 | 17 |

- **At the target the frame is the device's, and the device is full.** The zones sum to 15.7 ms
  (ship) and 16.0 ms (guild) against 16.7. The CPU adds 3.7 ms in series on the ship, hidden behind
  the wait. There is no headroom: the 1 % low is 43–45 fps.
- **The trace scales with pixels.** 4.3 ms per megapixel at 1.8 GHz, from 1280×720 through
  1920×1080 to 3840×2160. Ray Reconstruction at 5.1 ms for 1920×1080 → 3840×2160 buys 28 ms of
  trace and 10 ms of wavelet filter against native. The upscaled path is the right path.
- **Ray Reconstruction costs 5.1–5.8 ms at 3840×2160 output**, whatever the input: preset `e` reads
  the same as `d` (5.16 against 5.06–5.47). It is a third of the frame and has no dial.
- **Post is 1.3 ms at 3840×2160.** Bloom 0.47, tone 0.31, exposure 0.28, composite 0.20, all at
  output resolution.

**The dawn row, which the table above predates.** `views.cfg` has the ship at 6.5 h as
`seyda-neen-ship-dawn`, taking its camera from the noon view so the two cannot drift apart, and
`[default]` runs both. Measured on this branch: at 1920×1080 quality the noon ship is 8.19 ms median
(trace 4.12, 1965–2010 MHz) against dawn's 11.99 (trace 7.53, 2070 MHz) — **the trace goes up 83 %
for the hour alone**, and the walk sits at 3.7–4.3 ms behind either.

**And a caveat the clock line does not catch.** The 3840×2160 pass of the same suite read noon 18.12
(trace 9.20) and dawn 26.59 (trace 18.24) at 69–70 °C, on a card that had been benching for four
minutes — and it read the guild's `upscale` at 9.76 ms against the 5.78 in the table, for identical
work. That is heat, not a change. **A suite run back to back with another is not comparable with one
run cold**, and a clock sampled at the two ends of a place does not show it: sample through the run
if this keeps mattering.

### 2.2 Harness, one frame, best of 100 (`shot`, ship, 3840×2160 performance)

| when | frame | trace | upscale |
|---|---:|---:|---:|
| noon, clear | 13.6 | 7.75 | 4.52 |
| **dawn, 6.5 h, clear** | **20.2** | **15.32** | 4.44 |
| dawn, 6.5 h, foggy | 20.2 | 15.14 | 4.49 |
| midnight, clear | 13.2 | 7.61 | 4.56 |
| noon, rain | 15.6 | 8.82 | 4.60 |

**Dawn doubles the trace.** The sun is low, its shadow rays run long and grazing, the fog is lit
from the side and asks its eight sun probes on every pixel, and the moons are still up. The frame
budget has to be written against dawn, not noon: today dawn is 50 fps at the target before the CPU
is counted.

### 2.3 The game

`OPENMW_RTX_BENCH=600:120 release.sh game --config <dir with validation = false>`. The quicksave
is an interior of 1233 instances, 30 lights and 101 deforming drawables, in a 1994×1366 window
traced at 1329×911 (quality).

| frame med | p99 | wait | GPU | fps | 1 % low |
|---:|---:|---:|---|---:|---:|
| 8.49 | 11.4 | 6.61 | trace 4.54, upscale 3.05, tlas 0.19, bloom 0.13, refit 0.09 | 118 | 88 |

The game's whole CPU frame — update, physics, scripts, cull, the walk and the placement — is 1.9 ms
here. The game is device-bound at this setting like the harness is.

### 2.4 The island crossing

`bench --suite=streaming`: 12 000 units a second, twenty cell boundaries in ten seconds.

| output | frame med | mean | p95 | p99 | worst | crossings | worst crossing | in crossings | fps | 1 % low |
|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|
| 1920×1080 quality | 6.77 | 16.1 | 65.8 | 214 | 306 | 19 | 298 ms | 2.9 s of 9.65 — 2.3 reading, 0.7 building | 148 | 4.7 |
| 3840×2160 performance | 16.45 | 24.0 | 68.9 | 220 | 335 | 19 | 327 ms | 3.0 s of 14.4 — 2.4 reading, 0.7 building | 61 | 4.6 |

**After the fold changed** (1920×1080 quality, three runs): frame median 8.2–8.9 ms, mean
15.8–16.5, p99 181–190, worst 286–298, and 5.3–5.5 fps at the 1 % low. Nineteen crossings still,
the worst 276–289 ms, 2.5 s in them over the run — 1.8 reading and 0.7 building. The table above is
the same route before it.

**A crossing is a 100–300 ms frame, and there are nineteen of them.** The median is the still
frame's median. Everything above p95 is a crossing or the paging in the frames around one. The
`wait` column stays at 1 ms: the device is idle through all of it. This is a CPU tail.

## 3. Where the time goes — GPU

### 3.1 What one pixel pays

Read out of `visibility.comp` and `lib/`. A lit ground pixel outdoors at noon:

| rays | for |
|---:|---|
| 1 | primary, with the cutout any-hit resolved by a texture fetch per candidate |
| 0–1 | a second primary behind a pane or a faded actor |
| 1 | sun shadow, a cone |
| 0–2 | moon shadows, when a moon is up |
| 1 | one lamp, chosen by a one-deep reservoir over every lamp reaching the point |
| 1 | the bounce (`bounceLight`), a cosine ray |
| 1 | `skyReaching` at the bounce hit |
| 1 + 0–2 + 1 | sun, moons and a lamp at the bounce hit (`shadeSurface` again) |
| 8 (+8) | `FOG_SHADOW_RAYS` sun probes along the fog march, and eight moon probes when a moon is up |
| — | 24 `FOG_STEPS` of extinction, each weighing every lamp in its grid cell into one reservoir |
| 0–2 | water: reflection and refraction, each shaded |

So 15 rays at noon and up to 25 at dawn, and the eight to sixteen of them that are fog probes
are the largest single count. Indoors `FOG_UNIFORM` takes the closed form: no steps, one walk of
the light grid along the ray (`FOG_CELLS_ALONG = 32`), and the same eight shadow stretches.

### 3.2 What each part costs — removed one at a time

`shot --repeat=100`, 3840×2160 performance, GPU `trace` zone in ms. Each row is one edit against
the base measured immediately after it, so the pair shares a clock.

| removed | ship noon | ship dawn | guild |
|---|---:|---:|---:|
| base | 7.5–7.9 | 14.8–15.7 | 6.6–6.8 |
| fog march entirely | 4.39 (−3.4) | 7.41 (−7.6) | 3.91 (−2.7) |
| fog at 2 shadow rays, 8 steps | 5.68 (−2.1) | 10.45 (−4.5) | 5.31 (−1.3) |
| fog at 4 shadow rays, 12 steps | 6.39 (−1.4) | — | — |
| fog at 4 shadow rays, 24 steps | 6.78 (−1.0) | — | — |
| the bounce | 5.19 (−2.5) | 10.42 (−4.5) | 4.57 (−2.0) |
| `skyReaching` at the bounce hit | 6.82 (−0.9) | 14.13 (−0.7) | — |
| the lamp shadow ray | 6.45 (−1.2) | 13.28 (−2.4) | 5.29 (−1.3) |
| the sun shadow ray | 6.99 (−0.65) | 12.91 (−2.2) | — |
| the cutout any-hit (every cutout opaque) | 7.22 (−0.46) | — | — |

- **Fog is 43 % of the trace outdoors at noon, half of it at dawn, and 41 % indoors.** Indoors
  that is after the closed form: what is left is the eight shadow stretches, and they are 2.7 ms.
  At dawn the march's sun and moon probes are 7.6 ms on their own — the whole of the noon trace.
- **The bounce is a third, and 4.5 ms at dawn.** Its own ray plus three or four more at the hit,
  and at dawn the hit shades the sun, the moons and a lamp with long rays.
- **Every shadow ray costs three times more at dawn.** The sun ray goes from 0.65 to 2.2 ms and
  the lamp ray from 1.2 to 2.4: a grazing ray crosses more of the structure before it leaves it.
- **The lamp ray is 1.2–1.3 ms at noon and indoors**, which is one ray per pixel against a scene
  where most pixels hold a lamp in reach.
- **The cutout any-hit is 0.5 ms at noon on the ship.** 93.8 % of micromapped microtriangles
  are `unknown`, so nearly every candidate still runs the texture fetch. Raising
  `sSubdivisionCeiling` to 8 changes nothing (93.79 %); 4 texels a microtriangle reaches 92.5 % at
  2.3× the build time; 2 texels and a ceiling of 10 reach 92.2 % at 3× (11.9 s). The classifier
  (`AlphaBounds::classify`, all texels of a box certainly one thing) is the limit, not the level.

### 3.3 What is not a lever

`bench` A/B on the ship at the target, read as `trace / upscale` against a base of 1.72–1.80.
Distant statics off (6992 instances) 1.73. People off 1.75. Props off 1.78. Preset `e` 1.81.
Distant terrain off 1.78. **None of the content dials moves the GPU.** The trace is per-pixel work
and the acceleration structure hides the instance count. Foggy weather reads 1.48: the fog *edge*
clamps the far distance and the rays end sooner.

## 4. Where the time goes — CPU

### 4.1 Camera still (ship, 1920×1080 quality, 0.49 cores)

75 % of the CPU is `PosedActors::step`, the per-frame walk of the whole graph — **2.7 ms a frame**
of the 3.4 the CPU spends, and the same walk `RtxRenderer::frame` does in the game. By self time:
`osg::Group::traverse` 9.4 %, `addDrawable` 7.8 %, `RigGeometry::cull` 7.3 % (the skinning),
`resolveMesh` 4.9 %, the identity hash maps (`StateSet → Known`, `Drawable → mesh`) about 8 %,
`retire` 2.6 %, `Matrixf` mult and compare 3.8 %, `dynamic_cast` 1.4 %, `StateSet::getUniform`
1.3 %. The walk visits 8544 instances to find that 179 drawables deformed.

### 4.2 Camera flying (island crossing, 1920×1080 quality, 1.87 cores, 17.7 core-seconds)

Two threads, each about half. Percentages are of all samples.

**The main thread.**

- `PosedActors::step` **23.5 % — 4.2 s, 6.9 ms a frame on average.** The bench has no row for
  it (`.notes/ISSUES.md`). It is large because the terrain and object paging runs *inside the
  walk*: `Terrain::QuadTreeWorld::collect` → `handOver` → `loadRenderingNode` is 27.2 %, of which
  `ObjectPaging::createChunk` 6.6 % (`SceneUtil::Optimizer::optimize` 5.0 % — the merge of a
  chunk's statics into one geometry), `ChunkManager::createChunk` 0.7 %. A camera at 12 000 units
  a second changes chunk levels every few frames, and every changed chunk is built on the frame.
- `SceneExtractor::addDrawable` **15.5 %** → **`SheetFold::fold` 5.2 %**, from 20.9 % and 12.6 %
  when the fold sorted every triangle of every arriving mesh to find its reversed twin. It looks a
  canonical corner triple up in a flat open-addressed table now, and hashes the corner bits rather
  than reaching `std::hash<float>`. **What is left of it is the paging**: a chunk is one merged
  geometry of every static in it, so a source drawable merged into several chunks is still folded
  once per chunk and again at every level change — step 7.
- `StagedWorld::moveTo` 12.75 % — **2.26 s, 119 ms a crossing**: reading the ring's cells,
  instancing their objects, the mirror walk, the sweep.
- `SceneUploader::hand` 5.5 %: `extendScene` 3.85 % (`buildMicromaps` 1.05 %, of which
  `AlphaBounds::classify` 0.58 %), `placeScene` 2.3 %. And `extendScene` begins with
  `finishFrames()` — **every arrival drains the frame ring**, so the pipelining of §2.1 is lost on
  exactly the frames that could use it.
- `ioctl` 5 %: the driver's submits.

**The worker thread.** `TerrainComposite::TerrainComposite` **47.5 % of all samples — 31 % self**,
with `bilinear` 8.1 %, `paintedLight` 2.9 %, `toEncoded` 2.6 %, `lroundf` 2.4 %. One thread bakes
every flattened chunk (39 at the ship) at 512×512 texels × layers, scalar, one texel at a time.
It is off the frame path, but it is a whole core for the length of a crossing, and a chunk waits
unbaked until its turn.

### 4.3 The composite bake is the CPU's largest single item

Half of all the CPU the streaming run burns is `TerrainComposite`, and it is the same arithmetic
the trace kernel does per hit when a chunk is not yet flattened: a mask weight, a bilinear fetch
per layer, a division by the painted light. It is a compute shader written in C++.

## 5. The plan

Priorities as `CLAUDE.md` has them: how it looks, then performance, and *feature-complete first*.
So the steps that change no picture come first and can land any time; the steps that trade
quality are listed with what they cost on screen and are the user's call; and every step ends in
the measurement that says it worked.

### 5.1 The tail — crossings and paging

These change no pixel. They are the 1 % low on any route: 4.6 fps today.

6. **Arrivals beside the frame, not in it.** `extendScene` stops calling `finishFrames()`. The BLAS
   builds and texture uploads of an arrival record on a second queue (async compute, or transfer
   for the uploads) with a fence of their own; the top level is rebuilt over the old set until that
   fence signals, then swapped. Needs nothing the ring does not already have (`Graveyard`,
   fence-per-frame).
   *Number:* worst crossing 300 → tens of ms; route 1 % low 4.6 → 30+.
7. **Paging off the walk.** **The fold's other half waits on this**: `SheetFold::fold` is now one
   pass, and what is left of its cost is that `ObjectPaging` hands the extractor a freshly merged
   `osg::Geometry` per chunk and per level change. The extractor keys meshes on the drawable
   pointer, so nothing is folded twice — the merge simply has no back-reference to the sources whose
   answers it could reuse. In the game, `CellPreloader::preloadTerrain` builds the quad tree's
   chunks and the paged statics on its worker for the player's position, and `TerrainResidency`
   then finds them in the chunk cache. The harness has no preloader, so *its* crossing pays what
   the game's does not. First measure the game on a route (`rtxtool travel` in `todo.txt`, or a
   Lua script flying the player) and split the 119 ms per crossing into what the game also pays
   (read, instance, mirror, upload) and what only the harness pays (chunk builds). Then give the
   harness a preloader or stop quoting its paging. Either way `collect()` must stop building
   chunks on the walk: a chunk that is not ready is a chunk that is not drawn this frame.
   *Number:* `QuadTreeWorld::collect` under 5 % of a route's profile.
8. **Estimate a texture once.** `SceneTextures` builds a `ShadingMap` for every arriving texture
   every time it arrives; the composite queue already caches by file. One cache, keyed by
   normalized path, for the life of the process.
9. **The composite bake becomes a compute pass** — the same mask, bilinear and delight the trace
   kernel already runs per hit, written once as a shader over a 512² image per chunk, submitted on
   the arrival queue of step 6. A chunk is flattened in under a millisecond of device time
   instead of a core-second of CPU, and it stops competing with the walk for the memory bus.
   *Number:* `TerrainComposite` leaves the profile; a chunk is baked the frame after it arrives.

### 5.2 The median — GPU, no picture change

10. **A froxel fog volume outdoors.** The per-pixel march integrates the same field, the same
    lamps and the same eight sun probes for every pixel of every frame. A frustum-aligned grid —
    240×135×64 at 1920×1080 — integrated once per froxel in a compute pass before the trace, with
    the previous frame's volume reprojected in, and one trilinear fetch per pixel. The interior
    closed form stays. This is what every shipping volumetric does and the fog's grain is far
    coarser than a froxel.
    *Number:* the march is 3.4 ms at noon and 7.6 at dawn (§3.2); a volume costs about a
    millisecond of its own, so −2.5 noon and −6.5 dawn. The largest GPU lever there is.
11. **Post at the resolution that shows.** Bloom's pyramid, the exposure histogram and the
    composite run at 3840×2160 for 1.3 ms; bloom from a half-resolution source and the histogram
    from a quarter-resolution one change nothing a viewer sees.
    *Number:* about −0.5 ms, an estimate from the zone sizes; measure it.
12. **The lamp reservoir's ray only when it is worth one.** `lampVisible` traces the held lamp
    whatever its weight. Skip the ray when the held lamp's unshadowed contribution is under a
    fraction of the pixel's direct light, and credit it whole. This is a bias, bounded by the
    threshold — set it where a reference cannot tell (`--accumulate`).
    *Number:* −0.3 to −0.6 ms, most indoors.

### 5.3 Quality for speed — the dials

The user's call. Each row is measured (§3.2), and each names what changes on screen. Ray
Reconstruction is a denoiser, so what a dial adds is noise it removes, up to a point.

| dial | ship noon | ship dawn | guild | what a viewer sees |
|---|---:|---:|---:|---|
| fog: 4 shadow rays, 24 steps | −1.0 | ~−2 | ~−0.6 | shaft edges a touch noisier; RR removes it |
| fog: 4 shadow rays, 12 steps | −1.4 | ~−3 | ~−0.7 | as above, plus a coarser step through thick fog |
| fog: 2 shadow rays, 8 steps | −2.1 | −4.5 | −1.3 | banding in dawn shafts; lamp halos noisier indoors |
| the bounce at half resolution (one ray per 2×2, or checkerboard + reproject) | ~−1.2 | ~−2.2 | ~−1.0 | softer indirect light on small clutter; RR is built for this input |
| no `skyReaching` at the bounce hit | −0.9 | −0.7 | 0 | a bounce that lands under an overhang reads the open sky — brighter bounce under bridges and eaves |
| the lamp at the bounce hit unshadowed | ~−0.5 | ~−1 | ~−0.7 | light leaks in the bounce term only, one bounce deep |
| the sun at the bounce hit unshadowed | ~−0.3 | ~−1 | 0 | the bounce ignores shadows at its landing point — brighter bounce in shade at dawn |
| composite extent 512 → 256 past *n* cells | CPU ÷ 4 per chunk | — | — | distant ground a little softer; it is under fog anyway |
| Ray Reconstruction preset `e` | 0 | 0 | 0 | — |
| distant statics, people, props off | 0 | 0 | 0 | not a lever |

Together the cheapest fog row and the half-resolution bounce are −2.2 ms on the ship at noon and
about −4 at dawn: at the target that is 62 → 72 fps at noon and 50 → 63 at dawn, from two dials.
Step 10 makes the fog rows moot: take the cheap halves now only if step 10 is far.

### 5.4 The median — CPU

13. **The incremental walk.** Cache the extraction of a subtree that cannot change — no update
    callbacks below it, no skeleton, no particle system, an unchanged parent transform — as a
    list of (slot, local transform), and re-place from the cache without visiting. Everything
    under an actor keeps the full walk.
    *Number:* walk 2.7 → under 0.7 ms on the ship; the game's `distant land cells` stops being a
    CPU dial.
    *Order:* after 5–7. At the target the CPU is 3.7 ms behind a 15.7 ms device and this buys
    nothing until the trace is shorter than the walk — 1920×1080 quality, or after §5.2.
14. **The per-frame work `review.md` lists** — §5.6, taken as one pass with the allocation test
    widened to catch each of them.

### 5.5 What is deliberately not on the list

- **Ray Reconstruction.** 5.1–5.8 ms at 3840×2160 and no dial. Replacing it with super-resolution
  plus the wavelet filter was costed: the filter is 10.6 ms at 3840×2160, 2.6 at 1920×1080, plus
  accumulate and SR — no cheaper and worse. It stays.
- **The micromaps.** 0.5 ms at noon on the ship, and the classifier not the level is the limit.
  Worth a look at dawn in the Bitter Coast canopy after step 10, not before.
- **Instance and content dials.** §3.3: they do not move the device.

### 5.6 From `review.md` — work the frame did not ask for

`.notes/rtx/review.md` lists these under "Work is done per frame that the frame did not change"
and "Requirements and code that nothing uses". None showed above the noise in the profiles of
§4, which is why they are step 14 and not step 1 — but each is on a frame path, each is an
allocation or a wait the posture forbids there, and each is a spike waiting for the scene that
triggers it. Grouped by the path they sit on; the file and line are in `review.md`.

**Every frame, in the walk and the placement.**

- `sceneextractor.cpp` builds a `std::string` and inserts into a `std::map` per emitter per frame
  (`stats.mTextureFormats`), builds `std::string(sNames[unit])` per pass in `getTextureTransform`,
  and `readMask` calls `image.getColor` per texel. Twenty-one emitters at the ship; a cell with
  hundreds pays a map insert each.
- `moonbuilder.cpp` `makeMoon` constructs `Sky::MoonModel` with its fallback-map string lookups on
  every call, and is called twice a frame by the game and by the harness.
- `scenedesc.cpp` `release()` allocates two `std::vector<char>` on every sweep frame; `setMaterial`
  reclassifies by a scan over every instance; `forEachPlacement` allocates a per-mesh box vector
  on every `getBounds` and `getContentBoundsWithin`.
- `scenebuffers.cpp` `place` refills three scratch lists, rebuilds the light grid and writes the
  light, offset, index, grid and emitter tables whole on every placement. Instances and materials
  have `SlotTable` row tracking; lights have none, so a still cell with three hundred lamps
  rewrites and re-bins them every frame. The ship has 306. *This one is a measurable item:* the
  `place` row and the `tlas` zone carry it.
- `vulkanrenderer.cpp` maps and unmaps `Frame::mHitCount` twice a frame when counting, and
  `Buffer::map` calls `vkMapMemory` on every call. `HostBuffer` stays mapped; `Buffer` should.

**Every frame the interface is drawn.**

- `guitextures.*` is synchronous: `getView`, `read` and `writeWith` each `flush()`, which is a
  submit and a wait, and `drawGui` calls `getView` per batch. A texture written in a frame is one
  submit-and-wait inside that frame, and `videowidget.cpp` writes one per frame — so a playing
  video, and any frame a GUI texture changes, serialises the CPU against the device. *Measure in
  the game with the menu open and with a video playing before sizing it.*

**On a crossing.**

- `sceneacceleration.cpp` `writeGeometry` calls `mPositions.reserve` once per slot inside a loop
  `SlotBlocks::reserve` already runs over every slot: `mSlots²` reservations per arrival.
- `apps/rtxtool/world.cpp` `findCell` scans every cell record per call, and `cellscene.cpp` calls it
  for each of the nine squares on every crossing and once per departed cell.
  `objectstorage.cpp` already builds an exterior index over the same data.
- `apps/rtxtool/stagedworld.cpp` walks the graph twice in the constructor (`mirror(0)` and again
  through `settle()`), discarding the first. Load time only — the harness's build is 3.9–4.2 s at
  the ship — and the first walk's share of it is unmeasured.

**Bench only.** `frametimes.hpp` holds `std::vector<std::vector<double>>`, one allocation per zone
per run — harmless to the frame, and the pattern the posture bans.

**A requirement with no user, and the decision under it.** `requirements.cpp` requires
`VK_KHR_ray_tracing_pipeline`, `VK_EXT_ray_tracing_invocation_reorder` and the features
`rayTracingPipeline`, `rayTracingInvocationReorder`, `rayTraversalPrimitiveCulling`,
`samplerAnisotropy` and `hostQueryReset`, and `device.cpp` loads and demands
`vkCreateRayTracingPipelinesKHR`, `vkCmdTraceRaysKHR` and three more; nothing outside `device.*`
names any of them. The renderer is compute with ray queries, so SER — which `CLAUDE.md` names in
the posture — cannot apply to it: reordering is a ray-tracing-pipeline feature. Either the trace
moves to a ray-tracing pipeline with `hitObjectReorder` around the bounce and the fog probes,
where divergence is (a measurement to take: the bounce at dawn is 4.5 ms and wholly divergent), or
the requirements come out and the posture's SER line with them. Decide after step 10, when the
kernel's shape is settled.

## 6. The order, in one place

| step | what | gain | picture | after |
|---|---|---|---|---|
| 6 | arrivals on a second queue, no ring drain | worst crossing ÷ 10 | none | — |
| 7 | paging off the walk; measure the game on a route | route 1 % low 4.6 → 30+ | none | — |
| 9 | composite bake on the GPU | a core freed; chunks baked on arrival | none | 6 |
| 10 | froxel fog volume | −3.4 ms noon, more at dawn | reprojected | — |
| 11 | post at half resolution | −0.5 ms | none | — |
| 8 | texture estimate cache | crossing CPU | none | — |
| 12 | lamp ray by contribution | −0.3 to −0.6 ms | bounded bias | 10 |
| §5.3 | the dials | −1 to −3.3 ms | named per row | the user's call |
| 13 | incremental walk | −2 ms CPU | none | 5–7, and only when the CPU is on the path |
| 14 | the `review.md` items, §5.6 — the light tables first | `place` and `tlas` on a still cell; the rest small | none | — |
| — | SER, or drop its requirements | unmeasured | none | 10 |

**The arithmetic at the target, at 1.8 GHz.** Ship noon today: trace 8.7 + RR 5.1 + post 1.9 =
15.7 ms. After 10 and 11: trace ~6.2, post 1.4 — 12.7 ms, 79 fps, and the CPU's 3.7 ms fits
behind it twice over. **Dawn today is 22 ms of device, 45 fps.** After 10 and 11 it is trace ~9 +
5.1 + 1.4 = 15.5 ms — under the budget with nothing to spare. The half-resolution bounce (−2.2 at
dawn) is what buys dawn its headroom, so it is the one dial in §5.3 the plan counts on.

## Appendix — raw data

`build-release/perf/` holds the last profile (ship, still). The streaming profile, the bench JSON
records, the A/B logs (`experiments*.txt`, `micromap-exp.txt`, `ab-ship-4k.txt`) and the GPU
clock log are in the session scratchpad and are reproduced by the commands in §1.
