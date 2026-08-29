# Open issues

- `verify` differs from itself between runs, at worst 1 to 8 of 255 on 0.01% of the pixels of
  `island-crossing`. The same viewpoint is reproducible under `shot` and under `bench`, each with a
  renderer of its own, and a renderer per view inside `verify` does not stop it. What `verify` and
  `bench` share and `shot` does not is `StagedWorld`; what `verify` alone does is stage sixteen
  views into one `World`.

- The same camera renders lit under `shot` and near-black under `bench`. At `19388,-27476,3000`
  looking at `20421,-25763,2300`, `shot --cell=2,-4 --albedo --upscale=off --exposure=1` reads a
  mean of 42.0 of 255 and `bench --suite=streaming` reads 3.2 at its own frame 268. No pixel misses
  in either, the placed count does not move across the frames either side, and the reading is
  identical to the digit with one table copy, with the sweep off, with every composite bake waited
  for, with the upscaler off and with the denoiser off.
