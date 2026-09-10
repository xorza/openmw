# Open issues

- A warm-up costs one frame three seconds in `place`, and it is most of what a warm-up costs at
  all. `one-cell-walk` at 1080p flies the same 360 frames either way — `fly` runs from the warm-up
  boundary onward — and `--seconds=6 --warmup=0` takes 10 s against 18 to 20 for any warm-up from
  one second up. The rows say where it goes: the `place` worst is 376 ms with no warm-up and
  3064 ms with two, against a median of 0.4 ms in both, and neither run rebuilds. So a run that
  stands still while the ring fills the reach, then flies, pays a placement spike that a run which
  flies from the first frame never does. Whatever that spike is, every measured run with a warm-up
  is carrying it, and the p99 and the worst frame of every bench are reading it.

- The scene carries no per-vertex colour at all, and the terrain is where it shows. `MeshReader`
  asks a geometry for its vertices, its normals and texture coordinate zero and never for its colour
  array, so every model `NifOsg` gave one — `nifloader.cpp:1705` and `1907` — is traced as though
  every vertex were white. `GroundReader` is worse than not reading it: it holds an
  `osg::Vec4ubArray`, hands it to `Terrain::Storage::fillVertexBuffers` on every cell, and copies
  only the positions and the normals into `PreparedGround`. The land record's `VCLR` is read off the
  disk and dropped on the floor. Carrying it means a fourth attribute the whole way down —
  `PreparedGround`, `SceneDesc::addMesh`, `MeshTable::writeAttributes`, `SceneBuffers` and
  `GpuTables`.

