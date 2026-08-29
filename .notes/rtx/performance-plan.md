# RTX performance plan

The route from `performance.md` (2026-08-29) to the 1920×1080 → 3840×2160 at 60 fps target, as
ordered steps. Each step names what the code does today, what changes, and the number that says
it worked. The numbers quoted as "today" are the ship at 1080p quality unless said otherwise.

**Done before this plan** (`4e416fdfe7`): `shade()` writes rows and runs, the sea is skipped where a
frame has no water, the composite bake is on its own thread and an arrival rides the placement's
submit. Ship 1 % low 26 → 48 fps at the same median; guild 78 → 90 fps.

**The rule this plan lives under.** *Feature-complete first, then fast.* Steps 1–3 and 5 are
structural and change no picture, so they can land whenever the frame they fix is in front of you.
Everything else waits for the renderer to draw everything the game has, and is measured again then.

**The budget.** At the target the GPU alone is 14.5 ms (ship) and 18.3 ms (guild) against 16.7, and
the CPU adds 3–6 ms in series. So two things have to happen, in this order: the CPU leaves the
critical path (steps 1–4), and the trace loses 3–6 ms at 1080p (steps 5–11). The crossings and the
walk (12–15) are about the 1 % low on a route, not about the median.

---

## 0. A measurement per step, first

Every step below ends in one of these, run from `build-release/` with `--validation=false`:

- `bench --views seyda-neen-ship --seconds 10 --warmup 1 --window=false` and the same for
  `balmora-mages-guild`: `frame`, `place`, `trace` medians and p99, the 1 % low.
- `shot --view <place> --repeat=200`: the GPU `trace` zone for a shader A/B. Two hundred, not
  thirty — the clock has not settled at thirty.
- `bench --suite=streaming --seconds 10`: `crossBuildMs`, the worst crossing, the 1 % low.
- `verify` before and after any step that must not change the picture.

Add one number the bench does not print yet: the walk's wall time per frame (`frame − trace −
place` today, which also hides the harness's own step). Print it as its own column in
`apps/rtxtool/bench.cpp`. Half of the CPU side is the walk and nothing measures it directly.

---

## 1. Placement that costs what moved

**Today.** `SceneAcceleration::prepareTopLevel` (`sceneacceleration.cpp:853`) rebuilds every row of
the instance buffer, asks the driver for the build size, **destroys and recreates the top-level
structure**, and records a build — every frame, whether or not anything moved. `makeInstanceRecords`
inverts every instance's transform every frame (`instancerecord.cpp`), and `SceneBuffers::place`
rewrites every instance row (`scenebuffers.cpp:314`). The scene already knows what moved:
`getMoved()` is the slots whose transform changed since `advancePlacement`.

**Change.**
- Keep the top-level structure and its storage across frames; recreate only when the instance
  count grows past what was sized. The build info is the same every frame — build it once.
- Keep the instance records and the rows persistent, indexed by slot, and rewrite only the slots in
  `getMoved()` (plus the slots `getFreedMeshes` emptied). The row table skips gaps today, so a row
  index is not a slot: either keep a slot→row map, or give a gap a row with `mask = 0` and let the
  builder skip it — the second is simpler and costs the build one masked primitive per gap.
- Skip the top-level build, and the placement submit with it, when `getMoved()` is empty and
  nothing refitted. A standing camera in a place with no actors is that frame every frame.

**Number.** `place` median 2.1 → under 0.5 ms; the harness ship gains ~1.5 ms per frame. `tlas`
GPU zone unchanged. No picture change: `verify` clean.

**Watch.** `advancePlacement` clears `mMoved`; the renderer has to consume it before the walk's
`advance`. `makeInstanceRecords` is shared by the acceleration and the buffers — keep it shared.

## 2. Refit, do not rebuild

**Today.** Every deforming drawable (106 in the game's Seyda Neen) has its positions copied to the
device and its bottom-level structure **built from scratch** each frame — `MODE_BUILD`,
`PREFER_FAST_TRACE`, no `ALLOW_UPDATE` (`sceneacceleration.cpp:632,814`). `getDeformed` names every
rig the walk met, posed or not.

**Change.**
- Build a deforming mesh's structure with `ALLOW_UPDATE` and refit with `MODE_UPDATE`, which is
  what the flag exists for. Keep `MODE_BUILD` for the frame the mesh arrives.
- Only name a mesh deformed when its pose changed: compare the skeleton's traversal number, or hash
  the first few vertices, in `SceneExtractor::resolveMesh` before calling `updateMesh`. An actor
  standing in a house two cells away deforms nothing.

**Number.** `refit` GPU zone 0.40 → ~0.15 ms; the position copy and the normal write go with the
skipped meshes. `verify` on an actor view.

## 3. The walk's cheap wins

**Today.** `readActorFade` (`sceneextractor.cpp:115`) constructs `std::string("actorFade")` and
`std::string("alpha")` and does two map lookups by string **per drawable per frame** — 1 % of the
game's CPU on its own. Every node meets a chain of `dynamic_cast`s (`:416,419,501,539,544,955,
1050,1056,1119,1139`) to find out what it is, every frame. `identify()` hashes the node path per
drawable.

**Change.**
- Resolve the fade uniforms once per state set per epoch: keep the `osg::Uniform*` pair in the
  identity map beside the material, and read the float from it.
- Replace the cast chain with one classification per node kept in the identity map — set on the
  first visit, since a node's class does not change.
- Do not re-identify a drawable whose parent group's identity is already known this frame.

**Number.** The walk column from step 0: ~2.5 ms → ~1.5 ms on the ship. No picture change.

## 4. Two frames in flight

**Today.** `CommandPool::submitAndWait` is the only way work reaches the queue; a frame is three of
them (placement, frame, GUI), each fenced before the next line of C++ runs. Nothing the CPU does
for frame N+1 overlaps anything the GPU does for frame N. `HostBuffer` is written on the
assumption that the last submit finished (`hostbuffer.hpp`). The presenter already keeps two
images and `GBuffer::begin` already orders against the previous composite.

**Change.** The largest single lever, and the one the others are sized against: the frame becomes
`max(CPU, GPU)` instead of their sum.
- One command buffer and one fence per frame slot, two slots; a frame waits for the fence of the
  frame before last.
- Double every table a frame rewrites: instance rows, lights, light grid, sprites, tiles, emitters,
  the top-level instance buffer, the refit positions, the GUI vertices, `mHitCount`, the timer's
  query pool. `HostBuffer` grows a slot index; `SceneBuffers` and `SceneAcceleration` write into
  `slot = frame & 1`.
- Two top-level storages and scratches; the trace binds the one its frame built.
- Deferred destruction: an image `dropTextures` or `TextureArray::write` replaces, and a structure
  `release` frees, go on a list tied to the frame's fence and are destroyed when it signals. The
  pool's deferred staging (from `Batch::defer`) moves to the same list.
- `readPixels`, `readChannel` and the GUI readback wait for the frame they read.
- View traces (dolls, maps) and setup batches stay synchronous: they are not frames.

**Number.** Harness ship 14 → ~8 ms, game Seyda Neen 14.3 → ~8, 4K-performance ship 20.9 → ~15.
Frame p99 must not move up: the double buffers are what keep it flat.

**Order.** After 1–3, so that what has to be doubled is smaller and the CPU half of the frame is
already short. Before 5 onward, so that a GPU saving shows up in the frame time one to one.

## 5. Specialise the kernel

**Today.** One `visibility.comp` serves an interior with no sun, no moons, no sea and no noise
field. Removing the moons' code alone saves 0.4–0.5 ms in a room where no moon ray is ever traced:
the shader is occupancy-bound and dead paths cost live pixels. `COUNT_HITS` is already a
specialization constant (`visibility.comp`, `visibilitypass.cpp:69`), so the mechanism exists.

**Change.** Constants for `HAS_SUN`, `HAS_MOONS`, `FOG_UNIFORM`, `HAS_SEA`, gated from
`VisibilityConstants` at record time. `VisibilityPass` keeps a small table of pipelines keyed by
the tuple and compiles one on first use — half a second each, so compile the three common tuples
(exterior day, exterior night, interior) at the first scene, on the composite queue's pattern of a
thread of its own, and the odd ones on demand.

**Number.** `shot --repeat=200` guild trace 4.8 → ~4.3; ship at dawn −0.5. Byte-identical picture:
`verify` clean.

## 6. Interiors take the closed form

**Today.** `fogWeatherAlong` (`fog.glsl:283`) marches 24 steps with 8 shadow rays per pixel. In the
guild `mFogUniform == 1`, there is no sun and no moon, and the 3.2 ms it costs — two thirds of the
interior trace — is 24 × (`exp` + `weighLamps` over the grid cell + a random draw), sampling a
field that is constant and a sun that is absent.

**Change.** Under `FOG_UNIFORM && !HAS_SUN && !HAS_MOONS` (step 5's constants): transmittance is
`exp(−σd)`, the ambient in-scatter is `colour × (1 − T)`, and the lamps are the only integral left
— one analytic point-light in-scatter per lamp along the ray (the `atan` form for inverse-square,
windowed to `mReach`), with the one shadow ray the reservoir already buys. No steps.

**Number.** Guild trace 4.8 → ~1.8 ms; at the 4K target the guild's GPU falls from 18.3 to ~11,
which is the interior inside budget. The picture is exact where it was sampled: compare against a
`--accumulate` reference, not against the current frame.

## 7. Outdoor fog, the cheaper halves

Only while the froxel volume (step 11) is not yet built.

- Weigh lamps at the 8 probes rather than at the 24 steps: most of the loop is `weighLamps`.
- `FOG_STEPS 12`, `FOG_SHADOW_RAYS 4` behind a constant, A/B'd at 200 repeats: −0.7 ms ship,
  −1.2 ms at dawn, for noise Ray Reconstruction is already removing.

**Number.** Ship trace −0.5 to −0.7 ms; check dawn (`--hour 6.5`) as well as noon.

## 8. Trace the bounce at half resolution

**Today.** `bounceLight` per pixel (`visibility.comp:56`), with its own sun, lamp and sky rays at
the bounce hit: 30–40 % of the trace, 1.0–1.4 ms.

**Change.** One bounce per 2×2 quad, shared through the albedo demodulation Ray Reconstruction
already does — or checkerboarded across frames and reprojected. A half-resolution indirect channel
in `GBuffer`, filled up by the composite.

**Number.** Ship −1.0, guild −1.1, dawn −1.4 ms. Judge with RR on: it is built for this input.

## 9. Make the micromaps cover the canopy

**Today.** 93.8 % of cutout candidates on the ship "still ask" — nearly every one runs the any-hit
shader with a texture fetch: 0.5 ms at noon, 1.7 ms at dawn. `sSubdivisionCeiling = 5`
(`micromap.hpp:72`).

**Change.** First find out why: tally per texture which level the classification ran at against
the level the shadow ray's cone reads, and how much of each map is `unknown` because the alpha sits
in the threshold band. Then either classify at the level the shadow ray reads, accept a two-state
map on the alpha's own coarse level, or raise the ceiling for the textures that are all canopy.

**Number.** `shot` micromap tally: opaque + transparent from 6 % to over 50 %; trace −0.3 noon,
−1.5 dawn.

## 10. Rays the bounce hit does not need

- One lamp shadow ray per pixel: keep the reservoir at the primary hit and let the bounce hit take
  its lamp unshadowed. −0.4 to −0.7 ms, most in interiors.
- `skyReaching` at the bounce hit every other frame, or an occlusion proxy: −0.3 to −0.6 ms
  outdoors, nothing indoors.

Both are one-bounce-deep biases. Judge them against a reference, last among the GPU items.

## 11. A froxel fog volume

**Today.** The per-pixel march is 44–66 % of the trace everywhere, and it integrates the same field,
the same lamps and the same eight sun probes for every pixel of every frame.

**Change.** A frustum-aligned grid — 240×135×64 at 1080p — integrated once per froxel per frame in
a compute pass before the trace, with temporal reprojection against the last frame's volume, and
one trilinear fetch per pixel in the trace. What every shipping volumetric does; at the fog's
900-unit grain a froxel is far finer than a bank. Replaces steps 6–7 outdoors; the interior closed
form stays, because it is exact and cheaper still.

**Number.** Ship −1.5, guild −3.2, dawn −3.2 ms — the largest GPU lever there is. After 4, 5, 6, 8
and 11 the 4K-performance ship should sit near 11 ms with room for the moons at dawn.

## 12. Crossings: fold once, and without a sort

**Today.** `SheetFold::fold` (`sheetfold.cpp:52`) canonicalises and sorts every triangle of every
arriving mesh — 21 % of a crossing's CPU, ~125 ms per crossing — and runs once per merged paging
chunk, so a source drawable merged into many chunks is folded many times.

**Change.** A hash set of canonical corner triples, O(n); fold once per source drawable at load,
keyed in the extractor's identity map, so a merged chunk reuses its sources' answers.

**Number.** `bench --suite=streaming`: `crossBuildMs` 70 → ~30 ms per crossing.

## 13. Crossings: estimate a texture once

**Today.** `SceneTextures` estimates a `ShadingMap` for every texture that arrives, every time it
arrives; a ring that leaves and comes back estimates its textures again. The composite queue keeps
a per-file cache for the baker; the uploader has none.

**Change.** A per-path cache on the uploader for the life of the process — the same shape as the
queue's, keyed by `VFS::Path::Normalized`. A cell's worth is a few hundred kilobytes.

**Number.** `ShadingMap::ShadingMap` leaves the crossing profile.

## 14. Crossings: build arrivals beside the frame

**Today.** Arriving structures and textures are built in the placement's submit, which the frame
waits on. The worst crossing is 344 ms and the 1 % low on the island route is 4 fps.

**Change.** A second queue (async compute, or transfer for the uploads) in `Device`; arrivals are
built there and swapped in when their fence signals, with the old top level traced until then.
Needs step 4's fence-per-frame and deferred-destruction machinery, so it comes after it.

**Number.** The worst crossing becomes tens of milliseconds of hitch; the route's 1 % low 4 → 30+.

## 15. The incremental walk

**Today.** The graph is walked whole every frame — 1.7 ms at 4 900 instances in the game, 2.5–3 at
8 500 in the harness — to discover that 99 % of it did not move. `distant land cells` is a CPU dial
because of it: 2 → 8 cells costs 3 ms of walk and 0.3 of trace.

**Change.** Cache the extraction of a subtree that cannot change — no update callbacks below it
(`getNumChildrenRequiringUpdateTraversal() == 0`), no skeleton, no particle system, an unchanged
parent transform — as a list of (slot, local transform), and re-place from the cache without
visiting. Everything under an actor keeps the full walk.

**Number.** The walk column 1.5 → under 0.5 ms in the game.

**Order.** Last, and possibly never: after step 4 the walk is hidden behind a GPU frame that is
longer than it, and at the 4K target the GPU is 14 ms against the CPU's 3–6. Do it when the CPU
is on the critical path again — a higher `distant land cells`, or a faster trace than this plan
reaches.

---

## The order, in one place

| step | what | gain (ms, 1080p) | picture | after |
|---|---|---:|---|---|
| 0 | a walk column in `bench` | — | — | — |
| 1 | placement costs what moved; keep the TLAS | −1.5 CPU | none | 0 |
| 2 | refit with `MODE_UPDATE`; only posed rigs | −0.25 GPU, copies | none | 1 |
| 3 | the walk's string and cast costs | −1 CPU | none | 0 |
| 4 | two frames in flight | frame = max(CPU, GPU) | none | 1–3 |
| 5 | specialise the kernel | −0.5 GPU | none | 4 |
| 6 | interior closed-form fog | −3 GPU (guild) | exact | 5 |
| 7 | outdoor fog, cheaper halves | −0.7 GPU | noisier | 6 |
| 8 | half-resolution bounce | −1.0 to −1.4 GPU | RR-judged | 4 |
| 9 | micromaps cover the canopy | −0.3 to −1.5 GPU | none | 4 |
| 10 | fewer rays at the bounce hit | −0.7 to −1.3 GPU | biased | 8 |
| 11 | froxel fog volume | −1.5 to −3.2 GPU | reprojected | 6 |
| 12 | fold once, without a sort | −40 ms/crossing | none | 0 |
| 13 | estimate a texture once | crossing CPU | none | 0 |
| 14 | arrivals on a second queue | worst crossing ÷ 10 | none | 4 |
| 15 | the incremental walk | −1 to −2.5 CPU | none | 4, if ever |

After 1–4 the harness ship should sit near 8 ms and the game near 8; after 5, 6, 8 and 11 the
4K-performance frame fits in 16.7 ms with room for the moons at dawn.
