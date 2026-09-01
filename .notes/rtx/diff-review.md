# Review: the whole diff against upstream master

Reviewed at commit e8b073bcd2, against merge base 97c3f81aba. The diff is 657 files and
approximately 96,500 added lines. The review looked for a smaller diff in upstream files, for
dependency reduction, for better component isolation, and for performance redesigns.

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
