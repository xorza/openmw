# Open issues

- Paged content and cell objects stand on different frames in two runs of one build. `bench
  --views=island-crossing --seconds=6 --warmup=3 --exposure=1 --hashes` differs on 15 to 19 frames
  of 360, and every differing frame also differs in content. Dumped and sorted at a differing frame,
  what one run holds and the other does not is a mixture of merged chunk drawables at chunk origins
  and individual paged statics at their own positions — 105 rows one way and 71 the other, on a
  frame whose placement counts are 8203 and 8237. Turning off `preload enabled`, and this fork's
  own terrain warming thread, changes neither the count nor the picture.

- `Terrain::ObjectPaging::getChunk` keys its cache on `(center, size, activeGrid)` and leaves out
  the view point, which `createChunk` then reads twice: `relativeViewPoint` sorts the alpha-blended
  merge, and `minSizeMerged` drops references by their distance. A chunk's geometry and its contents
  are a function of where the camera stood when it was first built. Holding both reads to the
  chunk's centre takes the content difference over `island-crossing` from 358 frames of 360 to 39,
  and costs 5824 placements against 6442, 227 MiB of structures against 284, and a median frame of
  9.6 ms against 10.3 with the p99 at 51 against 62 — for a picture that then differs on 22 to 25
  frames rather than 15 to 19. Putting the view point in the key, quantised to the chunk's own
  width, leaves the placement count alone and the picture at 15, and takes the p99.9 from 90 ms to
  180 and the worst frame from 169 ms to 245.

- The slot order of the placement, material and mesh tables is not repeatable, and `bench --hashes`
  reports it as a scene difference on almost every frame of a crossing. It moves no picture on its
  own: over three run pairs of `island-crossing`, every frame whose picture differed also differed
  in content, taken as an order-insensitive digest of each placement's transform, index buffer,
  vertex count and material.

- `MWWorld::Scene::mActiveCells` is a `std::set<CellStore*>`, so cells are unloaded in heap-address
  order. Two runs log the same three cells in two orders and drop 93 materials against 94.

- Nothing that crosses a cell is repeatable through Ray Reconstruction, because the network's
  history is recurrent and a frame the trace drew differently reaches every frame after it.
  `one-cell-walk` agrees on all 360 frames at every warm-up tried. `island-crossing` agrees on 1 of
  360 before the merge order was settled, and on 79 after it.
