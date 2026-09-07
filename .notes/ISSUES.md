# Open issues

- `bench --views=island-crossing --seconds=6 --warmup=3 --exposure=1 --hashes` falls into one of
  two states, as `one-cell-walk` does. Two runs of three differed on 5 frames of 360 and the third
  differed from both on 44 and 48, with the scene differing on 357 either way. The route flies six
  thousand units up over paged terrain, so what churns is `Terrain::ObjectPaging`. Holding the
  merge's alpha sort still and letting the terrain preload finish rather than aborting it leaves the
  count where it was.

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

- `bench --views=one-cell-walk --exposure=1 --hashes` draws the same 360 pictures over eight runs
  and is handed the same scene in six of them. The other two agree with each other and differ from
  the six from frame 102 on, so one decision early in the walk goes one of two ways and everything
  after it follows.

- Nothing that crosses a cell is repeatable through Ray Reconstruction, because the network's
  history is recurrent and a frame the trace drew differently reaches every frame after it.
  `one-cell-walk` agrees on all 360 frames at every warm-up tried.
