# Open issues

- With the PBR profile (`~/.config/openmw-pbr`), two `shot --views=all --map` runs of one release
  build hand over different scene layouts from `island-crossing` on: the parts digest differs in
  positions, normals, texcoords, indices, meshes, instances, deformers and poses. The trace and the
  pictures are the same on every frame. The vanilla profile's runs agree on everything. It also
  happens with the auto-use map switches off.

- `components/rtx/CMakeLists.txt` does not list `guirenderer.hpp`, `result.hpp`, `texturewrap.hpp` and
  `upscale.hpp`, where it lists every other header of the directory.

- Vanilla `shot --views=all --map` frame hashes move between builds whose shaders compute the same
  arithmetic, and sometimes between runs of one build. The views are always among `addamasartus`,
  `ald-ruhn`, `andrano-tomb`, `arkngthand`, `balmora` and `seyda-neen-ship-dawn`, and the channels
  mostly g-depth and g-motion, with the same scene digest. With `--upscale=off` the saved pictures of
  those views stay the same byte for byte, and one frame hash of `balmora`'s picture moved on two
  of eight frames. A build that adds code the vanilla frame never runs moves some of them in every
  run; the first run after a shader change, which primes the driver's cache and starts again, has
  moved up to five of them where the next run moved none. `rtx debug gate`'s repeat pair twice found
  `one-cell-walk` not repeatable: on frames 1 to 6 of 360 (g-direct, g-motion, g-depth), and on all
  360 (g-motion and g-depth on every frame, g-direct and composite on 358, the picture on 51). The
  second time, a later run agreed with the pair's second run on every frame. Ten pairs run straight
  after each found it repeatable.

- After a prime stands at its cap ("the driver's compile did not finish, so its cache cannot be
  primed"), the next run of the same build does not prime, because its launches come out of the
  pipeline cache, and draws some views differently. A vanilla `shot --views=all --map
  --upscale=off` after one moved `dagoth-ur-caldera` (picture and composite on all eight frames) and
  `island-crossing` (composite on eight, picture on one). The run after that agreed with the
  baseline on every picture.
