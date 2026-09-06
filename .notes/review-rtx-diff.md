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
device allocation per texture image and one per shading map.

- [ ] `components/rtxvulkan/memory.cpp:45` — one `vkAllocateMemory` per object, and
  two objects per texture. `maxMemoryAllocationCount` is 4294967295 on this driver,
  so the count is not the risk. The cost is the call itself and the padding: each
  allocation is a kernel-visible operation and each is rounded up to the driver's
  granularity, which a shading map of 2 KB pays in full. A suballocator behind
  `DeviceMemory` is what removes it.

## Facts about a node are derived again every frame

Each is asked once per node per frame. Two of the three turned out not to be facts
about the content at all, and each says below what it is instead.

- [ ] `components/rtx/lightbuilder.cpp:666` — `lightColour` calls `decodeColour`
  twice per light per frame. `decodeColour` calls `toLinear` per channel, and
  `toLinear` is a `std::pow`. That is six `pow` calls per light per frame. The
  colour comes from `LightController::getDiffuse`, which returns the record's own
  colour and never the animated one, so it is constant for the life of the light.
  Only `brightness` and `fade` change. **The table route is closed.** `toLinear` has
  a byte overload now, and a light's colour is `colourFromRGB`'s `byte / 255` — but a
  `mNegative` light's is `-byte / 255` (`lightutil.cpp:128`), which is not one of the
  256 and takes the curve's other leg. Recovering the byte by rounding would be a
  guess. What is left is caching the decoded colour where the light is first met.
- [ ] `components/rtx/materialresolver.cpp:123` — `animate` calls `findUpdater` for
  every node that carries any callback, on every frame. `findUpdater` walks two
  callback chains and does a `dynamic_cast` per link. **Caching the answer is not
  open, either sign of it.** `SceneUtil::GlowUpdater` is a `StateSetUpdater` and
  `Animation::addSpellCastGlow` hangs one on a live node and takes it off again when
  its duration runs out (`animation.cpp:1679` and `:1687`) — so a cached negative
  loses an enchanted weapon's glow, and a cached pointer outlives the callback the
  node let go of. Whether a node animates is not a property of the content. What is
  left is making the question itself cheaper than a `dynamic_cast` a link.
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
