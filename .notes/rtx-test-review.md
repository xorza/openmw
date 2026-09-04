# Review: `apps/components_tests/rtx`

Delete each item when you address it. This file lists open items only.

Scope: `apps/components_tests/rtx/**` — 78 files, 25,988 lines. The review covers structure,
duplication, asymmetry and the accuracy of the comments. It does not question what the tests assert.

---

## One test asserts many independent claims

The first failure hides the rest, and the name cannot say what broke. Several of the names admit it.

- [ ] `scenedesc.cpp:214` `aSkinnedMeshKeepsItsBindPoseAndNamesItselfOncePerPose` carries 50
      assertions across rig tables, bind offsets, pose lists, release, slot reuse and attribute
      clearing.
- [ ] `frameworld.cpp:90` `everyNumberTheWorldDecidesReachesTheFrame` carries 46 assertions.
- [ ] `sceneuploader.cpp:61` `aSceneIsRebuiltThenPlacedThenAppendedToThenRebuiltAgain` carries 38
      assertions and names four separate branches in its own title.
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
