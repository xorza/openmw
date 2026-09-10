# Open issues

- The scene carries no per-vertex colour at all, and the terrain is where it shows. `MeshReader`
  asks a geometry for its vertices, its normals and texture coordinate zero and never for its colour
  array, so every model `NifOsg` gave one — `nifloader.cpp:1705` and `1907` — is traced as though
  every vertex were white. `GroundReader` is worse than not reading it: it holds an
  `osg::Vec4ubArray`, hands it to `Terrain::Storage::fillVertexBuffers` on every cell, and copies
  only the positions and the normals into `PreparedGround`. The land record's `VCLR` is read off the
  disk and dropped on the floor.
