# Open issues

- Two `World`s over one installation describe one cell's materials and textures in different orders.
  The placed count, the mesh count, the material count, the texture count, every instance's mesh and
  transform, every light and every sprite agree. Only `MeshInstance::mMaterial` and the order of the
  texture array differ. Thirty-five of Seyda Neen's exterior instances disagree, the same thirty-five
  on every run, with no other cell staged in either world.

- The texture bytes a `bench` reports over a route are not reproducible.
  `--views=seyda-neen-shore --seconds=1` reports 672 textures on every run and either 67.0 or 65.7
  MiB of them. The instance, cutout and structure figures of the same runs agree exactly.
