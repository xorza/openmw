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


## The harness and the game measure two slightly different frames

What already converges, so nobody checks it twice. Both hosts meet at `Rtx::describeWorld` /
`applyWorld`, `Rtx::exposureBias`, `Rtx::distantLandReach`, the `Sky::*` arithmetic and the shared
`Weather::Precipitation` — one sky, one exposure, one paged radius, one rain. The tool loads the
same `settings.cfg`, builds the same `QuadTreeWorld` with the settings' own numbers, follows the
same two residencies, walks the whole graph every frame (`StagedWorld::EveryFrame`), warms its
emitters, poses actors through the same `SceneUtil` skeleton machinery, and its near plane is the
game's. The items below are what still differs, and each one either skews a bench row or is a
derivation written twice.

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

## Upstream lines that only respell a name

The fork's priority order puts the clean seam first and the smallest upstream diff second.

- [ ] **Narrative design comments sit inside upstream files.** Examples: the rewritten `lerp`
  comment in `weather.cpp` (a comment-only hunk), the long rationale blocks in
  `renderingmanager.cpp`/`.hpp` (~100 added comment lines), and similar blocks in `scene.cpp`,
  `mapwindow.cpp` and `localmap.cpp`. The rationale is good — move it to the RTX-owned side of each
  seam (`sceneframe.hpp`, `renderer.hpp`, `components/sky`, `components/weather`) and keep the
  upstream edits mechanical. Every removed line is one fewer conflict at the next upstream merge.
