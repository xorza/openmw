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

- `SceneExtractor` names a placement by a hash folded from node **addresses**, so a node the paging
  freed and a node the allocator later hands the same address to share one identity. A new drawable
  then inherits a dead drawable's entry, and its slot with it, instead of taking a fresh one.
  `identityWith` says a collision does not happen at sixty-four bits, which is true of a hash and
  not of an address handed out twice.

  Counted over fourteen runs of `one-cell-walk`, the walk met 1,259,742 drawables by frame 102 in
  every one of them, and adds traded against hits exactly one for one: 5480 added and 1,254,262 hit
  in eleven runs, 5477 and 1,254,265 in one, 5479 and 1,254,263 in another. Every other counter the
  walk keeps is identical, and so is the entry list `QuadTreeWorld::collect` hands over and the
  drawable count under it.

  **The live scene is not affected and neither is the picture.** After the sweep the placement count
  is 4267 in every run, and all fourteen drew the same 360 frames. What differs is the table's shape
  — 5338, 5337 or 5335 slots — because a slot reused is a slot not retired, and slot numbers never
  shrink. That is what moves the scene column of a hashed run.

  A fix needs an identity that survives address reuse. A content key is not available for the reason
  `identityWith` gives — a hundred crates share one geometry, and the node path is what tells them
  apart — so it would have to be a serial the loader stamps on a node.
