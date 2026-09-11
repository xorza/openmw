# Review: whole diff against `upstream/master`

Delete an item when you address it. Delete a heading when its items are gone.

Scope: 815 files, +119k/-4.6k lines. Read in full: the seam (`renderer.hpp`, `stage`, `sceneframe`,
`offscreenview`, `renderingmanager`, `engine`), `mwrender/gl/glrenderer`, `mwrender/rtx/*`, the core's
scene/mirror/cell/composite/material/mesh paths, the Vulkan renderer and pass headers, `terrain/objectpaging`,
`surface/material`, `sky`/`weather` headers, `myguirtx`, the harness entry. Sampled: the rest of
`components/rtxvulkan`, the shaders (a scan for uncalled functions found none), `rtxbench`. Tests were ignored.

## Comments narrate history and restate code

The comment rule is *why* only: an invariant, a workaround and its cause, a trade-off. Across the fork a third of
every line is a comment (`components/rtx` 8194 of 23975, `rtxvulkan` 5468 of 19646, `mwrender/rtx` 1253 of 4338),
~1960 of them open with a bolded thesis, and ~120 tell what the code *used to be*. Headers such as
`mwrender/renderer.hpp` are 430 lines for ~80 of code. This is the largest single cost in the diff to read and to
keep true, and several are already stale.

- [ ] History narration is commit-message material: "The `Capabilities` struct this class once carried"
  (`renderer.hpp`), "It was four virtual calls with one call site" (`renderer.hpp`), "`osgViewer::Viewer` was two
  things and thirteen classes wanted the smaller one" (`stage.hpp`), "They lived in `gl/skyutil.hpp`"
  (`weatherresult.hpp`), "These used to be spelled out here… copied across four times" (`weatherresult.hpp`),
  "`SceneDesc` used to re-export every table one method at a time — forty-one forwarders" (`scenetables.hpp`),
  "It was nine members of the session and four accumulators" (`session.hpp`), "`TracedView` held `RtxRenderer&` and
  read nine things off it" (`viewhost.hpp`), "A file-static used to carry the request" (`setup.hpp`,
  `engine.hpp`). Every one of these describes a diff, not the code.
- [ ] Measurements quoted in prose go stale the moment the code moves: "measured, 48% of the frame moved by up to 29
  of 255" (`rtxrenderer.cpp`), "13 of 360 frames differed" (`rtxrenderer.cpp`), "47% of the frame differed by up to
  38 of 255" (`session.hpp`), "measured at a tenth of a second apiece over sixty rebuilds" (`rtxrenderer.cpp`),
  "was flown at eighty-five to twelve hundred" (`session.cpp`). `CLAUDE.md` already says why a number in prose is
  wrong.
- [ ] `sceneframe.hpp`: the `mWeatherTransition` comment cites `apps/openmw/mwworld/weather.cpp:1261` — a line
  number.
- [ ] `sceneframe.hpp`: the doc block "What there is to draw, and what the world is doing while it is drawn…" is
  glued to `EyeState`; `SceneFrame`, three structs later, has none, and `EyeState` carries two.
- [ ] `sceneframe.hpp`: `mTerrain`'s comment names `Renderer::wantsTerrainChunks`, which does not exist
  (`TerrainPlan::mChunks`).
- [ ] `weatherresult.hpp`: a blank line between the doc block and `struct WeatherResult` detaches it.
- [ ] `renderingmanager.hpp` `mStormParticleDirection`: the comment says the value is
  `Weather::defaultStormDirection` while the initializer is a literal `(0, 1, 0)`.
- [ ] `renderingmanager.cpp` `setWeather`: the first paragraph ("Kept apart rather than multiplied together") is
  about `mSunDiscColour`/`mSunGlare` six lines below, not the line under it.
- [ ] `renderingmanager.cpp` `configureAmbient` vs `describeWorld`: the two comments contradict — one says the same
  test decides the room and clears it, the other says "nothing else ever would" clear it (see the double clearing
  below).
- [ ] `rtxrenderer.cpp` `trace()`: ~110 lines of which ~30 are code; the constructor's validation block is three
  statements under thirty lines of comment. Same shape in `presentWithGui`, `finishBehind`, `handOver`.
- [ ] `rtxrenderer.hpp`, `myguirtx/rendermanager.hpp`: `/*internal:*/` marker comments split the public section.
- [ ] `glrenderer.cpp`: `renderFrame`'s explanation is a `//` block above the definition while every other member is
  documented with `///` in the header.

## Upstream edits go past a lift and change the rasterizer

`CLAUDE.md`: the rasterizer is never modified, a lift leaves it reading what it read before, and any other upstream
edit is named and waits for a go-ahead. ~130 upstream files are modified or renamed (`git diff --name-status`),
some at 45–75% similarity. The ones below change what the rasterizer does or reach outside the named places.

- [ ] `components/terrain/objectpaging.cpp` (R071 from `mwrender/objectpaging.cpp`): per-reference culling measured
  from the eye (`dSqr = (viewPoint - ref.mPosition).length2()`) is replaced by one `chunkReach` per chunk,
  `relativeViewPoint` is forced to zero, and `std::map<RefNum,…>` becomes a vector reduced by `collectPagedRefs`.
  Which references a rasterized chunk merges or drops now differs from upstream.
- [ ] `components/sceneutil/optimizer.cpp`: pointer-keyed `std::map`/`std::set` replaced by insertion-ordered
  vectors and `std::sort` by `std::stable_sort`. Deterministic, but it changes the order upstream's optimizer
  merges geometry in, for both renderers.
- [ ] `renderingmanager.cpp` `renderFrame`: the precipitation is updated from the frame with
  `getInverseViewMatrix().getTrans()` rather than from upstream's cull-time `WrapAroundOperator`. Same matrix,
  same phase; only `mUnderwater` is a frame earlier than upstream's cull-callback answer. Keep, and say so in the
  comment.
- [ ] `mwrender/gl/sky.cpp` (R045), `sky.hpp` (R075), `skyutil.hpp` (R074), `screenshotmanager.hpp` (R052): the
  rasterizer's own files, rewritten around `Sky::`, `Weather::Precipitation` and `OffscreenView`. Less than half of
  `sky.cpp` survives the rename.
- [ ] `RenderingManager::update` calls `Sky::timescaleClouds()` every frame — a `Fallback::Map` string-keyed lookup
  per frame for both renderers, where upstream read `Weather_Timescale_Clouds` once into `mTimescaleClouds`.
- [ ] `apps/opencs/editor.cpp`: upstream set `showMarkers` only; the fork now also gives the editor's NIF loader
  `mHiddenNodeMask = CSVRender::Mask_Hidden`, so hidden nodes in OpenCS carry a mask they did not before.
- [ ] `apps/launcher/graphicspage.cpp`, `apps/launcher/ui/graphicspage.ui`, `apps/opencs/view/render/mask.hpp`,
  `files/lang/*.ts`, `files/data/l10n/*`: edits outside every place `CLAUDE.md` names.
- [ ] `components/nifosg/nifloader.cpp`: a `Surface::Material&` is threaded through ~15 handler signatures beside
  the `args` bundle that already carries `mMaterial`; the parameter belongs in the bundle, not beside it.
- [ ] `apps/openmw/mwgui/*` (19 files), `mwlua/*`, `mwinput/*`, `mwworld/weather.{hpp,cpp}` (-539/+90),
  `components/esmloader/*`, `components/myguiplatform/*` (14 files), `components/sdlutil/*`,
  `components/sceneutil/*` (13 files): worth a pass to separate the lift from the edit in each.

## Per-frame work the "computed once" rule forbids

- [ ] `worldmirror.cpp` `mirror()`: reads `Settings::terrain().mObjectPaging`, `mObjectPagingMinSize`,
  `Settings::rtx().mDistantLandCells` and `Settings::camera().mViewingDistance` (through `landReach()`) every
  frame; `landReach()` is called again in `RtxRenderer::trace` and in two checks. Read once, hold on the mirror.
- [ ] ~~`scenedesc.cpp` `setMaterial` walks all placements per animated material per frame~~ — withdrawn on a
  closer read: `MaterialTable::set` returns true only when the traversed subset changed, so a flipbook or UV
  scroll never walks. See `redesign-plan.md`.
- [ ] `surface/material.cpp` `getMaterial`: a linear scan of the `UserDataContainer` comparing `getName()` strings.
  `findDescription` runs it per state set in the shading stack — per emitter per frame (`EmitterResolver::add`) and
  per animated material per frame. A typed user object or a fixed slot answers without a string compare.
- [ ] `sceneextractor.cpp`, `nodelibrary.hpp`, `materialresolver.cpp`, `posecull.hpp`, `poseupdate.hpp`: the mirror
  walk still `dynamic_cast`s per node — `castFrom<Skeleton>` for every SceneUtil group, `castFrom<LightSource>`,
  `ParticleProcessor`/`ParticleSystemUpdater` in `stepParticles`, `StateSetUpdater` in `findUpdater` for every
  node with a callback, `ParticleSystem` per drawable, two more per node in `PoseCull::apply`. The library check
  narrows but does not remove the cast; `isExactly` + `static_cast` would.
- [ ] `cellring.cpp` `ask()`: rebuilds and sorts the whole `(2·band+1)²` request list every frame, each entry doing a
  sorted lookup plus a linear `handed()` scan, then hands it to the supply, whether or not the eye cell moved.
- [ ] `mirroridentity.hpp` `Kept`: the identity maps (`mPlacements`, meshes, materials, rigs, morphs, emitters,
  animated) are `std::unordered_map`s that are never reserved; a cell arriving grows them and rehashes on the
  frame. `retire` erases node by node.
- [ ] `rtxrenderer.cpp` `eventTraversal`: `SDLUtil::InputWrapper` pushes every event into the stage's queue and this
  drains the `std::list` to drop it every frame. Nothing needs the queue under this renderer.
- [ ] `rtxrenderer.cpp` `updateTraversal`/`drawGui`: `MyGUIRtx::RenderManager::getInstancePtr()` static-cast from
  MyGUI's singleton per frame; the renderer built the manager in `createGuiPlatform` and could keep the pointer.
- [ ] `vulkanrenderer.cpp` `traceGuiTexture`: `mRing.finishAll()` then `submitAndWait` — a full pipeline drain for
  every doll or map-tile redraw; `dropViewScene` does `waitIdle` as well. Both land as a spike on the frame that
  opened the inventory.
- [ ] `distantlights.cpp` `build()`: allocates a `std::vector<PagedCellRef>` and an `osg::Group` with a
  `MatrixTransform` per light for every cell that enters reach — on the load path the "loading allocates no more
  freely than a frame" rule covers.
- [ ] `cellring.cpp` `walkRings`/`sift`: `erase` inside the loop over `SortedRows` (a sorted vector) shifts the tail
  per drop; a worldspace change drops many cells in one frame.

## Two spellings of one fact

- [ ] `sceneframe.hpp` `RoomMood` + `WorldState::mFogDepth` are the four fields of `ESM::Cell::AMBIstruct` under new
  names; `readworld.cpp` rebuilds the `AMBIstruct` from them. Carry the record.
- [ ] `renderingmanager.cpp`: `configureAmbient` clears `mWorld.mRoom` in its `else` branch *and* `describeWorld`
  clears it when the location is not `Interior`. One of the two is dead, and the comments disagree about which.
- [ ] `renderingmanager.cpp` `update` reads `mSky->getBaseWindSpeed()` for the rasterizer's uniform while
  `setWeather` stores `mWorld.mBaseWindSpeed` from the same `WeatherResult`. `SkyManager::getSkyColor()` and
  `SkyManager::mWindSpeed` (initialised, never written) are left over from the same move.
- [ ] `renderingmanager.cpp` `getTerrainReach`: encodes "unwidened" as `fov = 0` through `GlRenderer`'s `cos()`
  formula — a fact about one renderer's implementation leaking into the caller. Ask the plan for the reach.
- [ ] `framereport.hpp` `FrameContext`: `mHost` (`ViewHost&`) and `mViews` (`Renderer&`) are the same
  `RtxRenderer` through two bases. `viewhost.hpp` `ViewHost::getSceneRoot()` restates `Stage::getSceneRoot()` /
  `hasSceneRoot()`.
- [ ] `rtx/renderer.hpp` `SceneHeld::mScene` is a `const MeshTable*` compared by address in `SceneUploader::hand`
  to decide whether the renderer holds "my" scene, while `SceneSlot` already names the scene.
- [ ] `stopwriter.cpp` `writeMapTile` builds its own map framing (512 px, eye at 50000, far 150000) beside
  `LocalMap`'s (resolution setting, z-range from the cell's bounds). The "map tile" a stop writes is not the game's
  map tile.
- [ ] `rtxrenderer.cpp`: `renderGui()` is a one-line forward to `presentWithGui()`; `sceneextractor.cpp`:
  `addDrawable()` is a one-line forward to `mirrorDrawable()`.
- [ ] `residency.hpp`/`sceneextractor.cpp`: `SceneAdopter` is implemented by `MirrorTraversal` forwarding to
  `SceneExtractor::adoptMesh` (private, friend) forwarding to `MeshResolver::adopt` — three layers for one call.
- [ ] `scenedesc.hpp`: the write side is still a forwarder facade — `addRig`/`addMorph`/`poseRig`/`poseMorph`/
  `addMask`/`addLayers`/`hold*`/`drop*`/`fadeInstance`/`moveInstance`/`dropInstance` are one-liners onto the
  tables `SceneTables` already hands out on the read side.
- [ ] `preparedcell.hpp`, `preparedground.hpp`, `preparedtexture.hpp`, `heldcell.hpp`, `compositequeue.hpp`,
  `session.cpp` `StopProgress`: eight hand-written `reuse()`/`restart()` lists that a new field reaches only if its
  author remembers — the failure `session.hpp` warns about in its own words.
- [ ] `rtxrenderer.cpp` ctor: `Settings::rtx()` is read in three places (`profileFromSettings`, `mFixedStep`,
  `readSessionSetting`) while `RtxSetup` claims to carry "the knobs a measurement turns"; the fixed step is such a
  knob and the harness reaches it only through the settings registry.
- [ ] `benchrun.hpp` `Actions::mDoll` + `mDollOut`: two fields for the one action whose every sibling is a single
  path.

## Errors turned into strings and back, or swallowed

- [ ] `vulkanrenderer.cpp` `createVulkanRenderer` catches `Unsupported`, returns null plus a string,
  `rtxbackends/renderer.cpp` forwards both, and `RtxRenderer` re-throws `runtime_error` from the string;
  `setUpscale` on the same interface throws `Rtx::Error` directly. One convention.
- [ ] `rtxbackends/renderer.cpp`: `<SDL_video.h>` is included after `<components/rtx/renderer.hpp>`.
- [ ] `compositequeue.cpp` `Baker::bake`: `catch (const Error&) { continue; }` drops a layer silently, then
  `catch (std::exception)` logs and returns a composite of nothing.
- [ ] `cellreader.cpp` `readTexture`: `catch (const Error&)` marks the texture unreadable and carries on;
  `CellReader::read` runs `readTexture` from inside an `std::erase_if` predicate.

## Harness side effects

- [ ] `stopwriter.cpp` `writeDoll`: `world.placeObject` spawns the NPC into the player's cell and nothing deletes it —
  every doll stop leaves a body standing for the stops after it.
- [ ] `session.cpp` `frame()`: `renderer` is fetched before the `mDone || !mStarted` early return.

## Wide signatures and raw flag tuples

- [ ] `rtxvulkan/image.hpp` `transition(commands, from, to, srcStage, srcAccess, dstStage, dstAccess)`: seven
  parameters at ~30 call sites, each spelling stage/access pairs by hand (`COMPUTE_SHADER_BIT,
  SHADER_STORAGE_WRITE_BIT` appears 27 times). A named `{layout, stage, access}` use struct per side.
- [ ] `skybuilder.hpp` `describeClouds(weather, next, blend, light, storm, nextStorm, scroll, textures)`: eight
  arguments unpacked from a `WorldReading` the caller already holds.
- [ ] `sceneuploader.hpp` `hand(renderer, slot, scene, images, composites*, sea, readings*, spend*)`: seven
  parameters, three nullable, with a `FrameSpend unread` stand-in for a null.
- [ ] `tonepass.hpp` `record` (8), `visibilitypass.hpp` `VisibilityPass` (8), `image.hpp` `Image` (8),
  `transitionLevels` (9), `moonmodel.hpp` `MoonModel` (10 floats).
- [ ] `rtxtool/main.cpp` `parseSize` returns a `std::pair`.
- [ ] `frameworld.cpp` `describeWorld` writes `constants` through an out-parameter and returns the exposure bias as
  a bare `float`.

## Dead code

- [ ] `rtxbench/runrecord.hpp`: `getExitStatus()`, `getPlaces()` — no callers.
- [ ] `rtxvulkan/physicaldevice.hpp` `getProfile()`, `rtxvulkan/image.hpp` `getTexelBytes()` — no callers.
- [ ] `rtx/frameworld.{hpp,cpp}` `noPatches()` — declared and defined, never called (`describeWorld` fills with
  `noPatch()` in a loop).
- [ ] `mwrender/gl/sky.hpp` `getSkyColor()`, `mWindSpeed` (see above).
- [ ] `rtxvulkan/device.hpp` `canDescribeFault()`, `sky/timeofday.hpp` `hasSetting()` — test-only.
- [ ] `glrenderer.cpp`: stale includes after the move — `<fstream>`, `<osgDB/ReaderWriter>`, `<osgDB/Registry>`,
  `<osg/Image>`, `sdlutil/imagetosurface.hpp`, `l10n/manager.hpp` (their users went to `windowsetup.cpp` and
  `screenshotwriter.cpp`). `tracedview.cpp` includes `<variant>` for nothing.

## Double lookups and small waste

- [ ] `materialresolver.cpp` `adopt(const MaterialReading&)` and `meshresolver.cpp` `adopt(...)`: `find`, then
  `add`, then `find` again for the entry `add` just made; `Kept::add` could return the entry `emplace` gives it.
- [ ] `tracedview.cpp` `redraw`: reads the GUI texture into `mPixels` and then `memcpy`s into `mCopy->data()`,
  where the image's own buffer could be the read target.
- [ ] `framecapture.cpp` `thumbnail`: `frameImage` builds an RGBA image at the thumbnail size, then a per-pixel loop
  copies it into an RGB one.
- [ ] `glrenderer.cpp` `resetPreparationBudget`: allocates a fresh `IncrementalCompileOperation` to read two default
  numbers.
- [ ] `cellplacer.cpp`: the three-line "drop the ground slot" block is written twice (`dropGround`, `dropSlots`).
- [ ] `compositequeue.hpp`: `mQueuedAt` is a second deque kept in step with the sequence numbers of `mPending`/
  `mDone` by position; `getDue` only holds while every filed `Baked` (including the re-ask path in `gather`) keeps
  the count aligned.
- [ ] `nodelibrary.hpp` `NodeLibrary::of`: compares `libraryName()` by pointer before falling back to `learn`.
  Works while every OSG class returns one static literal; nothing states that.

## Include order and layout nits

- [ ] `rtxrenderer.cpp`: `#include "setup.hpp"` sits between the own header and the standard block.
- [ ] `sceneframe.hpp`: `"weatherresult.hpp"` sits between `<optional>` and `<osg/...>`.
- [ ] `renderingmanager.hpp`: `"weatherresult.hpp"` isolated by blank lines; a stray blank line after
  `namespace MWRender {`.
- [ ] `glrenderer.cpp`: no blank line between `createWindow`'s closing brace and `getTerrainPlan`; the local
  include block is `../renderingmanager`, `../sceneframe`, `../stage`, `../windowsetup`, `gloffscreenview`,
  `../screenshotwriter`, `postprocessor`, `screenshotmanager` — neither one block nor sorted.
- [ ] `apps/openmw/main.cpp`: `<osg/Notify>` between two quoted local headers.
- [ ] `gloffscreenview.cpp`: `<osgUtil/...>` in the same block as `<osg/...>`.
