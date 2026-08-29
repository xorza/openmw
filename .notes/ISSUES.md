# Open issues

- `verify`'s `island-crossing` differs from itself between runs, at worst 1 to 6 of 255 on 0.01% of
  the pixels, while the other fifteen views are identical. Not the two causes a `bench` run had:
  `verify` states a frame's length like every measured command now does, and it stages its
  composites, so the bake queue never leaves one to a worker thread's timing. Six hundred and sixty
  frames of `bench --suite=streaming --hashes` are identical, so whatever this is does not reach a
  streaming run.

- `bench` reports a frame result for fewer than half its frames: `beginFrame`'s own drain consumes
  the oldest frame when the ring is full, so the `finishFrame` that follows finds nothing and the
  run's wait, GPU and hit rows are sampled from whichever frames it happened to reach.

- The same camera renders lit under `shot` and near-black under `bench`. At `19388,-27476,3000`
  looking at `20421,-25763,2300`, `shot --cell=2,-4 --albedo --upscale=off --exposure=1` reads a
  mean of 42.0 of 255 and `bench --suite=streaming` reads 3.2 at its own frame 268. No pixel misses
  in either, the placed count does not move across the frames either side, and the reading is
  identical to the digit with one table copy, with the sweep off, with every composite bake waited
  for, with the upscaler off and with the denoiser off.
