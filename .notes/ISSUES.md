# Open issues

- One frame traced twice is not the same frame. `verify` run twice against one build moved 9 of 22
  standing views, each by 1 of 255 on up to 0.11% of the pixels — one process, one scene, one
  camera, no upscaler and no denoiser. A difference of a least significant bit over a scattering of
  pixels is what a sum taken in another order looks like, and a floating-point sum is not
  associative however it is taken. Through the accumulated history it reaches every frame after it:
  `one-cell-walk` agreed on every column of the hashes table and differed on 175 to 342 pictures of
  360. It is worse when the card is busy — the same binary and route repeated exactly on a card at
  55 °C and 2325 MHz, and differs on most pairs at 76 °C and 1770 to 1905 MHz.

- `Terrain::ObjectPaging::getChunk` keys its cache on `(center, size, activeGrid)` and `createChunk`
  still reads the view point twice: `dSqr` drops references against `minSize` and against
  `minSizeMerged`. So which references a chunk holds is decided by where the eye stood when the
  chunk was first built. On the pairs where it shows, every geometry column of `island-crossing`
  moves together — positions, normals, texcoords, indices and the mesh rows on 279 frames of 360 —
  while each mesh's own vertices agree, which is a relayout of the same meshes and not new ones.
  Holding the culls to the chunk's own viewing distance took 5824 placements to 6175; holding them
  to its centre took them to 6442, the structures from 227 MiB to 284, and the p99 from 51 ms to 62.

- The material and texture tables take different slots run to run. It is the one thing that moves on
  every pair of `island-crossing`: the `materials` and `textures` columns differ on 64 to 115 frames
  of 360 while the geometry, the mesh rows and the placements agree. `Rtx::TextureTable::takeSlot`
  and `Rtx::takeSlot` hand out the last slot freed, so a free list built in another order is another
  layout — and what fills those two tables that does not fill the others is the bake, which arrives
  from `Rtx::CompositeQueue` on a budget of composites a frame.

- `Terrain::QuadTreeWorld::preload` stops at `abort`, so how many chunks it builds is decided by how
  fast its thread ran. Two runs logged `preload 5 of 118` and `preload 16 of 118` at one view point.
  `MWWorld::Scene::preloadTerrain` queues it on a work queue and `CellPreloader` aborts it from the
  next grid change, and neither is gated by `preload enabled`. It moves no column: letting every
  pass finish gave 191, 309 and 360 frames of 360 on its own, and the same 69 or 115 as holding the
  chunk sort did when the two were held together.
