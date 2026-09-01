# Review: the whole diff against upstream master

Reviewed at commit e8b073bcd2, against merge base 97c3f81aba. The diff is 657 files and
approximately 96,500 added lines. The review looked for a smaller diff in upstream files, for
dependency reduction, for better component isolation, and for performance redesigns. A second
pass at 3a7c70addb compared `apps/rtxtool` against `apps/openmw/mwrender/rtx/` for benchmark
validity and code reuse, and added the harness group below.

**Delete an item when you address it or reject it.** This file lists open findings and nothing
else.

What the review checked and found sound, so nobody checks it twice: `components/rtx` includes no
graphics API header, and its CMake keeps it that way. The Vulkan frame path is incremental —
changed rows, per-slot graveyards, a frame ring — and rebuilds nothing per frame. The `Renderer`
seam, `OffscreenView`, `Surface::Material`, `Picture`/`RegionTexture` and the `Downpour` lift are
clean seams with one answer each. `.notes/rtx/cpu.md` items B1–B5 and D own the extractor's CPU
work, and `.notes/rtx/shader-review.md` owns the shaders. This file does not repeat their items.

## The frame re-derives facts the engine can state directly (performance redesigns)

The walk itself belongs to `cpu.md` §D and is not repeated. These are the redesigns beside it.

- [ ] **Move skinning to the device.** The animated residents cost ~2.5 ms of CPU per frame
  (`cpu.md` §A, §C). For every deforming drawable, the mirror re-reads the posed vertex arrays,
  compares them against the held copy, copies them into the scene, uploads them, and refits the
  structure (`SceneExtractor::resolveMesh` deforming path, `SceneDesc::updateMesh`). A redesign:
  upload only the bone matrices, and run the skinning in a compute pass that feeds the refit. The
  mirror then treats a rig as a static mesh plus a bone table. The did-it-move test becomes a
  compare of tens of matrices instead of thousands of vertices. This removes the only per-frame
  vertex traffic in the renderer and most of the deforming path's CPU cost. It also removes the
  need to answer `cpu.md` §C's "narrow to actors in view" question — off-view actors become nearly
  free, and shadows and reflections keep them.

- [ ] **`SceneDesc::release` leaks the layer runs and the mask runs of a freed material.** The
  header says so itself ("a freed material leaks its run until the scene is replaced outright").
  A terrain chunk's masks are tens of kilobytes, and a long session in one worldspace never calls
  `clear` — a player can cross the whole continent through one `SceneDesc`. Give the two runs back
  when the sweep frees the material that owns them, the way the material already gives back its
  textures.

## The harness and the game measure two slightly different frames

What already converges, so nobody checks it twice. Both hosts meet at `Rtx::describeWorld` /
`applyWorld`, `Rtx::exposureBias`, `Rtx::distantLandReach`, the `Sky::*` arithmetic and the shared
`Weather::Precipitation` — one sky, one exposure, one paged radius, one rain. The tool loads the
same `settings.cfg`, builds the same `QuadTreeWorld` with the settings' own numbers, follows the
same two residencies, walks the whole graph every frame (`StagedWorld::EveryFrame`), warms its
emitters, poses actors through the same `SceneUtil` skeleton machinery, and its near plane is the
game's. The items below are what still differs, and each one either skews a bench row or is a
derivation written twice.

- [ ] **The two benches put the GPU wait in different rows.** The game's frame runs
  `mUploader.hand` first and `finishFrame` after it (`rtxrenderer.cpp:730,941`), so when the
  device is the wall, `placeScene`'s internal wait for the in-flight frame lands inside
  `place ms` and `wait ms` reads low. The tool deliberately waits first and hands after
  (`bench.cpp:354-364`), so the same stall lands in `wait ms` and `place ms` stays clean. Same
  row names, different meanings — a tool row cannot be read against a game row on a GPU-bound
  view. Give both spines one order (the tool's split is the readable one) and one shared
  measuring helper beside `Rtx::FrameSamples`, so the next drift cannot appear without being
  written down.

- [ ] **The game retires every frame and the tool only on a crossing.** `rtxrenderer.cpp:707`
  sweeps each frame; `stagedworld.cpp:301-304` sweeps only when cells departed. `cpu.md` B4
  prices the per-frame sweep at 1.9% of the profile, so every tool frame under-costs the game's
  by that much. Give the tool the game's cadence — it walks the whole world every frame already,
  which is the precondition `retire` names.

- [ ] **Three far planes for one world.** The game traces every frame at `sFar = 200000`
  (`rtxrenderer.cpp:98`); the tool's `shot` takes eight scene radii (`shot.cpp:86`) and `bench`
  and `view` floor that at ten thousand (`bench.cpp:254`) — `Framing::mFar` records the drift as
  "undecided". `mFar` is not only cost: it is the sun's shadow-ray reach, the unbounded water
  path and the depth encode. Move the game's constant into `components/rtx` beside the camera
  builders, take it everywhere, and delete the field's "undecided" note.

- [ ] **The tool's resource cache expires at nought.** `world.cpp:22` sets `sExpiryDelay = 0`
  where the game runs `Settings::cells().mCacheExpiryDelay`. A route bench's crossings then
  re-read meshes the game would still hold, and the `reading` half of the crossing line is
  inflated by exactly what the cache would have kept. Use the setting.

- [ ] **Active-grid statics are merged in the game and individual in the tool.** The game passes
  `Settings::terrain().mObjectPagingActiveGrid` — default on — so its near statics live in paged,
  merged chunks and `getPagedRefnums` keeps them from being placed twice. The tool pins the flag
  false (`world.cpp:131`), because it has no `Scene` to ask. Same place, different instance
  counts and different structures. Either bench with the game setting off, or record the
  difference beside every A/B that crosses the two hosts.

- [ ] **`makeDaylight` re-derives what `WeatherManager` computes, and nothing pins the two.**
  `readWeather`/`settle` (`lightbuilder.cpp:142-232`) rebuild the weather's colour ramps, fog
  depth and disc from the fallback keys; the game reaches the same numbers through
  `MWWorld::Weather`'s interpolators and `calculateTransitionResult`. Both sit on `components/sky`
  for the arithmetic, but the ramp reading and the transition blend are written twice, and only a
  comment ("exactly the quantities `calculateTransitionResult` blends") holds them together. Add
  a parity test in `apps/openmw_tests` that evaluates both paths at the same weathers and hours
  and asserts equality — or lift the ramp evaluation into `components/sky` so the game reads it
  too.

- [ ] **The paper doll is assembled twice.** `apps/rtxtool/npc.cpp` re-implements the body-part
  assembly `MWRender::NpcAnimation` performs — the slot-to-bone table, the race-and-sex part
  lookup, the garment slot claims — with two documented deltas (the drawn weapon, the one-bone
  weapon slot). The game's copy cannot move: it reads live inventory through `MWWorld`. What can:
  the record-level resolution (slots, bones, part selection) as a component both feed their own
  equipment state into. Until then, a change to how the game dresses a person walks past the
  harness unnoticed.

- [ ] **The texture-unit stand-in `32` is written twice.** `rtxrenderer.cpp:133` and the tool's
  `world.cpp:29` each carry the constant with a comment pointing at the other. One name in
  `components/rtx`, two deletions.

## One fact, two derivations

- [ ] **`Sky::TimeOfDaySettings` is built twice from the same fallback keys.**
  `MWWorld::WeatherManager` fills its `mTimeSettings` field by field
  (`apps/openmw/mwworld/weather.cpp:344-364`), and `Sky::TimeOfDaySettings::fromFallback()`
  derives the identical value — same keys, same four `addSetting` calls, same "Stars" arithmetic.
  The RT path reads `shared()` while the game's weather reads its own copy, so the two can drift
  if either changes. Replace the block in `weather.cpp` with
  `mTimeSettings = Sky::TimeOfDaySettings::shared()`. This also removes ~20 lines from an upstream
  file.

- [ ] **`RenderingManager` mirrors ~20 world facts into members only so `describeWorld()` can copy
  them out again.** The header grew a parallel field list (`mSunPosition` … `mStormParticleDirection`
  in `renderingmanager.hpp`) whose only reader is the per-frame `WorldState` assembly. Hold one
  `WorldState` member instead. Let each setter write its fields into it directly, and let
  `describeWorld()` patch only the per-frame facts (underwater, location, fog readings, game hour,
  weather ids, wind). This deletes the parallel list, shrinks the upstream header diff, and removes
  the failure mode where a new fact is stored but never reported.

## Upstream lines that only respell a name

The fork's priority order puts the clean seam first and the smallest upstream diff second. Both
items below keep the one shared table and shrink the standing diff for future upstream merges.

- [ ] **The vismask lift respelled ~130 lines across ~25 upstream files.** The lift itself is
  right — one table in `components/sceneutil/vismask.hpp`, read by the game, the harness and the
  extractor. But most of the churn in `mwclass/*`, `mwmechanics/actors.cpp`, `worldimp.cpp`,
  `groundcover.cpp`, `renderingmanager.cpp` and the animation family is only
  `MWRender::Mask_X` → `SceneUtil::Mask_X`. Option: keep `apps/openmw/mwrender/vismask.hpp` as two
  lines — `#include <components/sceneutil/vismask.hpp>` and
  `namespace MWRender { using enum SceneUtil::VisMask; }` — and revert the respelling hunks. The
  table stays canonical in one place. The alias is one C++20 line, not a second copy of any bit.

- [ ] **Narrative design comments sit inside upstream files.** Examples: the rewritten `lerp`
  comment in `weather.cpp` (a comment-only hunk), the long rationale blocks in
  `renderingmanager.cpp`/`.hpp` (~100 added comment lines), and similar blocks in `scene.cpp`,
  `mapwindow.cpp` and `localmap.cpp`. The rationale is good — move it to the RTX-owned side of each
  seam (`sceneframe.hpp`, `renderer.hpp`, `components/sky`, `components/weather`) and keep the
  upstream edits mechanical. Every removed line is one fewer conflict at the next upstream merge.

## The core reaches for globals

- [ ] **`components/rtx` reads the game's settings registry in three files.**
  `cloudshell.cpp:264` and `nightsky.cpp:266` read `Settings::models()`, and `distantland.cpp:10-15`
  reads `Settings::rtx()` and `Settings::camera()`. This couples the core to the settings machinery,
  which every host must then initialise. Pass the three values in — the mesh paths through the
  builders' existing reading structs, the two distances through `DistantLand`'s constructor. The
  hosts already own configuration plumbing on both sides.
