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
  frame. **The comparator is fixed and the sort still runs.** It ties five members
  where it copied nine floats a side, and `everyFieldOfALightTakesItsTurnInTheOrder`
  now pins the order every level of it deep — nothing did before, and the order is
  what a repeated run rests on.

  **Merging the residency's own contribution is closed.** `SceneExtractor::walk`
  does put the residency after the graph, so one walk is a stable prefix and an
  uncertain suffix — but `orderLights` is at the uploader, which is "the one point
  every path passes", and the game walks its precipitation beside its world. So the
  list is several prefix-and-suffix pairs and a merge would have to carry each
  boundary out of the extractor. What makes the suffix uncertain is the order
  `Terrain::View` hands its chunks over in, which is the lifted terrain code the
  rasterizer walks too — so fixing it at source is out of this fork's places.
- [ ] `apps/components_tests/rtx/` — the allocation guard covers the frame path and
  stops there. `extractor/materials.cpp:630`, `extractor/skinning.cpp:111` and
  `lightbuilder.cpp:437` all assert zero allocations for a scene already walked.

  **`CompositeQueue::bake` needs none.** What the queue's scratch exists for is
  `TerrainComposite`'s own working set, and
  `RtxTerrainCompositeTest.aScratchTheCallerKeepsLeavesABakeNothingButItsAnswerToAllocate`
  already asserts that a warm bake reaches the heap exactly twice and names which two.
  What the queue adds over that is a request copy on the baker's thread, which is off
  the frame path by design — the class opens by saying a bake happens on no frame at
  all.

  **`SceneTextures::describe` still wants one, and a unit test cannot reach it.**
  Every route through `describe` in a test meets a file that is not there:
  `ImageManager::getImage` answers a miss with its warning image, which is `GL_RGB`
  and so a format this renderer refuses, and the refusal is logged by name — a
  `std::string` per slot per call. So a guard would read the log's allocations rather
  than the scratch's. Reaching the real path needs a decodable file in the VFS, which
  means depending on an OSG image plugin being loadable in the test binary; a test
  that skips when it is not there is the silent pass this suite refuses. What would
  settle it is a description built from bytes the test owns, the way `TestTexture`
  already builds one — a seam `describe` does not have.
