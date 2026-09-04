# Open issues

- The first renderer test includes shared Vulkan renderer and pipeline construction in its
  duration. `RtxGpuTimerTest.aFrameAccountsForItsOwnDeviceTimePassByPass` takes 1.793 seconds
  with cached pipelines and 7.481 seconds after shader changes, exceeding the one-second limit.
