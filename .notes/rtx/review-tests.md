# Review of the RTX test suite

Scope: `apps/components_tests/{rtx,rtxtool,surface}` — 80 files, ~26,700 lines, 518 tests in 91
gtest suites. A fresh build runs them all green in 22.8 s on this machine, none skipped (it has
both the GPU and the game data).

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only. The plan at the bottom orders the work; strike its steps as they land.

## One world is read from disk once per test

- [ ] 27 tests across `rtxtool/{actor,stability,retire,lamps,props,pagedterrain,crossing,material}.cpp`
      each call `openWorld`, and every call re-reads and merges the full content files.
      The device-side suite caches its instance, device and renderer once per binary
      (`rtx/harness.hpp` + the teardown environment in `harness.cpp`); nothing caches a `World`.
      The reopens carry the tool suites to roughly 7 of the run's 23 seconds
      (PagedTerrain 2.6 s, Crossing 2.5 s, Retire 2.0 s, the rest behind them).
      A cache has one hazard to own: `World` borrows references into the
      `ConfigurationManager` and the `variables_map` it was opened with, so the three must live
      and die together.

- [ ] Every call site repeats the same five lines — `ConfigurationManager`, `variables_map`,
      `openWorld`, null check, `GTEST_SKIP` — and the skip string
      `"no Morrowind installation configured"` is written out 27 times because `openWorld`
      returns null without a reason, where the device harness hands one back.

## The draw-and-read loop is written six times

- [ ] `visibilitypass.cpp`'s fixture owns the render loop (`countHits`, `renderPicture`,
      `renderRadiance`, `renderFiltered`, `inSceneOrder`); `dlss.cpp`, `frames.cpp`,
      `guitextures.cpp`, `gputimer.cpp` and `sceneextractor.cpp` each write their own
      resize/setScene/renderFrame/readback loop of the same shape.

- [ ] Nine files hand-build the same one-quad scene — `frames`, `gputimer`, `sheetfold`,
      `sceneextractor`, `guitextures`, `visibilitypass`, `instancerecord`, `dlss`,
      `scenedesc` — each with its own corner list, index list and builder name
      (`makeWall`, `wall()`, `makeSheet`, a bare `quad`). `dlss.cpp` also re-derives a channel
      `meanOf` beside the one in `visibilitypass.cpp`.

- [ ] The four-line skip preamble (`reason`, get, null check, `GTEST_SKIP`) appears about 40
      times in three flavours (harness, renderer, unvalidated). Some files hold it in a
      fixture's `SetUp` (`visibilitypass`, `frames`); others inline it per test (`gputimer`,
      `dlss`, `structurestorage`, `wavepass`) — two shapes for one thing.

## Files that hold several suites' worth of tests

- [ ] `rtx/visibilitypass.cpp` is 6,890 lines: 78 tests in seven gtest suites (Transform,
      Camera, Jitter, SceneTable, Visibility, CausticGain, FrameCost). Inside
      `RtxVisibilityTest` the tests fall into domains the file already orders them by:
      camera/jitter/motion vectors, textures and mips, sun and panes, moons and the cloud
      deck, water (~20 tests), fog (~8), sprites, the world's edge, filter/exposure/history,
      and the frame-cost guard. What binds them into one TU is ~600 lines of shared helpers
      plus the fixture.

- [ ] `rtx/sceneextractor.cpp` is 2,409 lines, 51 tests, and its helper clusters already name
      the domains: material describe/paint, skinning (`RiggedQuad`, `BoneClock`,
      `DeformingCull`), particles (`Plume`, `emit`, `drive`), light sources, retirement stats.

- [ ] `rtx/frameworld.cpp` holds `RtxFrameWorldTest` and all of `RtxSkyBuilderTest` — the sky
      builder is its own production file (`components/rtx/skybuilder.cpp`), and its tests hide
      under another file's name.

- [ ] `rtx/lightbuilder.cpp` holds four suites (LightBuilder, Skylight, SunAloft, RoomLight);
      `rtxtool/view.cpp` holds four (ProfileLine, WindowTitle, Viewpoint, Views). Both names
      cover half of what the file tests. `scenedesc.cpp` (1,239) is one domain and borderline.

## Runtime heads

- [ ] The suite spends a fifth of its time in five tests: `RtxDlssTest.anUpscaledFrame…`
      3.2 s, `RtxUpscalerStabilityTest.aStillCamera…` 2.5 s,
      `RtxRetireTest.aCompactedScene…` 1.8 s, `RtxDlssTest.rayReconstruction…` 0.9 s,
      `RtxPagedTerrainTest.aPagedWorldStandsStatics…` 0.8 s. Under the 30 s flag today, and
      the world cache above buys ~5 s back — worth knowing before adding to these suites.

## Plan

Order matters: step 1 is independent pure win; steps 2–3 build the shared pieces the splits then
import, so they come before the splits; 4–6 are then mechanical moves. After every step: build
`components-tests`, run `--gtest_filter='Rtx*:Surface*'`, all green, note the time.

1. **Cache the world per binary.** Give `rtxtool/installation` the same shape as
   `rtx/harness`: a `Once`-style cache owning config + variables + world together, a reason
   string instead of a bare null, and a teardown environment that closes it before `main`
   returns. Collapse the 27 call sites to the four-line skip shape.
2. **Extract the shared scenes and probes.** One header beside `harness.hpp` for the quad
   corners/indices, `makeWall`/`makeSheet`-class builders, `encodeSrgb`/`decodeSrgb`,
   `meanOf`/`contrastOf` and the centre-pixel probe. Point the nine local copies at it.
3. **Extract the render-loop fixture.** Move `RtxVisibilityTest`'s `countHits`/
   `renderRadiance`/`renderFiltered`/`inSceneOrder` and its skip-in-`SetUp` into a header the
   other five draw-and-read files can also derive from; fold their private loops into it
   where they are the same loop.
4. **Split `visibilitypass.cpp` along its domains** into `rtx/visibility/`: the fixture
   header plus one file per domain group (camera+jitter+motion, surfaces+mips+panes,
   sun+moons+sky, water, fog, sprites+worldedge, filter+exposure+history, framecost). The
   small standalone suites ride with their nearest domain file. CMakeLists names the new
   files; test names do not change.
5. **Split `sceneextractor.cpp` by its helper clusters** (materials, skinning, particles,
   lights, retire) the same way, sharing what step 2 extracted.
6. **Move suites to the files their production code names**: SkyBuilder tests out of
   `frameworld.cpp` into `skybuilder.cpp`; the three lamp suites out of `lightbuilder.cpp` if
   it is being touched anyway; `view.cpp`'s four suites likewise — lowest value, do last or
   leave.
