# Review: the whole diff against upstream/master

Scope: every production file the fork adds or edits against `upstream/master`
(merge base `97c3f81aba`) — `components/rtx*`, `components/surface`,
`components/myguirtx`, `apps/rtxtool`, `apps/openmw/mwrender/rtx`, the lifted
`components/{sky,weather,terrain,bodyparts}`, and the upstream files the seam
touches. Test files are out of scope.

**Delete an item when you have addressed it.** This file lists open findings only.

---

## Nothing can ask whether the renderer carries state between frames

`Check::PictureSettles` asked it and was deleted, because its premise is false here:
it rested on "two frames of one scene from one eye differ only if something carried
state it should not have", and the trace walks its bounce sequence by
`VisibilityConstants::mFrame`, which `Session::getSampleFrame` steps every frame.
Measured at Addamasartus, it failed under `--upscale=off --filter=false`, where
neither denoiser runs and nothing but the sampler is left to differ. So the question
it meant to ask is still unanswered, and no other instrument in the fork can see a
shimmer.

- [ ] Give a stop a switch that holds `mFrame` across the frames it compares, then ask
  the question again against `Denoiser::None`. `Schedule::mFrozen` cannot be that
  switch — `shot --accumulate` freezes the world precisely so it can average frames
  the sampler is still stepping — so it wants a field of its own on `Schedule`, read
  by `Session::getSampleFrame`. Under either denoiser the answer stays "they differ"
  by design, so the check only ever runs with both off, which is why it is worth
  asking whether it earns its keep before it is built again.

## Comment blocks lost their paragraph breaks, so a paragraph documents the wrong line

Two kinds, one cause: a `///` block that should be two paragraphs, or two
declarations' documentation, got joined.

A doc comment now sits on a declaration it is not about:

- [ ] `components/rtx/renderer.hpp:435-436` — "What one traced frame came to."
  is `FrameResult`'s summary and sits on `struct GpuSpan`. `FrameResult`
  (line 450) has none.
- [ ] `components/rtx/scenedesc.hpp:438-441` — "One mesh placed in the world: a
  row of the top-level acceleration structure." is `MeshInstance`'s summary and
  sits on `struct Sprite`.
- [ ] `components/rtx/frameworld.hpp:26-51` — the six-paragraph summary of
  `struct FrameWorld` (including the three-bugs list) sits on `rainOnWater`.
  `struct FrameWorld` at line 98 has no comment and a stray blank line after
  its brace.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp:51-68` — the summary of
  `RtxRenderer` sits on the forward declaration `class TracedView;`.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp:331-337` — `mMirror`'s comment
  is two comments spliced; the first sentence ends mid-clause at "a subtree the
  world and a doll can both reach must".
- [ ] `components/rtx/scenedesc.hpp:1041-1046` — a stale `//` block describing
  "Two flat arrays rather than one struct" sits above `PlacementTable
  mPlacements`, which is one struct, immediately above the `///` comment that
  says so.
- [ ] `components/rtx/sceneextractor.cpp:80-81` — `/// The texture bound at
  `unit`, or null.` is the last line of an anonymous namespace and documents
  nothing.
- [ ] `apps/openmw/mwrender/rtx/worldmirror.cpp:109-112` — a copy of the "Where
  the benchmark's `walk ms` starts" comment from
  `rtxrenderer.cpp:599-601`, attached to no statement. The timing really starts
  in `rtxrenderer.cpp:602`.
- [ ] `components/rtx/lightbuilder.hpp:360-362` — a bare `///` line between the
  paragraph and the `@param` block of `requireWeather`.

A paragraph break inside one block was dropped, so the second paragraph reads
as a continuation of the first (`components/rtx/lightbuilder.hpp` style is a
blank `///` between paragraphs):

- [ ] `components/rtx/sceneextractor.cpp:581`
- [ ] `components/rtx/texturebuilder.cpp:281`
- [ ] `components/rtx/renderer.hpp:454`
- [ ] `components/rtxvulkan/presenter.cpp:131`
- [ ] `apps/openmw/mwrender/rtx/readworld.cpp:25` and `:34`
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.cpp:717` and `:736`

## `chooseView` returns a `View` whose optionals are never empty

`View::mHour` and `View::mWeather` are `std::optional` because a view file entry
may fix neither. `chooseView` resolves both through `hourFor`/`weatherFor` and so
always fills them, and every caller then dereferences without checking. The type
says something untrue of what that function returns, and `stageOnePlace` now
states the reason once rather than six times — which is a comment standing in for
a type.

- [ ] Give `chooseView` a return type whose two conditions are a `float` and a
  `std::string`, so nothing has to know the optionals are full. It is `View`
  minus the optionality plus `mRoute`, which is close enough to `View` that the
  question is whether `View` should carry a resolved sibling or the file's own
  entry should be a separate type from a chosen place.

## One rule for "the command line beats a view", implemented twice

`hourFor`/`weatherFor` say the command line wins. `FrameRequest::describeStaging`
says `view.mHour.value_or(mHour)`, which reads as the opposite. The two compose
to the documented answer in every case, because `chooseView` resolves before the
frame is built and `applyConditions` pre-mutates the views a run stages — so this
is not a bug, and the review entry that called it one was wrong. What is true is
that a reader of `describeStaging` cannot tell which rule is in force.

- [ ] Make `describeStaging(view)` apply `hourFor`/`weatherFor` itself, which
  needs `FrameRequest` to carry what the command line *named* rather than what it
  resolved to — two `std::optional`s. `applyConditions` then goes away, and the
  rule has one home.

## Duplicated blocks inside one function

- [ ] `apps/openmw/mwrender/rtx/session.cpp:363-367` and `:376-379` —
  `beginStop`'s free-camera branch and its final `else` branch compute `mFrom`
  and `mFromLook` with identical code. Hoist it above the `if`.
- [ ] `apps/openmw/mwrender/rtx/session.cpp:346` and `:363` — `const
  MWWorld::Ptr player` is declared twice, the second shadowing the first inside
  the free-camera branch. The comment beside the inner one explains where a
  position lives, not why a second `getPlayerPtr()` is needed.
- [ ] `components/rtx/scenedesc.cpp:427-447` — `holdMaterialTextures` and
  `dropMaterialTextures` are the same four-step walk with one call swapped. One
  `forEachMaterialTexture(material, fn)` removes the pair and the risk that a
  role added to one is forgotten in the other.

## A per-frame `dynamic_cast` chain the cache beside it already covers

- [ ] `components/rtx/materialresolver.cpp:105-149` — `animate` is called for
  every node of the graph every frame. For every node that has any callback it
  runs `findUpdater`, which walks both callback chains and `dynamic_cast`s each
  link, *before* looking the node up in `mAnimated`. The entry is already keyed
  on the node and already lives across frames — store the `StateSetUpdater*` in
  it and the chain walk happens once per node instead of once per node per
  frame. Every other cast on this walk is gated on `isFrom(node, ...)` for
  exactly this reason (`sceneextractor.cpp:257`, `:378`, `:662`).

## Two contracts for one keep set, and they disagree

- [ ] `components/rtx/scenedesc.cpp:610-624` — `release`'s early return compares
  `meshes.size()` against the live count, which is only sound if the keep set
  holds no duplicates. `markKept` (`:594-596`) documents the opposite:
  "Duplicates and any order are fine." Today the callers happen to be
  duplicate-free, so the code works and one of the two comments is a trap. Say
  it once — assert the set is unique, or count uniques.

## Two parallel families of build scratch in `SceneAcceleration`

- [ ] `components/rtxvulkan/sceneacceleration.hpp:313-365` — `mBuildGeometries`,
  `mBuildMicromaps`, `mBuilds`, `mBuildRanges`, `mBuildSizes`,
  `mBuildScratchOffsets`, `mLiveBuilds`, `mBuildRangePointers` and the parallel
  `mRefitGeometries`, `mRefitMicromaps`, `mRefitBuilds`, `mRefitRanges`,
  `mRefitRangePointers` are the same shape twice, filled by `buildMeshes` and
  `prepareRefit` with the same sequencing rule ("the geometries are sized before
  any build info names one"). One `StructureBuildBatch` holding the vectors and
  that rule, instantiated twice, removes thirteen members and one of the two
  places the rule can be got wrong.
- [ ] `components/rtxvulkan/sceneacceleration.hpp:76-77` — the deleted copy
  constructor and assignment sit after `build()`, away from the constructor and
  destructor at lines 58-59.

## `SceneDesc::clear()` names forty-four members by hand

- [ ] `components/rtx/scenedesc.cpp:718-764` — a member added to `SceneDesc` and
  forgotten here is a scene that keeps a departed world's table with no
  diagnostic. `mKeptMeshes` and `mKeptMaterials` are already absent from the
  list; they happen to be safe because `markKept` clears them. Group the
  members into sub-objects that clear themselves, the way `PlacementTable` and
  `TextureTable` already do, so the list shrinks to the things that are really
  `SceneDesc`'s own.
- [ ] `components/rtx/scenedesc.cpp:718` — `SceneDesc::clear()` has no production
  caller at all, and `components/rtx/renderer.hpp:554` states as much:
  "`SceneDesc::clear` is never called on it". So `getResetRevision()` never
  moves, `SceneUploader`'s `Kind::Rebuilt` path is reached only by an uploader
  that has never seen the pair in front of it, and `CompositeQueue::gather`'s
  whole reset branch is dead. Either delete the three of them, or say what is
  meant to call `clear` and why nothing does.

## Include blocks out of the order the tree states

`AGENTS.md` fixes the order: own header, standard library, other libraries,
`<components/...>`/`<apps/...>`, then quoted local headers. `.clang-format`
preserves the blocks, so nothing checks this.

- [ ] `components/rtx/texturebuilder.cpp:15-22` — `<components/debug/debuglog.hpp>`
  and `<components/resource/imagemanager.hpp>` come after the quoted locals.
- [ ] `apps/openmw/mwrender/renderingmanager.cpp` — `<components/weather/precipitation.hpp>`
  was added inside the quoted-local block, after `"vismask.hpp"`.

## Helpers with external linkage that every neighbour keeps internal

- [ ] `components/rtx/lightbuilder.cpp:549-583` — `lightPhase`, `band` and
  `flame` are defined at `Rtx` namespace scope and declared in no header, so
  they have external linkage. Every other helper in the file sits in an
  anonymous namespace. Move them into one.
- [ ] `components/rtx/lightbuilder.cpp:31-113`, `:115-208`, `:210-240` — three
  separate anonymous namespaces in one file, with `Rtx`-scope definitions
  between the second and the third. One block, or a comment saying why not.

## One fact derived twice across the two hosts

- [ ] `apps/openmw/mwrender/renderingmanager.cpp:809` computes the rain
  intensity from `mSky->getRainRipplesEnabled()` and
  `mSky->getPrecipitationAlpha()`; `components/rtx/frameworld.cpp:14-17`
  computes the same number from `Weather::Precipitation`. Both read the same
  object by different routes. `Rtx::rainOnWater` is already the shared
  spelling — call it from `RenderingManager::update` too.

## Smaller things

- [ ] `components/rtx/lightbuilder.cpp:448-450` — a blank line between
  `weatherIndex`'s signature and its opening brace.
- [ ] `components/rtxvulkan/vulkanrenderer.hpp:216-219` — `static constexpr
  VkFormat sTargetFormat` is declared between `mInstance` and `mDevice`, inside
  a member list whose opening comment states that declaration order is
  destruction order. Move it above the data members.
- [ ] `components/rtxvulkan/vulkanrenderer.cpp:124-125` — no blank line between
  `deviceExtensionsFor` and `hasSea`'s doc comment.
- [ ] `components/rtx/spanallocator.cpp:37-42` — the best-fit search ranks holes
  by `hole->mCount`, not by what is wasted after the block alignment `place()`
  applied. With `mBlock` set, a smaller hole can waste more than a larger one.
  Rank on `taken.getEnd() - hole->mOffset`, or say why the hole's own size is
  the right key.
- [ ] `components/rtx/sceneextractor.cpp:337-338` — `descend` recognises
  `osg::Sequence` with `std::strcmp(node.className(), "Sequence")` where every
  other gate on this walk uses `isFrom(node, "<library>")`. Both are cheap; one
  spelling would read better and would put the class test beside the library
  test in `nodelibrary.hpp`.
