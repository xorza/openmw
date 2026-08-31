# Review of the RTX test suite

Scope: `apps/components_tests/{rtx,rtxtool,surface}` — 80 files, ~26,700 lines, 518 tests in 91
gtest suites. A fresh build runs them all green in 21.3 s on this machine, none skipped (it has
both the GPU and the game data).

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only. The plan at the bottom orders the work; strike its steps as they land.

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

- [ ] The suite spends a third of its time in four tests: `RtxDlssTest.anUpscaledFrame…`
      3.4 s, `RtxUpscalerStabilityTest.aStillCamera…` 2.4 s,
      `RtxRetireTest.aCompactedScene…` 1.7 s, `RtxDlssTest.rayReconstruction…` 0.9 s. None of
      it is a world reopen any more, so no cache buys any of it back — these are frames the
      device actually draws. Under the 30 s flag today, and worth knowing before adding to
      these suites.

## Plan

Steps 1–3 landed. What is left is the splits, which import the fixtures in `rtx/harness.hpp`
(`DeviceTest`, `RendererTest`) and `rtxtool/installation.hpp` (`InstallationTest`). After every
step: build `components-tests`, run `--gtest_filter='Rtx*:Surface*'`, all green, note the time.

4. **Split `visibilitypass.cpp` along its domains** into `rtx/visibility/`: a fixture header
   deriving from `Testing::RendererTest` and carrying the file's ~600 lines of helpers
   (`countHits`, `renderRadiance`, `renderFiltered`, `inSceneOrder`, `encodeSrgb`/`decodeSrgb`,
   `meanOf`/`contrastOf`, the wall and the water columns), plus one file per domain group
   (camera+jitter+motion, surfaces+mips+panes, sun+moons+sky, water, fog, sprites+worldedge,
   filter+exposure+history, framecost). The small standalone suites ride with their nearest
   domain file. CMakeLists names the new files; test names do not change.
5. **Split `sceneextractor.cpp` by its helper clusters** (materials, skinning, particles,
   lights, retire) the same way. It renders nothing, so what it shares is a scene-graph helper
   header rather than a fixture.
6. **Move suites to the files their production code names**: SkyBuilder tests out of
   `frameworld.cpp` into `skybuilder.cpp`; the three lamp suites out of `lightbuilder.cpp` if
   it is being touched anyway; `view.cpp`'s four suites likewise — lowest value, do last or
   leave.
