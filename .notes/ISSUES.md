# Open issues

- A mesh gets an opacity micromap or does not according to which textures arrived with the upload
  that carried it, rather than according to the material it wears. `SceneAcceleration::buildMicromaps`
  returns early on an empty arrival and skips any mesh whose diffuse is not among the ones that
  arrived, because the arrival is the only thing carrying host-side mask data. So a mesh appearing
  beside an image the renderer already holds is left asking `RTX_RESOLVE` until the next reset. The
  comment at that line states the behaviour. `SceneUploader::hand` describes
  `scene.getArrivedTextures()` on an extend, and `SceneTextures` already takes an explicit list of
  slots, so the masks an arrived mesh wears could be named there as well.

- One exterior renders differently depending on which cell the process staged before it, and every
  field of the scene it was handed is equal. `verify --views=balmora,seyda-neen-shore` against
  `--views=seyda-neen-shore` reports `differs: worst 8 of 255 on 0.43% of the pixels`. The pixels sit
  on one campfire west of the town, a second faint spot north of it, and a thin speckle over the
  grass between them.

  Only Balmora at -3,-2 and Vivec at 2,-10 do it. Ald-ruhn, Sadrith Mora, Dagon Fel, Vivec's
  canalworks, Addamasartus, the island crossing, Seyda Neen's own views and a second staging of the
  shore itself do not. Balmora and Vivec are the two nearest cells to Seyda Neen at -2,-9 of every
  one tried.

  Compared after staging Balmora at its own camera: the positions, normals, texture coordinates,
  indices, meshes, layers and masks by digest, and the materials, instances and sprite emitters
  element by element. All equal. `--distant-terrain=false` removes the difference.
  `--exposure=1`, `--delight=0`, `--distant-statics=false`, `sCompositeFrom` raised past every chunk,
  and the micromap bake disabled outright do not. `--gpu-validation` reports nothing.

- The instance count a `bench` reports is not reproducible. `--views=seyda-neen-shore --frames=1
  --warmup=0` gives 21,874 once and 21,887 twice, and `--seconds=1` gives 13,345, 13,350, 13,344 and
  13,344. The figure is the acceleration structure's own instance count, which the sweep compacts
  over frames, so a longer run reports a smaller number. `scene --view=seyda-neen-shore` reports its
  placed count of 8,535 on every run.
