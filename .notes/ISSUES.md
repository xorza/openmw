# Open issues

- `bench --views=island-crossing --seconds=6 --warmup=3 --exposure=1 --hashes` draws all 360 frames
  the same over two processes and reports a different scene on 252 to 270 of them. An identical picture from
  a differing digest says the two runs hold the same geometry, the same materials and the same
  transforms in a different slot order. The two entries below are what moves a chunk between the
  runs, and a chunk that arrives at a different frame takes a different slot.

- `Terrain::QuadTreeWorld::preload` stops at `abort`, so how many chunks it builds is decided by how
  fast its thread ran. Two runs logged `preload 5 of 118` and `preload 16 of 118` at one view point.
  `MWWorld::Scene::preloadTerrain` queues it on a work queue and `CellPreloader` aborts it from the
  next grid change, and neither is gated by `preload enabled`. What it builds lands in
  `ObjectPaging`'s cache, which later frames read.

- `Terrain::ObjectPaging::getChunk` keys its cache on `(center, size, activeGrid)` and leaves out
  the view point, which `createChunk` reads three times: `relativeViewPoint` sorts the alpha-blended
  merge, and `dSqr` drops references twice, once against `minSize` and once against `minSizeMerged`.
  Holding the sort to the chunk's centre makes the merged buffers match. Holding the culls to the
  chunk's own viewing distance takes 5824 placements to 6175; holding both to the centre takes them
  to 6442, 227 MiB of structures to 284, and the p99 from 51 ms to 62.
