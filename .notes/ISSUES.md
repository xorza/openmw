# Open issues

- `RtxVisibilityTest.theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow` asserts
  `settled < alone * 0.60` and the build measures 0.596, so the bound has under a per cent of
  headroom left. The ratio was 0.563 when the figures beside it were taken.

- `CLAUDE.md` tells the reader to read `.notes/rtx/openmw.md`, `.notes/rtx/plan.md`,
  `.notes/rtx/backends.md` and `.notes/rtx/performance.md`, and none of the four exists in the tree.
  `performance.md` was deleted in `be0082047b`.
