# Review: the whole diff against `upstream/master`

Delete an item when you address it. Delete a heading when its last item goes. The `[RTX]`
settings pages and their translations are out of scope by decision and are not listed.

Scope: `git diff upstream/master HEAD`, 752 files. Tests are ignored by the review's rule;
they are a third of the lines. Review the gathered rasterizer with
`git diff -M20% --color-moved=dimmed-zebra`, or `glrenderer.cpp`, `glworld.cpp`,
`gloffscreenview.cpp` and `precipitation.cpp` read as 2,600 new lines.

## The rasterizer was changed where the rule says it is the path not taken

Each of these edits an upstream file so that the played GL game behaves differently from
upstream. The fork's rule allows a lift that the rasterizer reads unchanged, and nothing else.
Every one is also diff a reviewer must read as a behaviour change rather than as a move.

- [ ] `mwrender/globalmap.cpp` (83+/274−), `mwrender/pixels.{hpp,cpp}`, `mwgui/mapwindow.cpp`,
      `files/shaders/compatibility/globalmap.{vert,frag}` deleted: the GL global map now composites
      explored cells on the CPU (`GlobalMap::exploreCell` with `sampleBilinear`, `resampleRegion`)
      instead of upstream's camera-and-quad blit. The GL path lost its shaders for the RT path's
      convenience. Restore upstream's `GlobalMap` for the GL host and put the CPU compositing in the
      RT owner (`mwrender/rtx/`) behind the seam, or make the CPU path the one description both read
      and say so as a lift. The `MapWindow::mExploredPending` / `paintExplored` / `mFogAsked` machinery
      and the `WindowManager::setPlayerPos` call go with whichever answer stands.
- [ ] `mwrender/localmap.cpp`: `mapDepthRange` widens the map camera's near/far by
      `Terrain::Storage::getMinMaxHeights`. Upstream clips at the scene sphere alone. This changes
      what the GL local map draws in a tall cell. Either both renderers need it (then it is a lift and
      the comment must say what upstream drew wrong) or only the trace does (then it belongs in
      `TracedView`).
- [ ] `components/sceneutil/optimizer.cpp` (63 lines): stable sorts and index maps make the merge
      order deterministic for the mirror's hashes. It also changes the order the GL renderer merges
      geometry in. Either the extractor sorts what it reads (`digestScene` is already order-blind, and
      `SceneUploader` can sort placements by a key it owns) or the edit is a bug fix to upstream and
      the commit says which GL picture it changed.
- [ ] `mwworld/scene.hpp`: `CompareCellStores` orders `Scene::CellStoreCollection` by location so
      `unloadInactiveCells` walks the same order in every process. That is an order change under the
      GL game too. Same answer as the optimizer: order in the mirror, or justify as a fix. The
      `using is_transparent = void;` with a single `(const CellStore*, const CellStore*)` overload
      enables heterogeneous lookup that nothing can use. Remove it.
- [ ] `components/sdlutil/sdlvideowrapper.{hpp,cpp}` (14+/24−): `VideoWrapper` lost its
      `osgViewer::Viewer` and `setSyncToVBlank`. The GL host's vsync now lives in `GlRenderer`.
      Add a viewer-free constructor beside upstream's instead of rewriting the class, and leave
      `setSyncToVBlank` where upstream calls it.
- [ ] `components/myguiplatform/additivelayer.{hpp,cpp}` (23−/13−): the layer's own `StateSet` was
      replaced by `GuiRenderManager::setAdditiveBlend`. The GL GUI's additive layer is now drawn
      through a call the fork wrote. If `GuiRenderManager` stays (see the seam group), the GL
      implementation of `setAdditiveBlend` must be upstream's state set, byte for byte, and the
      comment must say so.
- [ ] `mwrender/renderingmanager.{hpp,cpp}`: `mWaterHeight`, `mWaterEnabled`, `mWaterToggled` and
      `RenderingManager::isUnderwater` re-implement `Water::isUnderwater` and `Water::isVisible` so the
      RT path can answer without a `Water`. `toggleWater` now flips a `RenderingManager` field and
      tells `Water` second. One owner: `Water` keeps its two flags and `RenderingManager` asks the
      seam (`Renderer::isUnderwater(pos)`), or the flags move whole into `WorldState` and `Water`
      reads them from the frame.

## One truth, two sources

- [ ] `mwrender/rtx/rippleemitters.hpp` documents itself as "a copy of `RippleSimulation::update`'s
      rule" (`isUnderwater && !isSubmerged || isWalkingOnWater`, 12 units per particle). Lift the rule
      into one function both call, the way `components/sky/` was lifted, or have `RippleSimulation`
      produce the impulses and the RT path read them from the frame.
- [ ] Two channels for one light fact. `SceneUtil::LightSource` gained typed `mSourceRadius` and
      `setController`, while `mwworld/projectilemanager.cpp` publishes a bolt's area through
      `setUserValue("spellArea", …)` and `Rtx::lightRadius` reads it back by string
      (`sSpellAreaValue`). Put the spell area on `LightSource` beside `mSourceRadius`, and delete the
      string key.
- [ ] `RtxRenderer::mFrame` is `getFrameStamp().getFrameNumber()` copied at `renderFrame`, and
      `PoseMoment` carries both the stamp and the copy. Read the stamp.
- [ ] Harness defaults restate settings defaults. `RtxTool::FrameRequest` holds `mDistantCells = 4`
      and `sUpscaleByDefault = Quality`, the same knobs `files/settings-default.cfg` `[RTX]` defaults.
      `options.cpp` records that the two drifted once (five cells against four). Read the defaults
      from `Settings::rtx()` and keep no literal in the harness.
- [ ] `apps/rtxtool/hosted.cpp` repeats `apps/openmw/main.cpp`'s engine setup (content files, data
      dirs, `data-local`, fallback archives, encoding, fallback map) and says so in its comment. The
      one-source shape is a function on `OMW::Engine` that takes the variables map, used by both. It
      touches `engine.hpp`; name the trade-off if the copy stays.
## Per-frame work the fork's own rules forbid

- [ ] `GlobalMap::exploreCell` calls `mOverlayImage->dirty()` after compositing one cell. Under GL
      that re-uploads the whole overlay texture; under RT `SharedTexture::refresh` then compares
      every row of the two-megabyte image. Upstream blitted one cell on the GPU. A cell crossing pays
      this as a spike. If the CPU path stays, keep a dirty rectangle beside the image and upload
      that.
- [ ] `MapWindow::paintExplored` runs from `WindowManager::setPlayerPos` every frame and walks
      `mExploredPending` with `std::erase_if` whether or not a tile is pending. Run it from the
      event that completes a tile, or early-out on an empty list.

## The seam carries what only one host or one renderer reads

`MWRender::Renderer` (`renderer.hpp`, 399 lines, about forty members) is the one interface both
renderers implement. Members that only the harness or only the trace uses make the GL side carry
stubs and make the interface read as the RT renderer's.

- [ ] `Renderer::enableReference` / `forgetReferences` are seam members with no-op defaults that
      only `RtxRenderer` overrides, called from `RenderingManager::setEnabled` and
      `notifyWorldSpaceChanged` for the cell ring. A default no-op on the seam is one renderer's
      callback chain wearing the interface's name. Either the seam states the fact for both renderers
      (`WorldState` carries the enable changes of the frame, and each renderer reads them) or the
      ring asks the game's registry (`WorldModel::getPtr(refnum)` and `RefData::isEnabled`, as
      `Check::StaticsNotDoubled` already does) when it stands a reference.
- [ ] `MWBase::WindowManager::getLocalMap()` (8 lines in `mwbase/windowmanager.hpp`,
      `windowmanagerimp.hpp`) has one caller, `apps/rtxtool/stopwriter.cpp:303`. `RtxRenderer` made
      every `TracedView` and knows the map's; let the harness ask the renderer for the tile it drew
      and take the accessor out of the upstream interface.
- [ ] `GroundSpec::mResources` duplicates `Renderer::getResources()`, which every caller of
      `createGround` already holds.
- [ ] `describeWindow(surfaceFlag)` sets SDL hints as a side effect of a describe call, and both
      renderers call it once before creating their window. Make the hint call explicit at the two
      call sites and keep `describeWindow` pure.
- [ ] `MyGUIPlatform::GuiRenderManager` (60 lines, new in `myguiplatform`) exists for two calls,
      `shareTexture` and `setAdditiveBlend`. `MyGUIRtx::RenderManager` and upstream's
      `MyGUIRenderManager` both derive from it. If the additive layer goes back to upstream's state
      set (first group), the interface is one call and belongs on `Rtx::GuiRenderer`'s side, not in
      upstream's directory.
- [ ] `PostProcessor` is GL-only and never built under RT, so `mwlua/postprocessingbindings.cpp`
      (20+/14−), `mwlua/debugbindings.cpp` and `windowmanagerimp.cpp` each grew a null check on
      `getPostProcessor()`. Answer once at the seam (a `Renderer::getPostProcessor()` that the RT
      renderer answers with "none", or bindings registered only by the GL host) and take the four
      checks out of upstream's files.

## Structures that hold two things

- [ ] `RtxTool::FrameRequest` mixes window fields that go to `Settings::video()` and
      `Settings::camera()` (`mWidth`, `mHeight`, `mFieldOfView`, `mVerticalSync`) with trace fields
      that go to `RunSetup` (`mProfile`, `mDistantCells`, `mDistantStatics`). `applyHostedSettings`
      and `mirrorKnobsOf` split it by hand, and `runStops`, `commandBench`, `commandView`,
      `commandCheck` each rebuild `SessionRequest::mSetup` from it. Parse straight into a
      `WindowRequest` and a `RunSetup`, and build `SessionRequest` in one place.
- [ ] `RenderingManager` keeps `mFrameWorld`, `mFrameEye`, `std::optional<SceneFrame> mFrame`,
      `mProjectionMatrix`, `mFrameDelta`, `mFramePaused` beside the seam that consumes them. The
      frame description is the seam's product; own it in one `FrameDescriber` (the `sceneframe.cpp`
      functions are already free of `RenderingManager`'s other state) and let `RenderingManager`
      hold one member.

## Lists kept in two or three places

- [ ] `Rtx::Check` is named in `sChecks` (`benchrun.cpp`), asked-or-not in `canAsk` (`main.cpp`,
      twelve cases) and answered in `checkHolds` (`stopwriter.cpp`, twelve cases). Put the
      `canAsk` predicate in the table beside the name so a new check is two places, not three.
- [ ] `Rtx::ScenePart` has a hand-written `nameOf` switch in `scenedigest.cpp` while every other
      enum in the fork is a `NamedEnum` table. Use the table; the hashes header then comes from it.
- [ ] `NamedEnum<Timing, 13>` in `framespend.hpp` spells the count beside the list, where
      `sChecks` and `sNames` deduce it. Deduce it.
- [ ] `RtxTool::Verbs::Every = 0x3f` restates the six enumerators. Derive it.
- [ ] `apps/openmw/mwrender/objectpaging.cpp` (81+/27−) threads a second kind through upstream's
      collection: `typeFilter(RefKind, …)`, `struct Collected { mPaged, mLit }`, `listFor`,
      `collectESM3References(…, bool lit)`, `collectESM4References(…, bool lit)`, and
      `MWRender::ObjectStorage::collect / getLight / getModel` at the file's end. One `RefKind`
      parameter on `collect` that returns one list does the same with `typeFilter` as the only edit
      to upstream's functions. `Collected` and `listFor` go.

## Diff that buys neither design nor speed

- [ ] `.zed/tasks 1.json` is a tracked ten-line duplicate of one entry of `.zed/tasks.json` with an
      editor's copy-name. Delete it.
- [ ] `components/resource/scenemanager.{hpp,cpp}` (15 lines): `setShadersEnabled(false)` has one
      caller, `RtxRenderer`, so loaded models keep the loader's state. If the RT host can build its
      `SceneManager` so that `createShaderVisitor` is never reached (no shader manager installed,
      or the visitor made by the host), the flag and the edit go. If it cannot, the comment must say
      why.
