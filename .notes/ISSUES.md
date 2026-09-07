# Open issues

- `Terrain::ObjectPaging::getChunk` keys its cache on `(center, size, activeGrid)` and leaves out
  the view point, so a chunk's contents are decided by which frame first asked for it.
  `SceneUtil::Optimizer`'s `MergeGeometryVisitor` sorts alpha-blended geometry by distance from
  `relativeViewPoint` before it concatenates, so the merged vertex and index buffers a chunk arrives
  with are a function of where the eye stood at the build. `createChunk` reads the view point twice
  more: `dSqr` drops references against `minSize` and against `minSizeMerged`. Measured on
  `island-crossing`, the layout digest differed on 257, 281 and 345 frames of 360 as it stands, and
  on 69 or 115 with the view point held to the chunk's centre. A digest of each mesh's own vertices,
  which no slot order can tell, is identical over two runs with the view point held and differs
  without it. Holding the culls as well takes 5824 placements to 6175, the structures from 227 MiB
  to 284, and the p99 from 51 ms to 62.

- `island-crossing`'s layout digest still differs on 69 or on 115 frames of 360 with the view point
  held to the chunk's centre — two states and nothing between them over eight runs. What the walk is
  handed is the same either way: `Rtx::digestScene`, which sums per placement and reads a shape as
  the multiset of its triangles, is identical on all 360 frames over two runs. So the same geometry,
  the same materials and the same transforms reach the tables in a different order. The divergence
  opens inside the first second of the flight: three one-frame runs of four repeat exactly.

- The picture is not certainly repeatable either. One pair of about thirty-five differed on 53
  frames of 360 with the upscaler and the denoiser off, and nothing since has reproduced it.

- `Terrain::QuadTreeWorld::preload` stops at `abort`, so how many chunks it builds is decided by how
  fast its thread ran. Two runs logged `preload 5 of 118` and `preload 16 of 118` at one view point.
  `MWWorld::Scene::preloadTerrain` queues it on a work queue and `CellPreloader` aborts it from the
  next grid change, and neither is gated by `preload enabled`. It moves no digest: letting every
  pass finish gave 191, 309 and 360 frames of 360 on its own, and the same 69 or 115 as the view
  point alone when the two were held together.
