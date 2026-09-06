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

## One fact derived twice across the two hosts

- [ ] `apps/openmw/mwrender/renderingmanager.cpp:809` computes the rain
  intensity from `mSky->getRainRipplesEnabled()` and
  `mSky->getPrecipitationAlpha()`; `components/rtx/frameworld.cpp:14-17`
  computes the same number from `Weather::Precipitation`. Both read the same
  object by different routes. `Rtx::rainOnWater` is already the shared
  spelling — call it from `RenderingManager::update` too.

## Smaller things

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
