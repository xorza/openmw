# Open issues

- With the PBR profile (`~/.config/openmw-pbr`), two `shot --views=all --map` runs of one release
  build hand over different scene layouts from `island-crossing` on: the parts digest differs in
  positions, normals, texcoords, indices, meshes, instances, deformers and poses. The trace and the
  pictures are the same on every frame. The vanilla profile's runs agree on everything. It also
  happens with the auto-use map switches off.

- `components/rtx/CMakeLists.txt` does not list `guirenderer.hpp`, `result.hpp`, `texturewrap.hpp` and
  `upscale.hpp`, where it lists every other header of the directory.

- The first vanilla `shot --views=all --map` run after a change to the ray-tracing shaders, which
  compiles the launches, primes the driver's cache and starts again, has a different trace at one
  or two views on all eight frames, with the same scene digest. It happened twice in two such
  runs: at `ald-ruhn` (g-direct, g-motion, g-depth, composite) and `andrano-tomb` (g-depth,
  g-puffs), and at `balmora-fog-night` (g-motion, g-depth). The next run of the same build agrees
  with the baseline on every frame and picture. A run with an empty driver cache directory, whose
  own pipeline cache was warm, also primed and started again, and agreed.
