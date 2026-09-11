# Review: whole diff against `upstream/master` — what remains

Delete an item when you address it. Delete a heading when its items are gone. Everything else the
review found is landed.

## Comments narrate history and restate code

A third of every line in the RTX places is a comment line (`components/rtx` 8194 of 23975,
`rtxvulkan` 5468 of 19646), ~1900 open with a bolded thesis, and about a hundred still tell what the
code used to be or quote a number measured once. The seam and the cited passages are swept; the rest
is not.

- [ ] A file-by-file pass over `components/rtx`, `components/rtxvulkan`, `components/rtxbench`,
  `apps/rtxtool`: keep an invariant, a workaround and its cause, or a trade-off; delete what the
  code used to be, a number measured once, or what the line under it does.
- [ ] `myguirtx/rendermanager.hpp`: a `/*internal:*/` marker comment splits the public section.

## Upstream edits that go past a lift

- [ ] ~130 upstream files are modified or renamed. `mwrender/gl/sky.cpp` (R045), `sky.hpp` (R075),
  `skyutil.hpp` (R074), `screenshotmanager.hpp` (R052), `mwworld/weather.{hpp,cpp}` (-539/+90),
  `mwgui/*` (19 files), `mwlua/*`, `mwinput/*`, `esmloader/*`, `myguiplatform/*` (14 files),
  `sdlutil/*`, `sceneutil/*` (13 files): worth a pass to separate the lift from the edit in each.
  The object paging eye distance and the OpenCS hidden mask are restored; the optimizer's merge
  order and the launcher page are named in `AGENTS.md`.
- [ ] `components/nifosg/nifloader.cpp`: a `Surface::Material&` is threaded through ~15 handler
  signatures beside the `args` bundle that already carries `mMaterial`.
- [ ] `RenderingManager::update` calls `Sky::timescaleClouds()` every frame — a `Fallback::Map`
  string-keyed lookup per frame for both renderers, where upstream read it once.

## Per-frame work

- [ ] `vulkanrenderer.cpp` `traceGuiTexture`: `finishAll` then `submitAndWait` — measured 2.5–12 ms
  per `TracedView::redraw` in `openmw-rtxtool doll`, against a frame of about 5.5 ms; a cell entry
  draws nine map tiles through it. Both waits are load-bearing: the view's `binSprites` rewrites
  the world slot's sprite bins the frame in flight reads, its hits land in
  `mRing.recording().mHitCount`, and `readGuiTexture` orders only against submissions already made.
  The design that removes them: `FrameSource::renderFrame(camera, options, std::span<const GuiTrace>
  views)` records each view into the frame's own command buffer before the world trace, with a
  separate `mViewCounts` buffer, tone into `mViewTarget`, and the copy under the frame's fence
  (`TraceChain::record` already transitions its colour and target from `UNDEFINED`, so several
  views in one buffer order themselves); `TracedView::redraw` outside the frame's window defers,
  inside it appends; `getCopy` reads once the tracing frame is collected; the harness's `writeView`
  runs after the submit and needs one pumped frame or a `traceGuiTextureNow` kept for it alone.
  Keep only if `repeatable.sh --pairs=10` stays identical on the scene columns and the cost goes.
- [ ] `rtxrenderer.cpp` `updateTraversal`/`drawGui`: `MyGUIRtx::RenderManager::getInstancePtr()`
  per frame where the renderer could keep the pointer it made.
- [ ] `cellplacer.cpp` `place`: every placement of every held cell is re-evaluated per frame; the
  shown state changes only when the eye crosses a cell or the threshold moves.

## Small waste

- [ ] `tracedview.cpp` `redraw`: reads the GUI texture into `mPixels` then `memcpy`s into
  `mCopy->data()`.
- [ ] `framecapture.cpp` `thumbnail`: an RGBA image built at the thumbnail size, then copied
  per pixel into an RGB one.
- [ ] `glrenderer.cpp` `resetPreparationBudget`: allocates an `IncrementalCompileOperation` to read
  two defaults.
- [ ] `cellplacer.cpp`: the "drop the ground slot" block is written twice.
- [ ] `compositequeue.hpp`: `mQueuedAt` is kept in step with the sequence numbers of
  `mPending`/`mDone` by position.
- [ ] `nodekind.hpp`: keys on `libraryName()`/`className()` pointer identity, which holds while
  every OSG class answers with one literal; nothing states that.
- [ ] `scenedesc.hpp`: the write side is a forwarder facade onto the tables the read side hands out.
- [ ] `rtxtool/main.cpp` `parseSize` returns a `std::pair`.
