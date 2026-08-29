# RTX performance investigation — 2026-08-29

Commit `622f68f1a4`, clean tree. Release build (`apps/rtxtool/release.sh` flags: `-O3 -g1
-fno-omit-frame-pointer`). RTX 4090 Laptop, driver 610.57.04, i9-13980HX. Every number below is from
this machine, this day, with the validation layers off and the harness window off.

**The picture in five lines.**

1. The GPU is not the bottleneck today. At 1920×1080 output (traced 1280×720) the GPU finishes a
   frame in 6–8 ms and the frame takes 10–16 ms. The CPU work — the graph walk, the placement, the
   terrain bake, the table upload — runs *in series* with the GPU, never beside it.
2. At the stated target (1920×1080 traced → 3840×2160, "performance") the GPU alone is 14.5 ms in a
   busy exterior and 18.3 ms in the mages guild. The trace is 8–12 ms of that and Ray
   Reconstruction 4.5 ms. The frame then measures 20–21 ms (48 fps) because the CPU adds its 3–6 ms
   on top.
3. Inside the trace, **the fog march is 44–66 %** and the bounce is 30–40 %. Primary visibility with
   texturing is under 0.3 ms per megapixel. Everything else — sun, lamps, moons, sprites, water — is
   0.2–1.3 ms each.
4. Two things that should not be on the frame at all are: the terrain composite bake (1.2–2.4 ms a
   frame for twenty seconds after every cell load, and a ~15 ms spike every time one finishes) and
   `SceneBuffers::shade()`, which re-uploads every material, every layer and every terrain mask on
   every frame any material animates (≈1 ms, 16 % of the game's CPU).
5. A cell crossing costs 180 ms on average and 344 ms at worst on the island route. The sheet fold
   alone is ~125 ms of that.

---

## 1. Method

| what | how |
|---|---|
| harness static | `openmw-rtxtool bench --validation=false --window=false` from `build-release/`, 600 frames after 60 of warm-up, suites `exteriors`, `interiors`, `default`; JSON and text in `scratchpad/bench/` |
| harness travel | `bench --suite=streaming` (island-crossing, 12 000 u/s, 19 crossings) |
| game | `OPENMW_RTX_BENCH=10s:2s openmw --skip-menu --load-savegame bench_*.omwsave`, with `--config <dir>` holding `[RTX] validation = false` |
| CPU | `apps/rtxtool/profile.sh` (perf, `task-clock` at 5999 Hz, frame pointers, control fifo around the measured frames); the game with `perf record -p` attached 11 s in for 8 s |
| GPU | the renderer's own timestamp zones (`GpuTimer`, median of the run) and `shot --repeat=200` for A/B; **30 repeats is not enough** — the clock has not settled and the same shot varies by ±1 ms. At 200 it repeats within 0.5 %. |
| shader A/B | one-line edits to `shaders/lib/*.glsl`, `cmake --build --target openmw-rtx-vulkan-shaders`, shot, `git checkout` — `scratchpad/shader-ab.sh`; the tree is clean afterwards |

**Caveat on the user config.** `~/.config/openmw/settings.cfg` has `[RTX] validation = true`. The game
you play right now runs under the validation layers. The in-game numbers here were taken with an
override; the ones in `.notes/bench.txt` may not have been.

**Caveat on the harness.** A place with no actors does no per-frame walk and no placement (Sadrith
Mora, Dagon Fel): its frame is the GPU alone. The game walks and places every frame everywhere. So
the harness *understates* the CPU side unless residents are present, and it never pays the engine's
own simulation.

---

## 2. Static camera — what a frame costs

### 2.1 Harness, 1920×1080 output, quality (traced 1280×720)

Milliseconds. `frame` is wall, `trace` is the wall around the frame's one submit, `place` the wall
around the placement (which includes a fence wait for refit+TLAS). GPU columns are device time.

| place | instances | frame | p99 | trace | place | GPU trace | RR | refit | TLAS | fps | 1 % low |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| seyda-neen-ship | 8 544 | 12.71 | 28.2 | 6.37 | 3.21 | 3.36 | 1.99 | 0.40 | 0.22 | 78.7 | 35.5 |
| seyda-neen-shore | 13 375 | 16.21 | 25.7 | 5.31 | 4.51 | 2.22 | 2.08 | 0.68 | 0.26 | 61.7 | 38.9 |
| balmora | 8 667 | 12.29 | 28.4 | 5.02 | 3.31 | 2.12 | 1.99 | 0.45 | 0.22 | 81.4 | 35.3 |
| vivec | 10 774 | 16.47 | 32.5 | 6.01 | 3.82 | 3.07 | 1.97 | 0.54 | 0.22 | 60.7 | 30.7 |
| ald-ruhn | 5 599 | 12.93 | 29.3 | 4.80 | 3.81 | 1.87 | 1.98 | 0.65 | 0.20 | 77.3 | 34.1 |
| sadrith-mora (no residents) | 6 063 | 6.41 | 9.1 | 6.39 | 0 | 3.11 | 2.27 | – | – | 156.0 | 110.6 |
| dagon-fel (no residents) | 4 765 | 6.22 | 9.0 | 6.20 | 0 | 2.81 | 2.36 | – | – | 160.8 | 111.8 |
| balmora-mages-guild | 1 239 | 9.74 | 12.2 | 7.73 | 1.24 | 4.76 | 2.03 | 0.27 | 0.17 | 102.7 | 82.2 |
| seyda-neen-customs | 869 | 7.66 | 10.1 | 6.07 | 0.95 | 2.95 | 2.07 | 0.21 | 0.16 | 130.5 | 99.2 |
| vivec-canalworks | 548 | 7.01 | 9.0 | 4.65 | 0.83 | 1.68 | 2.01 | 0.17 | 0.13 | 142.8 | 110.6 |
| addamasartus | 637 | 6.94 | 9.4 | 5.13 | 1.09 | 2.05 | 2.06 | 0.27 | 0.17 | 144.1 | 106.2 |
| arkngthand | 999 | 7.84 | 10.3 | 6.35 | 0.90 | 3.31 | 2.09 | 0.20 | 0.17 | 127.6 | 97.3 |
| andrano-tomb | 959 | 6.83 | 9.2 | 4.74 | 1.02 | 1.76 | 2.03 | 0.23 | 0.15 | 146.4 | 109.1 |

Every frame also pays waves 0.19–0.23, bloom 0.11, tone 0.04, exposure 0.03, composite 0.02 ms.

Read the ship row: the GPU is busy 6.4 ms (3.36 + 1.99 + 0.40 + 0.22 + 0.4 of small passes) and the
frame is 12.7. The other 6.3 ms is CPU that the GPU waits for: the walk (~2.5), the placement (3.2,
of which ~0.6 is the fence wait), the hand-over. Where nothing moves and nothing is re-walked
(Sadrith Mora) the frame *is* the GPU: 6.4 ms, 156 fps. That gap is the single largest number in
this document.

### 2.2 Game, window 1994×1366, quality (traced 1329×911)

| save | instances | frame | p99 | trace | GPU trace | RR | refit | TLAS | fps | 1 % low |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bench_seyda_need | 4 900 | 14.33 | 29.1 | 7.34 | 3.51 | 2.67 | 0.36 | 0.21 | 69.8 | 34.4 |
| bench_balmora | 4 540 | 13.23 | 28.3 | 5.57 | 1.79 | 2.66 | 0.37 | 0.21 | 75.6 | 35.3 |
| bench_balmora_mages_guild | 1 233 | 13.16 | 15.8 | 10.67 | 6.89 | 2.78 | 0.28 | 0.17 | 76.0 | 63.4 |

Same shape. Seyda Neen: GPU busy ≈ 7.3 ms, frame 14.3. Perf on the main thread over 8 s gives 6.6
ms of CPU per frame, split as: graph walk 1.7, terrain bake 1.2, `SceneBuffers::place` 1.0 (almost
all of it one `memcpy`, see §4.3), engine simulation 0.7, TLAS/refit prep 0.1, texture describe 0.1,
GUI collect 0.1, the rest in small pieces. The CPU is 47 % busy and the GPU about 50 % busy, and
they take turns.

### 2.3 Upscale modes and the 4K target (default suite)

| mode | traced | ship frame | GPU trace | RR | guild frame | GPU trace | RR |
|---|---|---:|---:|---:|---:|---:|---:|
| off (wavelet) | 1920×1080 | 17.89 | 7.58 (+2.83 filter) | – | 17.78 | 11.12 (+3.38) | – |
| performance | 960×540 | 10.24 | 1.95 | 1.16 | 6.55 | 2.54 | 1.16 |
| balanced | 1114×626 | 11.44 | 2.56 | 1.58 | 8.16 | 3.72 | 1.59 |
| quality | 1280×720 | 12.71 | 3.36 | 1.99 | 9.74 | 4.76 | 2.03 |
| dlaa | 1920×1080 | 20.00 | 7.88 | 4.46 | 19.16 | 11.26 | 4.64 |
| **4K performance** | 1920×1080 → 2160p | **20.89 (47.9 fps)** | 7.94 | 4.47 | **20.25 (49.4 fps)** | 11.57 | 4.64 |
| 4K quality | 2560×1440 → 2160p | 32.72 | 15.22 | 8.12 | 34.09 | 20.99 | 8.47 |
| 4K off | 3840×2160 | 57.07 | 34.60 (+11.7) | – | 68.45 | 48.51 (+14.0) | – |

At 4K the post passes grow too: bloom 0.46, tone 0.31, exposure 0.28, composite 0.20 ms.

Three facts fall out:

- **The trace is linear in pixels**: 3.65 / 3.83 / 4.17 ns per pixel on the ship at 0.9 / 2.1 / 8.3
  Mpx, 5.2 / 5.6 / 5.85 ns in the guild. The interior costs *more* per pixel than the exterior.
- **Ray Reconstruction costs 2.2 ms per megapixel of input**, regardless of output size (1.16 →
  1.99 → 4.47 → 8.12 ms for 0.52 → 0.92 → 2.07 → 3.69 Mpx). Preset E costs the same as D.
- **At the target the GPU alone is 14.5 ms (ship) and 18.3 ms (guild).** The budget is 16.7. The
  trace has to lose 3–6 ms at 1080p and the CPU has to leave the critical path.

### 2.4 What each feature costs (harness A/B, default settings)

| switch | ship frame | ship place | ship GPU trace | note |
|---|---:|---:|---:|---|
| baseline | 12.71 | 3.21 | 3.36 | |
| `--people=false` | 10.58 | 2.35 | 3.33 | −2.1 ms, of which 0.9 is placement and ~1.2 the walk posing 423 actor drawables; GPU unchanged |
| `--props=false` | 12.65 | 3.18 | 3.33 | emitters cost nothing here |
| `--clothes=false` | 12.35 | 3.01 | 3.34 | |
| `--distant-statics=false` | 12.13 | 3.12 | 3.33 | **zero GPU cost**; 0.6 ms of CPU for 1 552 instances |
| `--distant-cells=2` | 11.38 | 2.07 | 3.34 | |
| `--distant-cells=8` | 14.43 | 3.42 | 3.62 | +0.26 ms GPU, +1.7 ms CPU for +1 617 instances |
| `--delight=0` | 12.74 | 3.18 | 3.34 | the shading-map divide is free |
| `--distant-terrain=false` | 10.62 | 1.92 | – | p99 13.7, 1 % low 72.9 |
| `--warmup=25` (bake finished) | 11.31 | 2.04 | 3.38 | p99 16.4, 1 % low 61.1 — see §4.2 |

The instance count is a CPU number and nearly not a GPU one: between 7 241 and 10 161 instances the
frame grows 3.05 ms and the trace 0.28. That is about **1 µs of CPU per instance per frame** — the
walk visiting it, hashing its node path, the map lookup, the transform compare, the record, the
TLAS row, the table row.

### 2.5 Weather and time of day (ship / shore, GPU trace ms)

| | ship | shore |
|---|---:|---:|
| Clear, noon | 3.36 | 2.22 |
| Foggy | 2.74 | 2.04 |
| Rain | 3.89 | 2.83 |
| Thunderstorm | 4.54 | 3.41 |
| Clear, midnight | 3.07 | 2.21 |
| **Clear, 06:30** | **6.22** | 2.62 |

Dawn nearly doubles the trace on the ship: the sun is low, the fog's phase function is large over
most of the frame, so every pixel spends its eight fog shadow rays, both moons are still up and get
a shadow ray each at every hit, and the sun's shadow rays run horizontally through every canopy in
Seyda Neen. A storm adds 1.2 ms of oriented rain quads marched per pixel. Foggy weather is *cheaper*
than clear: rays end sooner and fewer bounces find anything.

---

## 3. Inside the trace (shot, 200 repeats, GPU `trace` zone, ms)

Baseline: ship 3.54, guild 4.80, shore 2.07, ship at 06:30 6.30. Each row removes one thing.

| removed | ship | guild | shore | dawn | share of baseline |
|---|---:|---:|---:|---:|---|
| **fog march** (`fogWeatherAlong` → none) | 1.98 (−1.56) | 1.65 (−3.16) | 1.44 (−0.63) | 3.10 (−3.20) | **44 % / 66 % / 30 % / 51 %** |
| fog at 12 steps, 4 shadow rays | 2.82 (−0.72) | 3.30 (−1.50) | 1.82 (−0.25) | 5.11 (−1.19) | half the fog cost |
| fog sun shafts only | 2.70 (−0.84) | 4.47 (−0.33) | 1.86 (−0.21) | 4.09 (−2.21) | |
| **bounce** (`bounceLight` → 0) | 2.18 (−1.36) | 3.33 (−1.47) | 1.24 (−0.82) | 4.44 (−1.86) | **38 % / 31 % / 40 % / 30 %** |
| lamp shadow ray | 2.85 (−0.69) | 3.50 (−1.30) | 1.87 (−0.20) | 5.57 (−0.73) | 27 % in the guild |
| sun shadow ray | 3.01 (−0.53) | 4.50 (−0.30) | 1.74 (−0.33) | 5.53 (−0.77) | |
| `skyReaching` at the bounce hit | 2.98 (−0.56) | 4.80 (0) | 1.79 (−0.28) | 5.88 (−0.42) | |
| alpha test in the candidate loop | 3.06 (−0.48) | 4.53 (−0.27) | 1.73 (−0.34) | 4.56 (−1.74) | 28 % at dawn |
| sprites | 3.19 (−0.35) | 4.45 (−0.35) | 1.89 (−0.18) | 5.93 (−0.36) | |
| moons | 3.11 (−0.43) | 4.32 (−0.48) | 1.80 (−0.27) | 5.05 (−1.25) | see below |
| `textureSize` per fetch | 3.37 (−0.17) | 4.80 (0) | 2.08 (0) | 6.29 (0) | |
| water shaft march | 3.29 (−0.25) | 4.81 (0) | 1.90 (−0.17) | 6.31 (0) | |
| water shore ray | 3.32 (−0.22) | 4.76 | 1.95 (−0.12) | 6.19 (−0.11) | |
| `--albedo` (no surface shading at all; fog and sprites stay) | 2.07 | 3.22 | 1.22 | 3.79 | |

So on the ship: primary + texturing ≈ 0.2 ms, fog 1.6, bounce 1.4 (this includes the sun ray, lamp
ray and sky ray at the bounce hit), sprites 0.35, lamp 0.7, sun 0.5, sky 0.6. The pieces do not add
to the whole because removing one removes the rays the others traced from it.

Three things to read out of the table:

- **The fog march is the largest single cost everywhere**, and in an interior it is two thirds of
  the trace. The guild's fog is *uniform* (`mFogUniform = 1`, so `fogShape` is never evaluated) and
  has no sun and no moons — the 3.2 ms is 24 steps × (`exp` + `weighLamps` over every lamp in the
  grid cell + a random draw). The march is paying for a noise field it does not read and a sun it
  does not have.
- **Removing the moons' code saves 0.4–0.5 ms in the guild, where no moon ray is ever traced.** That
  is register pressure and code size, not work: the shader is occupancy-bound, and dead paths cost
  live pixels. Specialising the kernel per situation will pay in the same way.
- **The micromaps are barely helping.** `shot` reports 3.1 % opaque, 3.1 % transparent, **93.8 %
  "still asking"** at the ship (28 % / 1.5 % / 70 % in the guild). Nearly every cutout candidate
  still runs the any-hit shader with a texture fetch — 0.5 ms at noon and 1.7 ms at dawn.

---

## 4. The CPU side

### 4.1 Where the walk goes

Harness ship (0.46 cores busy): `osg::Group::traverse` 7.9 % self / 47 % total, `addDrawable` 4.8 %,
`RigGeometry::cull` 4.3 %, `memmove` 4.4 %, `resolveMesh` 3.3 %, `__dynamic_cast` 3.3 %, `retire()`
1.6 %, hashtable 3.9 %. Game Seyda Neen: `Group::traverse` 6.6 % self, `addDrawable` 2.4 %,
`dynamic_cast` 2.9 %, `StateSet::getUniform(std::string)` 1.0 % (`readActorFade` builds two
`std::string`s per drawable per frame and does two map lookups by string), `retire` 1.4 %.

The walk is a full traversal of a graph that is 99 % static, every frame, to discover that it is
static. It costs 1.7 ms at 4 900 instances in the game and ~2.5–3 ms at 8 500 in the harness.
`Terrain::QuadTreeWorld::collect → handOver` is 12 % of the game's CPU at a standing camera: the
chunk walk plus `loadRenderingNode`.

### 4.2 The terrain composite bake is on the frame

`CompositeQueue::drain` runs 16 rows per frame from `SceneUploader::hand`, on the main thread. A
512² composite is 32 drains. Seyda Neen at five cells has 39 flattened chunks, so the bake trickles
for **1 248 frames — twenty seconds — after every arrival**, at 1.2 ms a frame in the game (16.6 % of
its CPU) and 2.4 ms in the harness (21 %). Every ring a crossing brings restarts it.

And **every completion is a spike**: the finished composite takes a texture slot, becomes an
arrival, and the next `hand` goes through `SceneTextures` → `extendScene` → a staging allocation, a
copy, a `Batch::flush` — a full queue submit and fence wait — plus `buildChain`. That is the 18–19
ms `place` p99 on every exterior row of §2.1 and the 28–32 ms frame p99, and it is why the 1 % low
is 35 fps where the median is 79. With `--warmup=25` — long enough for every bake to finish before
measuring — the ship goes 12.71 → 11.31 ms median, p99 28.2 → 16.4, 1 % low 35.5 → 61.1.

The project's own rule already names this: work that cannot be made cheap belongs off the frame
path, not on a rota.

### 4.3 `SceneBuffers::shade()` re-uploads the world's masks every frame

The hottest single symbol in the game profile is `memmove` at **16.1 % of all CPU**, and its
caller is `SceneBuffers::shade()` (tail-called from `place`, so perf lands the return address on the
`call shade` in `place`). `shade` rewrites `mMaterials`, `mLayers` **and `mMasks`** whole whenever
`getShadingRevision()` moved — and it moves every frame, because `resolveMaterial` calls
`setMaterial` for every animated state set it meets (a flipbook, a scrolling UV, an alpha
controller) and any cell has one. The masks are every terrain chunk's blend maps widened to floats
— megabytes — memcpy'd into write-combined BAR memory once a frame to change nothing. ≈1 ms a frame
in the game.

### 4.4 Refit and the skinned copies

Every deforming drawable (106 in the game's Seyda Neen, 179 in the harness's) is re-posed by
`RigGeometry::cull` on the CPU, copied into the scene's arrays, copied again into BAR memory
(positions in `prepareRefit`, normals in `SceneBuffers::place`), and its BLAS **rebuilt** every
frame (`MODE_BUILD`, `PREFER_FAST_TRACE`, no `ALLOW_UPDATE`) — 0.4 ms of GPU, whether the actor is
on screen, moving, or standing still in a house two cells away.

### 4.5 Every submit waits

`CommandPool::submitAndWait` is the only way work reaches the queue. A frame is three of them —
placement (refit + TLAS), the frame, the GUI — each allocating a fresh command buffer, each fenced
before the next line of C++ runs. Nothing the CPU does for frame N+1 overlaps anything the GPU does
for frame N. `HostBuffer` and `GBuffer` are written on the assumption of one frame in flight; that
assumption is what has to go.

### 4.6 Small things that are on the frame anyway

- The wave synthesis runs every frame in every cell, water or not: 0.19–0.23 ms of GPU in the
  mages guild.
- The TLAS is rebuilt every frame even when `getMoved()` is empty and nothing refitted.
- `makeInstanceRecords` + `prepareTopLevel` + the instance-row loop touch every one of 8 500 slots
  a frame to rewrite what did not change; the scene already knows what moved.
- `SpriteShade::shade` sorts every non-additive emitter's sprites twice a frame (1.2 % in the game).
- `readActorFade` and `animate` look for uniforms and callbacks on every node every frame.

---

## 5. Travel — the island crossing

`--suite=streaming`, 12 000 units/s, 19 crossings in 600 frames:

| | |
|---|---|
| frame ms median / mean / p95 / p99 / worst | 16.2 / 26.3 / 79.9 / 252 / 360 |
| place ms median / p95 / p99 / worst | 5.5 / 16.5 / 30.5 / 72 |
| crossings | 19, none a rebuild, **344 ms worst**, 3.5 s of the 15.8 s run: 2.2 s reading, 1.3 s building |
| 1 % low | **4.0 fps** |

CPU during the run (0.73 cores): **`SheetFold::fold` 13.2 % self + its sort 4.2 % + `canonical` 1.8
% + `blockSum` 1.9 % ≈ 21 %** — about 125 ms per crossing spent canonicalising and sorting every
triangle of every arriving mesh to find doubled sheets, including 65×65 terrain chunks and
object-paging merges of tens of thousands of triangles. `Terrain::ObjectPaging::createChunk` 10 %
(the harness has no preloader; the game builds these off-thread). `extendScene` 11.9 %
(structures 2.3, micromaps 1.6, textures). `CompositeQueue::gather` 6.3 %, of which
`ShadingMap::ShadingMap` 5.8 % — the painted-light estimate is re-read off every ground texture's
largest level at every gather. Bake 7 %. `memmove` 10 %.

The reading half (2.2 s) is the harness's synchronous NIF/terrain load; the game preloads. The
building half (1.3 s = 70 ms a crossing) is this renderer's own and is the same in the game:
describing textures, estimating shading maps, folding sheets, building structures, uploading — all
on the frame, all under a fence.

---

## 6. Recommendations

Ranked by frame time bought per unit of work. Gains are for 1920×1080-quality unless said; at the
4K target the GPU numbers scale ×2.3 and the CPU numbers do not.

### 6.1 Structural — no picture changes

1. **Pipeline the CPU against the GPU.** Two frames in flight: the walk and placement of frame N+1
   run while the GPU traces frame N. Double-buffer every table the frame rewrites (`HostBuffer`
   rings for instance rows, TLAS rows, lights, sprites, tiles, the constants block), two TLAS
   storages, a fence per frame instead of per submit, and one command buffer per frame reused
   rather than allocated. `mPreviousCamera`, the history reset and `GBuffer::begin`'s barrier
   already assume the write-after-read hazard and only need the frame index. **Gain: the frame
   becomes max(CPU, GPU) instead of their sum — ship 12.7 → ~7 ms, game Seyda Neen 14.3 → ~8,
   4K-performance ship 20.9 → ~15.** This is the largest single lever and the precondition for
   the CPU ever mattering less than the GPU.

2. **Take the terrain bake off the main thread**, or onto the GPU. A composite is a compute
   dispatch over the layer stack (the shader already knows how to sum layers at a hit); baking it
   in a compute pass at arrival is under a millisecond of GPU and no CPU. Failing that, hand
   `TerrainComposite::bake` to the engine's work queue and only `swap` the finished bytes in on
   the frame. And make the arrival path not wait: allocate staging from a persistent ring and
   record the copy into the frame's own command buffer. **Gain: −1.2 to −2.4 ms a frame for 20 s
   after every load, p99 28 → 16 ms, 1 % low 35 → 61 fps.** The measurement in §4.2 is the whole
   proof.

3. **`shade()` writes what changed.** Keep a per-material dirty bit (or a range) and rewrite that
   row; write `mLayers` and `mMasks` only on an arrival, never on an animation. **Gain: ~1 ms a
   frame in the game (16 % of its CPU).** One afternoon.

4. **Update, do not rebuild.** Build deforming BLASes once with `ALLOW_UPDATE` and refit with
   `MODE_UPDATE`; skip the refit entirely for a drawable whose pose did not change (compare the
   skeleton's traversal number, or hash the first few vertices); skip the TLAS build when
   `getMoved()` is empty and no refit happened. **Gain: 0.3–0.5 ms of GPU and most of the two
   vertex copies.**

5. **Skip the wave pass when `mWaterLevel` is −∞**, and run it at half rate otherwise (a sea at
   30 Hz is not visible). **Gain: 0.2 ms in every interior.**

6. **Make the walk incremental.** The graph does not tell the mirror what changed, but it does
   tell it what *can* change: a subtree with no update callbacks below it
   (`getNumChildrenRequiringUpdateTraversal() == 0`), no skeleton, no particle system and an
   unchanged parent transform is the same subtree as last frame. Cache its extraction — the list
   of (slot, local transform) — keyed on the group, and re-place from the cache without visiting.
   Fall back to the full walk for everything under an actor. Cheaper first steps in the same
   direction: read `actorFade` once per state set per epoch instead of by `std::string` per
   drawable; replace the six `dynamic_cast`s per node with one `className()` pointer compare or
   `osg::Object` type queries; stop recomputing `identify()` for drawables whose parent group was
   cached. **Gain: 1–2.5 ms of CPU a frame, and it shrinks the per-instance cost that makes
   `distant land cells` a CPU dial.**

7. **Crossings.** Fold sheets once per source drawable at load, on the preloader thread, not per
   merged chunk on the frame; use a hash set of canonical corner triples (O(n)) instead of a sort
   (O(n log n)). Cache `ShadingMap` per texture path for the life of the process. Build arriving
   BLASes and upload arriving textures on an async compute/transfer queue and swap them in when
   the fence signals, keeping the old ring traced until then. **Gain: the 344 ms worst crossing
   becomes tens of ms of hitch; the per-crossing bake trickle goes with §6.1.2.**

### 6.2 Quality for speed — trades worth making

The trace has to lose 3–6 ms at 1080p to hit 4K/60. These are ordered by ms bought per unit of
picture lost, with the measurement each rests on.

1. **Replace the per-pixel fog march with a froxel volume.** A 1/8-resolution frustum grid
   (e.g. 240×135×64 at 1080p) that integrates the same field, the same lamps and the same eight
   sun probes once per froxel per frame, with temporal reprojection, then one trilinear fetch per
   pixel in the trace. This is what every shipping volumetric does; at the fog's 900-unit grain a
   froxel is far finer than a bank. **Gain: −1.5 ms ship, −3.2 ms guild, −3.2 ms at dawn (44–66 %
   of the trace).** The largest GPU lever there is.

   Cheaper halves of the same thing, if the volume is a later milestone:
   - **Interiors take the closed form.** Uniform extinction, no sun, no moons, no wind: the
     transmittance and the ambient in-scatter are `exp` and a subtraction, and the lamps are the
     only thing left to integrate — one analytic point-light in-scatter per lamp (the standard
     `atan` form for inverse-square along a ray, windowed) plus the one shadow ray the reservoir
     already buys. No steps at all. **−3 ms in the guild, exact rather than sampled.**
   - **Halve the steps outdoors** (12 steps, 4 probes): **−0.7 ms ship, −1.2 ms dawn**, more noise
     that RR is already removing.
   - **Weigh lamps at the probes, not at the steps**: 8 evaluations of `weighLamps` per pixel
     instead of 24. Most of the interior cost is this loop.

2. **Trace the bounce at half resolution.** One diffuse bounce (and its sun, lamp and sky rays)
   per 2×2 quad, shared by the four pixels through the albedo demodulation RR already does; or
   checkerboard it across frames. **Gain: −1.0 ms ship, −1.1 guild, −1.4 dawn.** Ray
   Reconstruction is built for exactly this input.

3. **Drop `skyReaching` for an ambient-occlusion proxy at the bounce hit**, or trace it every
   other frame. **−0.3 to −0.6 ms outdoors, nothing indoors.**

4. **Shadow rays through cutouts at a coarser mip / a cheaper test**, and **make the micromaps
   cover the canopy**: 94 % of candidates still ask. Raise the subdivision level, or classify at
   the level the shadow ray reads, or accept a 2-state map on the alpha's own coarse level. At
   dawn the alpha test is 1.7 ms. **−0.3 ms noon, −1.5 ms dawn.**

5. **One lamp shadow ray per pixel, not one per hit.** Keep the reservoir at the primary hit and
   let the bounce hit take the lamp unshadowed (a one-bounce-deep bias no eye finds). **−0.4 to
   −0.7 ms, most in interiors.**

6. **Specialise the kernel.** Three pipelines from one source by specialization constants:
   *exterior day*, *exterior night*, *interior* — with the moons, the sun, the clouds, the
   sea, the noise field and the water column compiled out where they cannot apply. The `no-moons`
   row (−0.5 ms in a room with no moon) says what dead code costs a register-bound shader. **−0.5
   to −1.0 ms, no picture change at all.** Pairs with the closed-form interior fog.

7. **Run at balanced instead of quality** if a mode has to be picked today: 87 fps vs 79 on the
   ship, 122 vs 103 in the guild. At the 4K target *performance* is already the plan.

### 6.3 Simplifications the numbers permit

- **Distant statics are free on the GPU** (§2.4). Keep them. Their 0.6 ms is the CPU walk and
  goes with §6.1.6.
- **`distant land cells` is a CPU dial, not a GPU one**: 2 → 8 cells costs 0.3 ms of trace and 3
  ms of walk. Once the walk is incremental the setting can be raised without a frame-time
  argument against it.
- **The de-lighting divide is free** — keep it on unconditionally and drop the A/B path from the
  frame if it costs a branch.
- **`textureSize` per fetch is free** (0–0.17 ms). Not worth a table.
- **The water's shaft march and shore ray are cheap** (0.1–0.25 ms). Leave them.
- **The wavelet path is only for references**: at 4K it is 12 ms of filter on top of a 35 ms
  trace. Nothing about it needs to be fast.
- **Ray Reconstruction preset D and E cost the same.** Pick by picture.
- The one thing that is *not* worth generalising is the composite bake into "shade the layer stack
  at the hit": the measurement is unclean (the bench never finishes baking, `shot` always does),
  but `distant-cells=2` against 5 moved the trace 0.02 ms, so the composite buys little on the
  GPU today. Baking on the GPU (§6.1.2) keeps the option and removes the cost either way.

---

## 7. What to do first

1. `shade()` dirty rows (a day), waves skip (an hour), TLAS skip when still (an hour). Together
   about −1.5 ms a frame in the game, no risk.
2. Bake off the frame and the arrival without a wait. The 1 % low doubles.
3. Frame pipelining. The frame becomes the GPU's.
4. Interior closed-form fog + kernel specialisation, then the froxel volume outdoors. The trace
   drops to the size the target needs.
5. Incremental walk, then crossings.

After 1–3 the harness ship should sit near 7 ms and the game near 8; after 4 the 4K-performance
frame should fit in 16.7 ms with room for the moons at dawn.

---

## Appendix — where the raw data is

`/tmp/claude-60354/-home-xxorza-Projects-openmw/8ae6a706-c27d-4d42-aa30-7e54f3ccd100/scratchpad/`:
`bench/*.txt|json` (every harness run, by the names in the tables), `bench/game-*.log`,
`perf-ship/`, `perf-guild/`, `perf-stream/`, `perf-game/` (perf data and the four reports each),
`shaderab/results200.txt` (the A/B rows) and `shader-ab.sh` (reproduces them), `run-bench.sh`,
`cfg/settings.cfg` (the validation override the game runs were made with).

Ashstorm could not be measured: the harness refuses it because `openmw.cfg` lacks
`Weather_Ashstorm_Sky_Sunrise_Color` — the ini importer has not been run for it.
