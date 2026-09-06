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

## The sweep walks the whole world every frame, and only the placements got the guard

`SceneExtractor::retire` runs once a frame from `WorldMirror::settle`. It walks
seven hash maps from end to end. It then writes two flag tables of the same
length.

The placements have a guard against this. `mPlacementsReached` counts the entries
each epoch stamped. `sceneextractor.cpp:581` compares that count with the map size
and skips the sweep where the two agree. A world that stands still reaches every
placement it holds, so the count proves that nothing died.

`MeshResolver::mMeshes` is the same size as `mPlacements` — one entry per drawable,
which is 47,828 on a nine-by-nine exterior. It got no guard. Every other map beside
it got no guard either. The frame budget is 5.3 ms and the walk is 1.8 ms of it, so
a second pass of the same length is not small.

The same count answers all of them. Each resolver can count the entries it stamped
this epoch. Where every count equals its map size, the sweep and
`SceneDesc::release` both have nothing to do, and both can be skipped together.

- [ ] `components/rtx/meshresolver.cpp:496` — `sweep(mMeshes, ...)` visits every
  entry of a map with one entry per drawable. Each visit is a hash node and a cache
  miss. It builds `live` from the survivors, and `SceneDesc::release` is the only
  reader of that list.
- [ ] `components/rtx/meshresolver.cpp:503` and `:504` — `std::erase_if` over
  `mRigs` and `mMorphs`, unguarded, every frame.
- [ ] `components/rtx/materialresolver.cpp:417` — `sweep(mMaterials, ...)`,
  unguarded.
- [ ] `components/rtx/materialresolver.cpp:438` — `std::erase_if` over
  `mTextureOf`, unguarded.
- [ ] `components/rtx/materialresolver.cpp:448` — `std::erase_if` over `mAnimated`,
  unguarded.
- [ ] `components/rtx/emitterresolver.cpp:225` — `std::erase_if` over `mHeld`,
  unguarded.
- [ ] `components/rtx/scenedesc.cpp:611` and `:612` — `markKept` clears a flag
  vector and resizes it to the table length. That is a memset of the whole mesh
  table and the whole material table, every frame. A scattered byte write per
  survivor follows it. The early return at line 622 comes after both passes, so the
  frame that has nothing to free still pays for both.

## The load path allocates buffers a persistent loader could keep

`SceneTextures` states the pattern this project wants. Its comment says it is
"held for the life of its owner, and cleared and refilled per arrival", so that an
arrival frame does not pay for the buffers. Three of its collaborators do not follow
that pattern. Each allocates its working set, uses it once and frees it.

A crossing queues dozens of bakes. The composite bake is the largest of these by a
wide margin.

- [ ] `components/rtx/terraincomposite.cpp:233` — `light` is one `osg::Vec3f` per
  texel of the finest level. At `sCompositeExtent` of 512 that is 3 MB, allocated
  and freed per bake.
- [ ] `components/rtx/terraincomposite.cpp:227` — `grounds` holds one `Ground` per
  layer, and each `Ground` holds two `Decoded` with a `std::vector<osg::Vec3f>`
  inside. A nine-layer chunk allocates up to nineteen buffers per bake.
- [ ] `components/rtx/terraincomposite.cpp:236` — `covered` grows once and serves every
  row, which is what its comment claims. It then dies with the bake, so the next
  chunk grows it again.
- [ ] `components/rtx/terraincomposite.cpp:331` — `coarser` in `buildChain`,
  allocated per bake.
- [ ] `components/rtx/compositequeue.cpp:239` and `:242` — `levels` and `stack` are
  locals of `bake`. The queue beside them already recycles its `Request` objects
  through `mSpare`, and states why. One thread runs `bake`, so the same scratch can
  live on the queue.
- [ ] `components/rtx/texturebuilder.cpp:210` and `:214` — `MipChain built` and
  `AlphaImage alpha` are locals, three lines below `mSourceLevels`, which is a
  member for exactly this reason. Each allocates and frees per sprite source.
- [ ] `components/rtx/texturebuilder.cpp:257` — `mChains.emplace_back` gives every
  chain its own `mTexels` and `mLevels`. `mChains.clear()` at the head of `describe`
  frees all of them. The vector is recycled and its elements are not. A 512-square
  texture decodes to 1.4 MB of loose texels here.
- [ ] `components/rtx/texturebuilder.cpp:217` — `mSpriteLights.emplace_back` has the
  same shape as the chains, and the same fault.
- [ ] `components/rtx/alphaimage.cpp:161` — `reachesSolid` builds a
  `std::vector<MipLevel>` and an `AlphaImage` per call. `MaterialResolver` caches the
  answer, so the call is rare. The buffers are still new every time.
- [ ] `components/rtx/distantlights.cpp:49` — `build` collects into a
  `std::map<ESM::RefNum, Terrain::PagedCellRef>`. That is a heap node per reference,
  over the eighty-one cells of the reach. `Terrain::ObjectStorage::collectLights`
  fixes the container, so this needs an upstream signature or a copy out of the map.

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
- [ ] `components/rtxvulkan/memory.cpp:42` — `findMemoryType` calls
  `vkGetPhysicalDeviceMemoryProperties` on every allocation. The properties never
  change. `PhysicalDevice` already caches its other properties, so read this once
  there.
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
- [ ] `components/rtx/texturebuilder.cpp:147` — `describeAll` fills `mEverything`
  with a hand-written loop. `std::iota` says the same thing.
- [ ] `apps/components_tests/rtx/` — the allocation guard covers the frame path and
  stops there. `extractor/materials.cpp:630`, `extractor/skinning.cpp:111` and
  `lightbuilder.cpp:437` all assert zero allocations for a scene already walked. No
  test measures `SceneTextures::describe`, `CompositeQueue::bake` or
  `TerrainComposite`. Those are the paths this review found allocating, and a count
  there is what would keep the persistent loaders persistent.
