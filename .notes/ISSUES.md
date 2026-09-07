# Open issues

- The renderer draws one identical scene two ways, and only outdoors. Three runs of 90 frames agree
  on every column of the hashes table and differ on 28 pictures of 90 at Seyda Neen and on 78 at the
  island crossing. `arkngthand`, an interior, agrees on every column and every picture. So what
  moves is not in the tables: the sky, the sun, the weather and the water are what an exterior has
  and an interior does not, and none of them is a column. The difference is 1 of 255 on a scattering
  of pixels across the whole frame.

- `Terrain::ObjectPaging::getChunk` keys its cache on `(center, size, activeGrid)` and `createChunk`
  still reads the view point twice: `dSqr` drops references against `minSize` and against
  `minSizeMerged`. So which references a chunk holds is decided by where the eye stood when the
  chunk was first built. On the pairs where it shows, every geometry column of `island-crossing`
  moves together — positions, normals, texcoords, indices and the mesh rows on 279 frames of 360 —
  while each mesh's own vertices agree, which is a relayout of the same meshes and not new ones.
  Holding the culls to the chunk's own viewing distance took 5824 placements to 6175; holding them
  to its centre took them to 6442, the structures from 227 MiB to 284, and the p99 from 51 ms to 62.

- `Terrain::QuadTreeWorld::preload` stops at `abort`, so how many chunks it builds is decided by how
  fast its thread ran. Two runs logged `preload 5 of 118` and `preload 16 of 118` at one view point.
  `MWWorld::Scene::preloadTerrain` queues it on a work queue and `CellPreloader` aborts it from the
  next grid change, and neither is gated by `preload enabled`. It moves no column: letting every
  pass finish gave 191, 309 and 360 frames of 360 on its own, and the same 69 or 115 as holding the
  chunk sort did when the two were held together.
