# Open issues

- `RtxVisibilityTest.theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow` asserts
  `settled < alone * 0.60` and the build measures 0.596, so the bound has under a per cent of
  headroom left. The ratio was 0.563 when the figures beside it were taken.

- The trace at the Balmora mages' guild is 0.03 to 0.06 ms slower with the scene's tables read
  through device addresses than through descriptors, in thirteen interleaved release bench pairs
  with the order rotated. The exteriors show no difference. The compute pipelines compile to
  byte-identical sizes and register counts both ways. The driver reports no internal
  representation for any pipeline, capture flag or not, so the load path a kernel took can only be
  read in Nsight Graphics.
