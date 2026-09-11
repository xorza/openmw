# Open issues

- `Rtx::Swapchain` treats `VK_INCOMPLETE` from `vkGetPhysicalDeviceSurfaceFormatsKHR` as a failure
  (`components/rtxvulkan/swapchain.cpp:23`). Seen once as
  `openmw-rtxtool: vkGetPhysicalDeviceSurfaceFormatsKHR failed: VK_INCOMPLETE (5)`, which killed one
  leg of a `repeatable.sh --pairs=2` run.
