# Open issues

- The scene carries no per-vertex colour at all, and the terrain is where it shows. `MeshReader`
  asks a geometry for its vertices, its normals and texture coordinate zero and never for its colour
  array, so every model `NifOsg` gave one — `nifloader.cpp:1705` and `1907` — is traced as though
  every vertex were white. `GroundReader` is worse than not reading it: it holds an
  `osg::Vec4ubArray`, hands it to `Terrain::Storage::fillVertexBuffers` on every cell, and copies
  only the positions and the normals into `PreparedGround`. The land record's `VCLR` is read off the
  disk and dropped on the floor.

- A settled frame that queues a composite waits for it. `CompositeQueue::waitFor` waits until the
  two `collect` takes are ready, and a walk that adopts one cell a frame hands over one stack a
  frame — so a frame that queues one reaches the second condition and waits for the bake it just
  asked for. Measured on `island-crossing` at 1080p over three legs: `bake ms` reads 0.00 ms median
  against 28.0 to 28.7 ms at the p99 and 31.0 to 35.3 ms worst, and the frame p99 is 46.4 to 48.4 ms
  where the same run under `--settled=false` reads 0.04 ms at the `bake` p99.
