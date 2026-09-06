# Review: the whole diff against upstream/master

Scope: every production file the fork adds or edits against `upstream/master`
(merge base `97c3f81aba`, merged at `429661af86`) — `components/rtx*`,
`components/surface`, `components/myguirtx`, `apps/rtxtool`,
`apps/openmw/mwrender/rtx`, and the lifted `components/{sky,weather,terrain}`.
Test files are out of scope.

The reviewer looked for two things the author asked about. First, per-frame work
that a load could do instead. Second, allocation on the load path, and where a
persistent loader could keep the memory it already has.

**Delete an item when you have addressed it.** This file lists open findings only.

---

## Every Vulkan buffer and image is its own device allocation

`Buffer` and `Image` each construct a `DeviceMemory`, and `DeviceMemory` calls
`vkAllocateMemory`. There is no suballocator. A cell arrival therefore spends one
device allocation per staging buffer, one per texture image and one per shading map.

- [ ] `components/rtxvulkan/texture.cpp:150` — `upload` makes a staging buffer per
  call. `Texture`'s constructor calls it twice, once at line 190 for the levels and
  once at line 211 for the shading map. A cell of two hundred textures spends four
  hundred device allocations on the arrival frame, and buries all of them in the
  batch until the submit finishes. One growable staging ring, reused across the
  uploads of one batch, removes every one of them.
- [ ] `components/rtxvulkan/memory.cpp:45` — one `vkAllocateMemory` per object, and
  two objects per texture. `maxMemoryAllocationCount` is 4294967295 on this driver,
  so the count is not the risk. The cost is the call itself and the padding: each
  allocation is a kernel-visible operation and each is rounded up to the driver's
  granularity, which a shading map of 2 KB pays in full. A suballocator behind
  `DeviceMemory` serves both this and the staging item above.

## Facts about a node are derived again every frame

Each of these is a property of the content. Each is asked once per node per frame.
The map that would hold the answer is already there in two of the three cases.

- [ ] `components/rtx/lightbuilder.cpp:666` — `lightColour` calls `decodeColour`
  twice per light per frame. `decodeColour` calls `toLinear` per channel, and
  `toLinear` is a `std::pow`. That is six `pow` calls per light per frame. The
  colour comes from `LightController::getDiffuse`, which returns the record's own
  colour and never the animated one, so it is constant for the life of the light.
  Only `brightness` and `fade` change. Two routes are open. Cache the decoded colour
  where the light is first met. Or give `toLinear` a 256-entry table, which is exact
  here because every input is a byte divided by 255.
- [ ] `components/rtx/mipchain.cpp:121` and `:134` — the same `pow` per channel per
  texel per level. The inputs at line 116 are `mTexels[...] / 255.0f`, so they are
  byte-derived and a table is exact. A 512-square chain takes about two million
  `pow` calls as it stands.
- [ ] `components/rtx/materialresolver.cpp:123` — `animate` calls `findUpdater` for
  every node that carries any callback, on every frame. `findUpdater` walks two
  callback chains and does a `dynamic_cast` per link. The `mAnimated` entry beside it
  already caches the state set the updater writes into. Cache the updater in the same
  entry, and record the negative answer as well, so that a node with callbacks and no
  updater is asked once.
- [ ] `components/rtx/nodelibrary.hpp:18` — `isFrom` is a virtual call and a
  `strcmp`. `MirrorTraversal` asks it up to four times per node per frame — at
  `sceneextractor.cpp:254`, `:257`, `:374` twice — and twice more per drawable at
  `:659`. `libraryName()` returns a string literal whose address is stable for the
  class, so a small set of literal addresses that already answered yes or no turns
  every call after the first per class into a pointer compare.

## Smaller items

- [ ] `components/rtx/scenedesc.cpp:553` — `orderLights` sorts every light every
  frame. The comparator builds two nine-element tuples per comparison. The sort exists
  to make a run repeat itself, and only the residency walks make the walk order
  uncertain. Sorting the residency's own contribution and merging it would leave the
  graph walk's order alone.
- [ ] `components/rtx/distantlights.cpp:49` — `build` collects into a
  `std::map<ESM::RefNum, Terrain::PagedCellRef>`. **Smaller than it reads.**
  `collectPagedRefs` applies `wanted` before it inserts, and `collectLights` passes
  `litType`, so only `REC_LIGH` reaches the map — a handful a cell, and `mCells`
  keeps the answer for the life of the scene. That is a few hundred nodes once per
  world. `Terrain::ObjectStorage` and `collectPagedRefs` are this fork's own files
  rather than upstream's, so the container could be changed — but the same function
  is what the lifted `objectpaging.cpp` collects every chunk through, and that is the
  rasterizer's path. Not worth the reach for the count.
- [ ] `components/rtx/texturebuilder.cpp:147` — `describeAll` fills `mEverything`
  with a hand-written loop. `std::iota` says the same thing.
- [ ] `apps/components_tests/rtx/` — the allocation guard covers the frame path and
  stops there. `extractor/materials.cpp:630`, `extractor/skinning.cpp:111` and
  `lightbuilder.cpp:437` all assert zero allocations for a scene already walked. No
  test measures `SceneTextures::describe` or `CompositeQueue::bake`. Those are the
  paths this review found allocating, and a count there is what would keep the
  persistent loaders persistent.
