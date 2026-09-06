# Open issues

- Two `bench --views=island-crossing --hashes` runs of one build differ on 30 to
  65 of 360 frames with the upscaler and the denoiser off, measured over two
  pairs. The same walk inside one cell is exact, so what is left is at a cell
  boundary. The scene digest, the bone pose, the camera path and the depth
  channel are all bit-identical, and `Rtx::Channel::Indirect` is what differs.

- Nothing that moves is repeatable through Ray Reconstruction. `one-cell-walk`
  agrees on 19 frames of 360 with it on and on all 360 with it off, and
  `island-crossing` agrees on 5 with a three-second warm-up and 21 with a
  twelve-second one. `verify` turns it off for the same reason.
