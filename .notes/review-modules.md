# Module review — the whole diff against `upstream/master`

Delete an item when you have addressed it. This file lists open items only.

Scope: the fork's own production code — `components/rtx*`, `components/surface`,
`components/myguirtx`, `components/myguiplatform`, `components/sky`, `components/weather`,
`apps/openmw/mwrender` and `apps/rtxtool`. Test structure is out of scope.

---

## A free list is filled as a stack and read as a heap

`slotrows.hpp` states the rule: "Every table takes its slots through this pair". One table does
not, and the result is undefined behaviour rather than a style break. `std::pop_heap` has a
precondition that the range is a heap. A range built by `push_back` is not one.

- [ ] `components/rtx/deformertable.cpp:137` gives a rig slot back with
  `mFreeRigs.push_back(range.mDeformer)`. `addRig` takes one with `takeSlot`, which calls
  `takeFreeSlot`, which calls `std::pop_heap`. `mFreeRigs` is never a heap, so the pop is undefined
  and the slot it answers with is not the lowest one free. Use `freeSlot`.
- [ ] `components/rtx/deformertable.cpp:151` does the same for `mFreeMorphs`.
- [ ] The lowest-slot rule exists to make a slot a function of what stands, not of the order the
  dead left in — `slotrows.hpp:14-23` measured what the other order cost. Rig and morph slots are
  outside that rule today, and `.notes/repeatable.txt` is what the rule protects. Add a test that
  asserts a released rig slot is handed out lowest-first.

## The repeatability gate lists struct fields by hand

`digestParts` is what the scene columns of `repeatable.sh` are built from. It enumerates the fields
of four scene types by hand. A field added to any of them compiles, runs, and is silently outside
the gate. `ExtractionStats` already solved exactly this problem: `extractionstats.cpp:16`
destructures the whole struct, so a new field does not compile until it is named.

- [ ] `components/rtxbench/scenedigest.cpp:239` lists every field of `MeshRange`, `MeshInstance`,
  `Material` and `SpriteEmitter` by hand. Bind the struct instead, as `countersOf` does.
- [ ] `components/rtxbench/scenedigest.cpp:51` lists the fields of `Material` a **second** time, and
  a different subset: `addMaterial` omits `mFlatten`, `mAnimated`, `mDiffuseNeverSolid` and
  `mLayers.mOffset`, which `digestParts` includes. One material, two hand-kept lists.
- [ ] `components/rtxbench/framehashes.hpp:27-31` hashes an object representation:
  `add(span<const T>)` goes through `std::as_bytes`. `Light`, `Rig`, `Morph`, `Sprite`,
  `MaterialLayer`, `GpuBone` and `GpuInfluence` reach the digest that way. None of them has padding
  today. One `bool` added to any of them introduces padding, padding bytes are indeterminate, and
  the gate itself then reports a difference the renderer did not make. Add a `static_assert` on
  `sizeof` against the sum of the members, or digest these field by field like the others.

## Change lists that do not use `SlotSet`

`SlotSet` exists because a plain vector made a crowded cell pay N²/2 comparisons, and `SlotChanges`
exists because an arrival and a release have to cancel. Four lists still do it by hand.

- [ ] `components/rtx/deformertable.hpp:129-130` keeps `mArrivedRigs` and `mArrivedMorphs` as plain
  vectors. `deformertable.cpp:138` and `:152` call `std::erase` on them once per released deformer.
  That is a linear scan and an erase from the middle, per rig, on the frame a cell leaves — the
  frame with the least room. Use `SlotChanges`.
- [ ] `DeformerTable` reports arrivals and never reports what it freed. Every other table has a
  `getFreed`. A backend therefore cannot reclaim what a departed rig held; it waits for the next
  arrival to take the slot over.
- [ ] `components/rtx/placementtable.hpp:80-81` keeps `mMoved` and `mSettled` as plain vectors that
  hold duplicates. The header states the cost — "one row written twice" — but the crowded cell is
  exactly the case `SlotSet` was measured on. Either use `SlotSet` or say why the placement table is
  the one that must not.

## "Allocate a run, grow the buffer, copy into it" is written nine times

Every one of these sites repeats the same three-step invariant: take a run from a `RunAllocator`,
grow the parallel buffer to `getEnd()` if it is short, then copy at the run's offset. A site that
forgets the growth writes past the end of a vector.

- [ ] `components/rtx/meshtable.cpp:63` and `:70`
- [ ] `components/rtx/materialtable.cpp:65` (`addMask`) and `:79` (`addLayers`) — these two
  functions are the same function with the element type and the member changed.
- [ ] `components/rtx/deformertable.cpp:30`, `:32` (`addRig`), `:54` (`addMorph`), `:173` and `:184`
  (`stand`)
- [ ] Give `RunAllocator` a companion that owns its buffer — `RunBuffer<T>` with `allocate(span)`
  returning the `Run` — and let `MeshTable`'s vertex allocator drive its three parallel arrays
  through one call. The invariant then exists in one place.

## `MeshTable` and `MaterialTable` are the same table twice

- [ ] `MeshTable::mark` (`meshtable.cpp:157`) and `MaterialTable::mark` (`materialtable.cpp:87`)
  have identical bodies.
- [ ] `MeshTable::getLiveCount` (`meshtable.hpp:53`) and `MaterialTable::getLiveCount`
  (`materialtable.hpp:37`) have identical bodies.
- [ ] Both hold a `std::vector<std::uint8_t> mKept` with the same comment, and both `sweep` walk
  every row and free what `mKept` did not name. Only what is released per row differs. Lift the
  shared half into one type that both hold, and leave each table its own release step.

## The Vulkan backend records a compute pass by hand, once per pass

The infrastructure below the passes is well factored — `Owned`, `SetLayout`, `PipelineLayout`,
`Specialization`, `ComputePipeline`. The layer above them is not: every pass spells out the same
four steps. Two files already have the general form of the first one.

- [ ] `std::uint32_t groupsFor(std::uint32_t)` is defined in eleven files:
  `accumulatepass.cpp:15`, `atrouspass.cpp:14`, `bloompass.cpp:17`, `compositepass.cpp:15`,
  `exposurepass.cpp:24`, `micromappass.cpp:12`, `skinpass.cpp:16`, `spritebinpass.cpp:14`,
  `tonepass.cpp:19`, `visibilitypass.cpp:34`, `wavepass.cpp:52`. `fogvolume.cpp:45` spells the same
  arithmetic as `columnsFor`. `spritebinpass.cpp` and `visibilitypass.cpp` already take the
  workgroup as a parameter, so the general version exists twice and the specialised copy ten times.
- [ ] The descriptor binding list of N identical storage images is built by a loop in
  `accumulatepass.cpp:31` and spelled out in `atrouspass.cpp:22`, `tonepass.cpp:26`,
  `compositepass.cpp` and `bloompass.cpp`. `accumulatepass.cpp:29` comments on the split rather
  than removing it.
- [ ] `std::array<VkWriteDescriptorSet, N> writes{}; for (i) writes[i] = { …STORAGE_IMAGE,
  &images[i] };` appears in twelve files.
- [ ] `vkCmdBindPipeline` / `vkCmdPushDescriptorSet` / `vkCmdPushConstants` / `vkCmdDispatch` is one
  four-line block repeated in nine files.
- [ ] One small type beside `ComputePipeline` — holding the workgroup size, building the binding
  list from a count, taking a span of image views, and taking the push constants by value — removes
  every item above.

## Three camera builders, one set of defaults, three copies of the comment

- [ ] `components/rtx/camera.cpp:93`, `:132` and `:205` each end a `VisibilityConstants` with the
  same three fields — `mWaterLevel` at negative infinity, `mSeaHeading` at `(1, 0)`, `mFogLift` at
  one — and two of the three comments are copied word for word. `makeCameraFromView` and
  `makeCameraAlong` also repeat the `halfHeight` / `halfWidth` / `spread` arithmetic. Put the
  defaults in one local helper the three builders start from.

## `MeshResolver::resolve` asks the same question five times

- [ ] `components/rtx/meshresolver.cpp:180` is 174 lines and branches on `read.mDeform` at five
  separate points: the reuse test, the deformer lookup, the pose on reuse, the deformer resolve, and
  the pose on arrival. Each arm names a rig or a morph and does the same thing to it. Split the
  deformer into a small object resolved once — "what poses this drawable, and its index" — and the
  function becomes one path.
- [ ] `DeformerTable::stand` (`deformertable.cpp:161`) and `DeformerTable::release` (`:116`) each
  carry a Rig arm and a Morph arm that differ only in which table, which allocator and which buffer
  they name. The same split would answer both.

## Three resolve functions with one shape

- [ ] `MaterialResolver::resolve`, `resolveTerrain` and `resolveWater`
  (`materialresolver.cpp:278`, `:157`, `:247`) each repeat: find the key; if found, stamp it, count
  a reuse and return; otherwise add, record and count an arrival. One `reach(key)` helper would
  leave each function only what it actually does differently.

## Casts that the file next to them argues against

- [ ] `components/rtx/meshresolver.cpp:58` defines `asVec3Array` with an essay saying that
  `osg::Array` states its type in a byte and that `dynamic_cast` walks the class hierarchy to
  discover the same thing. `meshresolver.cpp:308` then reads the texture coordinates with
  `dynamic_cast<const osg::Vec2Array*>`. Add `asVec2Array` and use it.
- [ ] `components/rtx/shading.cpp:27` uses `dynamic_cast` on the result of
  `getAttribute(osg::StateAttribute::BLENDFUNC)`, which is keyed by type. `addsLight` runs once per
  drawable's chain. `static_cast` is exact there.
- [ ] `sceneextractor.cpp` spells `from == Library::X ? dynamic_cast<T*>(&node) : nullptr` at five
  places: lines 324, 327, 404, 739 and 759. `stepParticles` gates a further pair (`:447`, `:452`)
  at the head of the function instead. One `castIf<T>(Library, node)` names the idiom once, and
  cannot be written with the wrong library.

## An invariant kept by two loops agreeing, with nothing that checks it

- [ ] `components/rtx/texturebuilder.cpp:249` reserves `mLevels` from a count taken in one loop, and
  a second loop then fills it through three different branches. Every `TextureData` already built
  spans `mLevels`, so a reallocation would leave earlier descriptions pointing at freed memory. The
  two loops agree today. Nothing says they must. Add `assert(mLevels.size() <= levels)` after the
  fill, or reserve from the branch structure itself.

## Small duplication and stale narration

- [ ] `double megabytes(std::uint64_t)` is defined identically in
  `components/rtx/memoryreport.cpp:9` and `components/rtxbench/benchrecord.cpp:14`.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp:350` documents `mSpentMs` as "a running average of
  what the trace costs". `rtxrenderer.cpp:801` accumulates `result->mWaitMs`, and the log line at
  `:810` correctly says "waited … for the device". The header comment describes a different number.
- [ ] `components/myguirtx/rendermanager.hpp:122` carries a doc comment — "The clock `update` reads
  its frame delta from, and what it last read" — with no member under it. The class closes on the
  next line. The clock left when `update` started taking the step from its caller.
- [ ] `RtxRenderer::renderGui` (`rtxrenderer.cpp:502`) also presents the frame, and it is the
  ordinary present path for a frame that has a world in it. `Renderer::renderGui`'s own
  documentation says "the GUI, with no world behind it". Either rename it or split the present out.
