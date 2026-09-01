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

- [ ] **Active-grid statics are merged in the game and individual in the tool.** The game passes
  `Settings::terrain().mObjectPagingActiveGrid` — default on — so its near statics live in paged,
  merged chunks and `getPagedRefnums` keeps them from being placed twice. The tool pins the flag
  false (`world.cpp:133`), because it has no `Scene` to ask. Same place, different instance
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

## One fact, two derivations

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
