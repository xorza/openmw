# Review of the RTX test suite

Scope: `apps/components_tests/{rtx,rtxtool,surface}` — 96 files, ~26,700 lines, 518 tests in 91
gtest suites. A fresh build runs them all green in about 22 s on this machine, none skipped (it has
both the GPU and the game data). The largest file is now `rtx/scenedesc.cpp` at 1,239 lines.

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only.

## Runtime heads

- [ ] The suite spends a third of its time in four tests: `RtxDlssTest.anUpscaledFrame…`
      3.8 s, `RtxUpscalerStabilityTest.aStillCamera…` 2.6 s,
      `RtxRetireTest.aCompactedScene…` 1.9 s, `RtxDlssTest.rayReconstruction…` 0.9 s. None of
      it is a world reopen any more, so no cache buys any of it back — these are frames the
      device actually draws. Under the 30 s flag today, and worth knowing before adding to
      these suites.
