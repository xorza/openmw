# Review: `apps/components_tests/rtx`

Delete each item when you address it. This file lists open items only.

Scope: `apps/components_tests/rtx/**` — 78 files, 25,988 lines. The review covers structure,
duplication, asymmetry and the accuracy of the comments. It does not question what the tests assert.

---

## The shared vocabulary is copied per file instead of taken from a fixture

The same constants, quads and helpers are written out again in file after file. Several of the files
that copy them already include the header that defines them.

- [ ] `Rtx::SceneDesc scene; SceneExtractor extractor(scene);` opens 56 of the 58 extractor tests, and
      `extractor.extract(*root, osg::Matrixf::identity(), 0)` appears 89 times. A fixture holding the
      pair, with a `walk(root)` method, removes both.

### Every file builds its own texture description

A one-texel `TextureData` is the most common object in this suite, and no two files build it the
same way.

- [ ] `MipLevel one{ 0, 1, 1 }` followed by a `TextureData` aggregate is written nine times:
      `visibility/surfaces.cpp:112`, `:264`, `:410`, `:617`, `visibility/sky.cpp:213`, `:279`,
      `:345`, `visibility/light.cpp:235`, `visibility/sprites.cpp:19`.
- [ ] Five structs hold the same three members — bytes, levels, description — and differ only in what
      they paint: `TestTexture` (`visibility/fixture.hpp:224`), `OneTexel`
      (`visibility/sprites.cpp:16`), `CheckerTexture` (`visibility/micromap.cpp:17`),
      `HoleyChainTexture` (`visibility/micromap.cpp:235`), `MaskTexture` (`micromap.cpp:335`). One
      builder with named painters covers all five.
- [ ] A `describe` lambda that wraps a span of texels in a `TextureData` is defined five times, each
      with a different signature: `visibility/surfaces.cpp:113`, `:265`, `:619`,
      `visibility/light.cpp:236`, `visibility/framecost.cpp:265`.

---

## Doc comments no longer describe the code under them

Each of these is a comment a reader will act on and be wrong. They cost nothing to fix and they are
the record this suite is written for.

- [ ] `visibility/surfaces.cpp:15-27` opens with "Parallel rays, and the whole difference between
      them and a pinhole's" and works out hit counts for an orthographic camera. No such test is in
      the file — it is `anOrthographicCameraSendsItsRaysParallelRatherThanThroughAnEye` in
      `visibility/frame.cpp:191`. The orphaned block is merged into `RtxSceneTableTest`'s own.
- [ ] `lightbuilder.cpp:791-795` opens with "Ash and blight blow off Red Mountain at whoever is
      standing in them" and cites `weather.cpp:47`. The test under it is
      `theHourHoldsAnExposureBackAndANoonDoesNot`. The described test is
      `anAshStormBlowsAwayFromRedMountainAndNothingElseTurnsAtAll` at `lightbuilder.cpp:1010`, which
      now has no doc comment.
- [ ] A doc block is split by a blank line, so the first line is not part of it:
      `visibility/water.cpp:103`, `visibility/sea.cpp:665`, `visibility/sprites.cpp:248`,
      `guipass.cpp:220`.
- [ ] `visibility/sprites.cpp:306` leaves a blank line between the doc block and
      `TEST_F(RtxVisibilityTest, aPuffIsLitByItsSideAndByWhatItsTextureLetsThrough)`, so the block is
      detached.
- [ ] `countingrenderer.hpp:19` names `apps/components_tests/rtx/visibilitypass.cpp`. That file does
      not exist; the suite is `apps/components_tests/rtx/visibility/`.
- [ ] `slottable.cpp:48` says `owedBy` returns rows "with the duplicates a repeated write leaves".
      The body erases them with `std::unique` at `:54`.
- [ ] `countingrenderer.hpp:76-79` documents `mDescribedSlots` — "The slots the scene gave back, in
      the order it named them, across every call" — and then declares `dropTextures`. The member the
      comment describes is `mDropped`, declared at `:146`.
- [ ] `countingrenderer.hpp` scatters its data members through its overrides: `mDescribedSlots` at
      `:74`, `mViewTextures` at `:126`, `mViewScenes` at `:133`. Group them.

---

## One test asserts many independent claims

The first failure hides the rest, and the name cannot say what broke. Several of the names admit it.

- [ ] `scenedesc.cpp:214` `aSkinnedMeshKeepsItsBindPoseAndNamesItselfOncePerPose` carries 50
      assertions across rig tables, bind offsets, pose lists, release, slot reuse and attribute
      clearing.
- [ ] `frameworld.cpp:90` `everyNumberTheWorldDecidesReachesTheFrame` carries 46 assertions.
- [ ] `sceneuploader.cpp:61` `aSceneIsRebuiltThenPlacedThenAppendedToThenRebuiltAgain` carries 38
      assertions and names four separate branches in its own title.
- [ ] `lightbuilder.cpp:447` `everyHourAsksOnlyForSettingsTheGameDefines` asserts the hour sweep, the
      allocation count, the fog ramp, the sun irradiance, the disc colour, the wind speed and a
      throw on an unknown weather. The allocation claim in particular belongs on its own.
- [ ] `scenedesc.cpp:789` `releasingAMaterialGivesBackItsLayersAndMasks` carries 29 assertions and
      ends by testing texture slot reuse, which its name does not cover.
- [ ] `visibility/light.cpp:693` `aMeasuredSourceCastsAPenumbraAndAnUnmeasuredOneCastsAnEdge` renders
      the lamp, the point source and the sun in one body over 128 lines. Three fixtures' worth.
- [ ] `dlss.cpp:76` `rayReconstructionBuildsAndResolvesAFlatFrame` asserts the second-instance
      refusal, the probe, four render sizes, the `Off` refusal, the build, the resolve, the epsilon
      floor and the validation log. The comment at `:69` says why they share a setup — a fixture
      holding that setup lets them be separate tests.

---

## `DeviceTest` is worked around rather than extended

The fixture at `harness.hpp:238` exists to hold a device and a command pool. Nine tests take the
device and build the pool themselves, and one rebuilds the whole fixture to change one flag.

- [ ] `visibility/framecost.cpp:19` writes out `DeviceTest`'s skip by hand because it wants the
      unvalidated harness. Give `DeviceTest` a validation flag instead.
- [ ] Nine tests build `CommandPool pool(device)` while inheriting `getPool()`: `wavepass.cpp:45`,
      `:114`, `:195`, `:255`, `waveline.cpp:119`, `:160`, `wavefield.cpp:227`, `probe.cpp:277`,
      `dlss.cpp:121`.
- [ ] `slottable.cpp` uses `getDevice()` in its fixture and `*mHarness->mDevice` in two tests
      (`:227`, `:249`). Pick one.
- [ ] `dlss.cpp:251` `anUpscaledFrameIsTheSameFrameLarger` derives from `DeviceTest` and never
      touches `mHarness`. It builds a device to skip.
- [ ] `visibility/micromap.cpp:69` `RtxMicromapPictureTest` and `visibility/surfaces.cpp:41`
      `RtxSceneTableTest` are the only per-file fixtures over the visibility suite. `RtxSceneTableTest`
      is an empty struct that adds nothing over `DeviceTest`.
- [ ] `harness.hpp` holds every definition inline, including `Once<T>` and the two caches, so all 70
      translation units compile them. Move the bodies to `harness.cpp` and leave the declarations.

---

## Helpers assert and throw on behalf of their callers

A failure raised inside a helper reports at the helper's line, not the caller's, and a helper that
throws reports as an unexpected exception rather than as a located failure.

- [ ] `litThroughPane` calls `EXPECT_GT(countHits(...), 0u)` (`visibility/fixture.hpp:541`), and it
      is shared by three tests. A failure names the fixture line for all three.
- [ ] `paneOverWall` does the same (`visibility/fixture.hpp:579`), shared by
      `visibility/water.cpp:50` and `:84`.
- [ ] `requireFrame` throws `Rtx::Error` (`visibility/fixture.hpp:384`). The comment says an assert
      "says nothing in the build a figure is taken in", which is true of `<cassert>` and not of
      `ASSERT_EQ`. Have `countHits` check the size at its own return instead.
- [ ] `countHits` calls `mRenderer->finishFrame().value()` (`visibility/fixture.hpp:349`). An empty
      optional throws `std::bad_optional_access` with no message. `frames.cpp:87` shows the better
      shape.
- [ ] `visibility/water.cpp:463` `middleOf` calls `ADD_FAILURE()` and returns `0.0f`, and the caller
      then compares that against 175. One fault reports twice.

---

## Consistency defects across the suite

- [ ] Three namespace shapes are in use: `namespace Rtx::Testing` (the extractor and visibility
      suites), `namespace Rtx` (46 files), and a bare anonymous namespace at global scope
      (`alphaimage.cpp`, `cloudshell.cpp`, `distantlights.cpp`, `meantexel.cpp`, `spritelight.cpp`,
      `spriteshade.cpp`, `terraincomposite.cpp`, `terrainresidency.cpp`). The last group forces
      `Rtx::` on every name it uses.
- [ ] `apps/components_tests/CMakeLists.txt` lists the RTX sources out of order:
      `rtx/slottable.cpp` at line 126 sits between `frames.cpp` and `frametimes.cpp`,
      `rtx/spritebinpass.cpp` at 163 follows `spriteshade.cpp`, and `rtx/structurestorage.cpp` at 166
      follows `terrainresidency.cpp`.
- [ ] `dlss.cpp:339` and `:344` restate the two test names by hand in the `#else` branch. A third
      test added to the `#ifdef` branch will silently have no stub.
- [ ] `dlss.cpp` uses `TEST_F(RtxDlssTest, ...)` in one branch and `TEST(RtxDlssTest, ...)` in the
      other, for the same suite name.
- [ ] `CountingRenderer::getTextureCount` casts away `const` to reach a non-const accessor
      (`countingrenderer.hpp:66`). Add a const overload of `countAt`.
- [ ] `visibility/fixture.hpp:77` and `:95` are stray blank lines between the opening brace of
      `makeOpenWater` and `makeFlooded` and their first statement.
