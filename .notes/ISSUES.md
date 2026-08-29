# Open issues

- Staging one view twice in a single process gives two different pictures: `bench
  --views=balmora,island-crossing,balmora,island-crossing --hashes` gives `balmora` the same hash
  twice and `island-crossing` two different ones. The scene differs by one sprite — 350 particles
  against 351 — because an emitter in the shared graph keeps running between stagings. Every other
  path is reproducible: sixteen `verify` views and a 660-frame streaming run.

- The same camera renders lit under `shot` and near-black under `bench`. At `19388,-27476,3000`
  looking at `20421,-25763,2300`, `shot --cell=2,-4 --albedo --upscale=off --exposure=1` reads a
  mean of 42.0 of 255 and `bench --suite=streaming` reads 3.2 at its own frame 268. No pixel misses
  in either, the placed count does not move across the frames either side, and the reading is
  identical to the digit with one table copy, with the sweep off, with every composite bake waited
  for, with the upscaler off and with the denoiser off.
