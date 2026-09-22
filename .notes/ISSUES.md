# Open issues

- A texture wider or taller than the device's `maxImageDimension2D` reaches `vkCreateImage` in `Rtx::Texture` unchecked; nothing between `describeImage` and the device compares a content texture's extent with what the device takes.
- A device allocation that content exhausts (a texture set past the card's memory) throws `Rtx::DeviceError` out of the frame, and the game ends; no texture or mesh upload stands in for one that did not fit.
