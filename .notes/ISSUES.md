# Open issues

- Vulkan synchronization validation reports nothing for a compute read that overtakes a compute
  write in one command buffer. `openmw-rtxtool doll --sync-validation` was silent while the
  composite pass read the a-trous cascade's last level with no barrier between them, which two runs
  of the same doll showed as a thousand differing pixels.
