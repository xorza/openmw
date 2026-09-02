# Open issues

- `RtxVisibilityTest.theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow` asserts
  `settled < alone * 0.60` and the build measures 0.596, so the bound has under a per cent of
  headroom left. The ratio was 0.563 when the figures beside it were taken.

- `requirements.cpp` lists `VK_EXT_device_fault` as an optional extension, and its comment says it
  turns a device loss into a list of what had not completed. Nothing in the tree calls
  `vkGetDeviceFaultInfoEXT`, so a device loss reports `VK_ERROR_DEVICE_LOST` and nothing more.

- The trace at the Balmora mages' guild is 0.03 to 0.06 ms slower with the scene's tables read
  through device addresses than through descriptors, in thirteen interleaved release bench pairs
  with the order rotated. The exteriors show no difference. The compute pipelines compile to
  byte-identical sizes and register counts both ways, and nothing in the tree can show which load
  path the trace pipeline takes.

- `sceneacceleration.cpp` gives the positions, the indices and the instance rows storage-buffer
  usage "because the shader reads the indices back at a hit", but the shader reads the index blocks
  through a reference constructed from an address, and no descriptor names any of the three.

- `shot` on `balmora-storm-night` is not repeatable. Two renders of one build differ by one level on
  about twenty isolated dark pixels, and three renders in a row can also agree. The other nineteen
  views in `views.cfg` repeat byte for byte across every run.
