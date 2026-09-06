# Open issues

- Two `bench --views=island-crossing --hashes` runs of one build still differ on
  about 45 of 360 frames with the upscaler and the denoiser off, and on all but
  five with them on. `Rtx::FrameClock` removed the larger half and made the two
  runs mirror the same world. What is left is not the terrain warming thread,
  whose removal makes it worse (276 of 360 against 317), not the paged statics
  (337) and not the distant ground (326).
