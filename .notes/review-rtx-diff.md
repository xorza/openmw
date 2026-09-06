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
