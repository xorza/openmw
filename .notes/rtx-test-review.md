# Review: `apps/components_tests/rtx`

Delete each item when you address it. This file lists open items only.

Scope: `apps/components_tests/rtx/**` — 78 files, 25,988 lines. The review covers structure,
duplication, asymmetry and the accuracy of the comments. It does not question what the tests assert.

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
- [ ] `CountingRenderer::getTextureCount` casts away `const` to reach a non-const accessor
      (`countingrenderer.hpp:66`). Add a const overload of `countAt`.
- [ ] `visibility/fixture.hpp:77` and `:95` are stray blank lines between the opening brace of
      `makeOpenWater` and `makeFlooded` and their first statement.
