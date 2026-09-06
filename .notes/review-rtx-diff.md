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

## Facts about a node are derived again every frame

Asked once per node per frame, and not a fact about the content at all — what it is
instead is below.

- [ ] `components/rtx/materialresolver.cpp:123` — `animate` calls `findUpdater` for
  every node that carries any callback, on every frame. `findUpdater` walks two
  callback chains and does a `dynamic_cast` per link. **Caching the answer is not
  open, either sign of it.** `SceneUtil::GlowUpdater` is a `StateSetUpdater` and
  `Animation::addSpellCastGlow` hangs one on a live node and takes it off again when
  its duration runs out (`animation.cpp:1679` and `:1687`) — so a cached negative
  loses an enchanted weapon's glow, and a cached pointer outlives the callback the
  node let go of. Whether a node animates is not a property of the content. What is
  left is making the question itself cheaper than a `dynamic_cast` a link.
  **Built, measured and taken out again.** 1352 calls and 1780 casts a frame at
  `seyda-neen-ship`, of which 25 find an updater; `__dynamic_cast` is 1.32% of the
  process's on-CPU time and 1.21% of that is under `SceneExtractor::walk`. The only
  sound key is the callback object itself — a class's `className()` does not identify
  it, because `StateSetUpdater` declares no `META_Object` and its subclasses report
  their base's name — so the memo is an `Identity<osg::Callback, ...>` swept beside
  `mAnimated`, which is a probe of a thirteen-hundred-entry table and an epoch write
  per callback per frame. That measured as a wash: a cold hash probe costs what the
  failing cast costs, and the cast's hierarchy walk stays warm because it is the same
  few classes every frame.

  **What is left is a cheaper key, and nobody has one.** Anything keyed on the class
  is unsound and anything keyed on the object is a table this size. Worth revisiting
  only with a way to ask an `osg::Callback` its type for less than a hash probe.

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
