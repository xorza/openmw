# Open issues

- `VulkanRenderer::finishFrame` reads `mHitCount` on the host after the frame's fence with no
  barrier naming the host read (`VK_ACCESS_2_HOST_READ_BIT`); the specification says a fence signal
  alone does not make a device write visible to the host.

- The same source draws `balmora-mages-guild` as one of two pictures, differing by up to 21 of 255
  on 1166 pixels that lie in the candles' smoke columns. Seven processes of one build all drew the
  same one, and each of three rebuilds that changed no shader the trace runs — a compute shader, then
  one barrier on the host side — moved every process after it to the other, so `verify --against`
  reports a difference no change to the picture made.
