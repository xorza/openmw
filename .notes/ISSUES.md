# Open issues

- With the PBR profile (`~/.config/openmw-pbr`), two `shot --views=all --map` runs of one release
  build hand over different scene layouts from `island-crossing` on: the parts digest differs in
  positions, normals, texcoords, indices, meshes, instances, deformers and poses. The trace and the
  pictures are the same on every frame. The vanilla profile's runs agree on everything. It also
  happens with the auto-use map switches off.
