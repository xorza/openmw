# Open issues

- `RtxVisibilityTest.theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow` asserts
  `settled < alone * 0.60` and the build measures 0.596, so the bound has under a per cent of
  headroom left. The ratio was 0.563 when the figures beside it were taken.

- `requirements.cpp` lists `VK_EXT_device_fault` as an optional extension, and its comment says it
  turns a device loss into a list of what had not completed. Nothing in the tree calls
  `vkGetDeviceFaultInfoEXT`, so a device loss reports `VK_ERROR_DEVICE_LOST` and nothing more.
