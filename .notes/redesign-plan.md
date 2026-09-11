# Redesign and implementation plan

Companion to `review-upstream-diff.md`. Each section states what the code does now (read, not inferred), what
it should be, and the steps. Items are ordered by phase, and a phase is done when its verification line is green.
Delete a section when it lands.

Two review findings did not survive the closer read and are withdrawn here rather than planned:

- `SceneDesc::setMaterial` walks every placement **only when `MaterialTable::set` reports a reclassification**
  (the traversed subset — kind, cutout, translucent, medium — changed). A flipbook or a UV scroll changes the
  texture or the transform and never walks. What remains is a scan on the rare frame an alpha controller crosses
  `1.0`, which is not worth an index. No change.
- The precipitation eye moved from a cull-time `WrapAroundOperator` reading `mCamera->getInverseViewMatrix()` to
  `RenderingManager::renderFrame` reading the same matrix after the same update traversal. The one difference is
  `mUnderwater`: upstream's `UnderwaterSwitchCallback` answered from the previous cull, the lift asks `Water`
  directly, which is a frame earlier and exact. Keep it, and say so in the lift's comment.

---

## Phase 0 — decisions the plan cannot make

Each of these changes upstream behaviour and needs a yes or a no before anything else is touched, because the
answer decides whether a file is reverted or documented.

### 0.1 `Terrain::ObjectPaging::createChunk` no longer measures from the eye

**Now.** Upstream's `createChunk(size, center, activeGrid, viewPoint, compile, lod)` culls each reference by
`dSqr = (viewPoint - ref.mPosition).length2()` against `minSize` and `minSizeMerged`, and builds the merged
geometry relative to `viewPoint - worldCenter`. The fork's `createChunk(size, center, activeGrid, compile, lod)`
replaces every `dSqr` with one `chunkReachSquared = max(size * cellSize, 1)²` and `relativeViewPoint` with zero.
The ray tracer never builds an `ObjectPaging` (`RtxRenderer::getTerrainPlan().mObjectPaging = false`), so this
changes what a *rasterized* chunk contains and nothing else.

**Recommended.** Restore upstream's signature and arithmetic in `components/terrain/objectpaging.{hpp,cpp}`:
the `viewPoint` parameter, the per-reference `dSqr`, `relativeViewPoint = viewPoint - worldCenter`. Keep the
lift proper — `ObjectStorage` injection, `nodeMask` and `pageActiveGrid` parameters, `PagedCellRef` with
`mType`, `collectPagedRefs` with its sorted `reduce` (it yields the same `RefNum` order upstream's
`std::map` did, so chunk contents match upstream once the eye distance is back).

**If kept instead.** Add the change to the list of upstream edits in `CLAUDE.md` with its reason, because the
"rasterizer is never changed" sentence is false while it stands unnamed.

### 0.2 `SceneUtil::Optimizer` merge order

**Now.** `MergeGeometryVisitor` keyed its duplicate lists on `std::map<ref_ptr<Geometry>, …, LessGeometry>` and
`std::map<StateSet*, std::set<Group*>>`; iteration order was address order. The fork keeps the maps as indices
into insertion-ordered vectors and uses `std::stable_sort`. The picture is the same; which geometry survives as
the merge target and the order of the merged children is now the order the file was read in.

**Recommended.** Keep. `repeatable.sh` gates on the scene columns, and a graph whose merge order depends on the
allocator would put the mesh table in a different order on every run. Name it in `CLAUDE.md` beside the other
upstream edits, with that reason.

### 0.3 `apps/opencs/editor.cpp` hidden-node mask

**Now.** Upstream: `NifOsg::Loader::setShowMarkers(true)`. Fork: `configure({ .mHiddenNodeMask =
CSVRender::Mask_Hidden, .mShowMarkers = true })`. The editor's hidden NIF nodes now carry `Mask_Hidden`, which
they did not before.

**Recommended.** Revert to `.mHiddenNodeMask = 0` (the `Configuration` default) so the editor keeps upstream's
behaviour; the `configure` API change itself is the lift and stays.

### 0.4 Launcher and localisation files

`apps/launcher/graphicspage.{cpp,ui}` and `files/lang/*.ts`, `files/data/l10n/*` add the RTX settings page. They
are outside every place `CLAUDE.md` names. Either name them there as sanctioned or move the page under
`files/rtx/` and a `[RTX]`-only branch in the launcher.

---

## Phase 1 — correctness and posture

### 1.1 `Surface::getWritableMaterial` runs under the rasterizer

**Now.** `nifosg/controller.cpp` calls `Surface::getWritableMaterial(*stateset)` from `UVController`,
`AlphaController`, `MaterialColorController` and `FlipController::apply`, which upstream's update traversal runs
for both renderers. `setMaterial` returns early on `!sDescribing`; `getWritableMaterial` does not — it reads the
container, scans it by name, and clones on a shared count. Under OpenGL the scan finds nothing and returns null,
but the rasterizer pays a container walk per animated state set per frame for a description it never asked for.

**Redesign.** Gate both accessors on `sDescribing`, and make the lookup a pointer compare:

```cpp
// surface/material.cpp
class Holder : public osg::Object { … };
const Holder* holderIn(const osg::UserDataContainer& c)
{
    // Always slot 0: setMaterial inserts at 0 so nothing after it has to be scanned.
    const osg::Object* first = c.getNumUserObjects() > 0 ? c.getUserObject(0) : nullptr;
    return first != nullptr && first->libraryName() == Holder::sLibrary && first->className() == Holder::sClass
        ? static_cast<const Holder*>(first) : nullptr;
}
```

`META_Object` returns one literal per class, so the two pointer compares identify the holder without a string.
`setMaterial` inserts the holder at index 0 (`insertUserObject(0, …)`), so `holderIn` is O(1).
`getWritableMaterial` returns null immediately when `!sDescribing`.

**Steps.** `components/surface/material.cpp` only. Test: `SurfaceMaterial*` in `apps/components_tests/surface`
— add a case that a state set with two unrelated user objects and a holder still answers, and that
`describeSurfaces(false)` makes `getWritableMaterial` return null.

### 1.2 `StopWriter::writeDoll` leaves the NPC in the world

**Now.** `world.placeObject(ref.getPtr(), player.getCell(), position)` puts a live NPC beside the player and
`InventoryPreview` draws it; nothing removes it, so every stop after a doll stop stands next to it.

**Redesign.** `world.deleteObject(subject)` after `writeView`, in every path out of the function (RAII guard or
a single exit). `MWBase::World::deleteObject` exists.

### 1.3 `RoomMood` is `ESM::Cell::AMBIstruct` under other names

**Now.** `configureAmbient` copies `cell.getMood()`'s three colours into `RoomMood`; `readWorld` rebuilds an
`AMBIstruct` from them plus `mFogDepth` to call `Rtx::makeRoomLight`.

**Redesign.** `std::optional<ESM::Cell::AMBIstruct> mRoom` in `WorldState`; `configureAmbient` stores
`cell.getMood()` whole; `readWorld` passes it through. `mFogDepth` stays, because it is read outdoors too.
`sceneframe.hpp` gains `#include <components/esm3/loadcell.hpp>`; that header is already pulled in by
`renderingmanager.hpp`'s neighbours.

### 1.4 `mRoom` is cleared in two places with two explanations

**Now.** `configureAmbient` has an `else mWorld.mRoom.reset()`; `describeWorld` also resets it whenever
`mLocation != Interior`. `MWWorld::Scene` calls `configureAmbient` only for rooms, so the `else` runs only from
the "classic falloff" settings path, and the comment in `describeWorld` ("nothing else ever would") is the
accurate one.

**Redesign.** Delete the `else` branch. Keep the one rule in `describeWorld`. Rewrite both comments to state the
single rule: the record is written when a room is entered and masked by the location.

### 1.5 `createRenderer` returns null plus a string

**Now.** `VulkanRenderer`'s constructor throws `Unsupported`; `createVulkanRenderer` catches it into
`std::string& reason` and returns null; `rtxbackends::createRenderer` forwards both; `RtxRenderer` throws
`std::runtime_error("no ray tracing renderer: " + reason)`; `rtxtool runInfo` prints `reason`. The same interface's
`setUpscale` throws `Rtx::Error` directly.

**Redesign.** `std::unique_ptr<Renderer> createRenderer(const RendererOptions&)` throws `Unsupported` (a build with
no backend throws it too). `RtxRenderer`'s constructor lets it propagate — `Engine` already reports the message.
`runInfo` catches `const Unsupported&` and prints `what()`. Delete the `reason` parameter from both factories.

### 1.6 `CompositeQueue::Baker::bake` drops layers silently

**Now.** A layer whose image `describeImage` refuses is skipped with `catch (const Error&) { continue; }`; a
composite that fails to bake is logged and returned empty.

**Redesign.** Count both on `Baked` (`mUnreadableLayers`, `mFailed`) and add them to `SceneUpload::mUnreadable`
in `SceneUploader::hand`, where the other unreadable textures are already summed and reported. The warning
`RtxRenderer::handOver` prints then covers composites.

---

## Phase 2 — the frame path

### 2.1 Settings read per frame in the mirror

**Now.** `WorldMirror::mirror` reads `Settings::terrain().mObjectPaging`, `mObjectPagingMinSize` and, through
`landReach()`, `Settings::rtx().mDistantLandCells` and `Settings::camera().mViewingDistance` on every frame.
`landReach()` is called again in `RtxRenderer::trace` and in `GroundReaches`/`GroundStands`. The view distance
already reaches the renderer through the seam: `RenderingManager::setViewDistance` →
`Renderer::getTerrainViewDistance(cameraDistance, fov)`.

**Redesign.**

```cpp
// rtxrenderer.hpp
struct MirrorSettings { bool mStatics; float mMinSize; float mDistantLandCells; };   // read once in the ctor
float mReach = 0.f;   // written by getTerrainViewDistance, read by mirror() and trace()
```

- `RtxRenderer::getTerrainViewDistance(cameraDistance, fov)` becomes non-const on the inside via a `mutable`
  or, cleaner, `RenderingManager` calls it and the renderer stores `mReach = distantLandReach(mDistantLandCells,
  cameraDistance)`. `WorldMirror::mirror` takes the reach as a field of a `MirrorFrame` argument beside the
  `SceneFrame`, and `readWorld` takes it from the renderer.
- `CellRing::setStaticsEnabled`/`setMinSize` are called once from `WorldMirror`'s constructor with the
  `MirrorSettings`. A live change of those two settings needs a restart; state that in
  `docs/source/reference/modding/settings/rtx.rst`.
- `landReach()` in `worldmirror.hpp` goes; the checks read `context.mReach` (added to `FrameContext`, see 3.1).

### 2.2 `dynamic_cast` per node in the mirror walk

**Now.** `NodeLibrary::of` caches `libraryName()` pointers to answer which library a node is from, but every
kind test still ends in a `dynamic_cast`: `castFrom<Skeleton>` for each SceneUtil group, `castFrom<LightSource>`
for each SceneUtil leaf, `ParticleProcessor` and `ParticleSystemUpdater` in `stepParticles`, `ParticleSystem` per
drawable in `mirrorDrawable`, `RigGeometry`/`MorphGeometry` in `readDrawable`, `LightSource` in
`PoseUpdate::apply`, two in `PoseCull::apply`, `Sequence` in `descendInWorld`, and `StateSetUpdater` per callback
per node with a callback in `MaterialResolver::findUpdater`.

**Redesign.** Generalise the library cache into a kind cache keyed on the `(libraryName(), className())` pointer
pair — both are string literals from `META_Object`/`META_Node`, so a pair identifies a dynamic type exactly.

```cpp
// rtx/nodekind.hpp  (replaces nodelibrary.hpp)
enum class NodeKind : std::uint8_t { Other, Skeleton, LightSource, ParticleSystem, ParticleProcessor,
                                     ParticleUpdater, Sequence, RigGeometry, MorphGeometry };
class NodeKinds
{
public:
    NodeKind of(const osg::Object& object) const;   // pointer-pair lookup; miss → learn() with dynamic_casts, once
private:
    struct Learned { const char* mLibrary; const char* mClass; NodeKind mKind; };
    static constexpr std::size_t sKept = 32;
    mutable std::array<Learned, sKept> mLearned{}; mutable std::size_t mHeld = 0;
};
template <class T> T* as(NodeKind held, NodeKind wanted, osg::Object& o) { return held == wanted ? static_cast<T*>(&o) : nullptr; }
```

- `learn()` performs the casts one time per distinct class; after that a node costs two pointer compares.
  `sKept` overflow falls back to the casts and is counted, so a content set with more classes than the table
  shows up in `ExtractionStats`.
- `stepParticles`, `enter`, `mirrorDrawable`, `readDrawable`, `PoseUpdate`, `PoseCull` and `descendInWorld` switch
  to `NodeKinds::of` and `as<>`.
- `findUpdater` is the one that cannot be classified by class name (every `StateSetUpdater` subclass has its
  own). Cache it instead: `MaterialResolver::animate` calls `mAnimated.reach(&node)` **first** and stores the
  found `StateSetUpdater*` (or null) in the entry on arrival; the callback chain is walked once per node, not per
  frame. Nodes with callbacks but no updater get a null entry and return early.

**Test.** `apps/components_tests/rtx/nodekind.cpp`: a `Skeleton`, a `LightSource`, a `ModularEmitter`, a
`ParticleSystemUpdater`, a `Sequence`, a `RigGeometry` and a plain `Group` answer their kinds; the second call of
each does not enter `learn` (count it through an `internals` hook).

### 2.3 `CellRing::ask` rebuilds the whole band every frame

**Now.** `walkRings` → `ask(eye, band)` clears `mAsking.mCells`, walks `(2·band+1)²` cells, each with a sorted
`holds()` and a linear `handed()` scan, sorts by distance, then `CellSupply::ask` compares the vector to the last
one and hands it over only on change.

**Redesign.** Track the three inputs the list depends on and rebuild only when one changed:

```cpp
bool mAskStale = true;  // set by: eye cell change, adoptHanded, dropCell, discard, setStaticsEnabled, follow()
```

`walkRings` computes `eye = cellOf(mAround.mEye)`; `if (eye != mLastEye) { mLastEye = eye; mAskStale = true; }`.
`ask()` runs only when `mAskStale`, then clears it. `CellSupply::ask`'s comparison stays as the second guard.

### 2.4 Identity maps grow on the frame

**Now.** `Kept<std::unordered_map<…>>` for placements, meshes, materials, textures, rigs, morphs, emitters,
animated nodes — none reserved, so a cell's drawables arriving on a frame rehash the map on that frame.

**Redesign.** `Kept` gains `reserve(n)`; `SceneExtractor`'s constructor reserves each with a stated budget
(placements 64k, meshes 16k, materials 16k, textures 8k, rigs/morphs/emitters/animated 2k). The budgets are
constants in `sceneextractor.cpp` with the reason: a map that never rehashes on a frame. `retire` keeps
erasing node by node — an erase is bounded and does not move other entries.

### 2.5 One event per frame pushed to be dropped

**Now.** `SDLUtil::InputWrapper` pushes `frame(0.f)` and the F-keys into the stage's `osgGA::EventQueue` every
frame; `RtxRenderer::eventTraversal` `takeEvents` into a local `std::list` and drops it. That is one
`GUIEventAdapter` allocation per frame under the ray tracer.

**Redesign.** `InputWrapper` takes `osgGA::EventQueue*`; `WindowManager`/`InputManager` hand it
`mStage.hasEvents() ? &mStage.getEvents() : nullptr`; `RtxRenderer` adopts no queue (`Stage::adopt` gets an
optional). `eventTraversal` on the ray tracer becomes empty. `sdlinputwrapper.{hpp,cpp}` are already edited by the
fork, so this widens no footprint.

### 2.6 Offscreen views drain the device

**Now.** `VulkanRenderer::traceGuiTexture` does `mRing.finishAll()` and then `submitAndWait`; `dropViewScene`
does `waitIdle`. A doll redraw (equip, unequip, inventory open) or a map tile stalls the CPU for the whole
in-flight frame plus the trace.

**Redesign, gated by a measurement.** Record the view trace into the *frame's* command buffer:
`RtxRenderer::drawDeferredViews` already runs between hand-over and trace; make every `TracedView::redraw`
defer (never trace immediately), and add `FrameSource::recordGuiTrace(GuiSlot, constants, options)` that
`traceWorld` calls inside `renderFrame`'s recording, before the world trace, with the copy into the GUI texture
ordered by the frame fence. `readGuiTexture` for `keepCopy` then waits on that frame's fence, which the harness
can afford. Measure with `bench --views=<one>` while toggling the inventory; keep only if the stall goes.

### 2.7 Load-path allocation in `DistantLights::build`

**Now.** Per cell entering reach: a local `std::vector<PagedCellRef>`, an `osg::Group`, and a
`MatrixTransform` per light — a graph built to be walked by the mirror once per frame.

**Redesign.** The refs vector becomes a member scratch (`mRefScratch`, `clear()` per build, like
`CellReader::mRefScratch`). The node graph stays: `standLight` is the one route both kinds of lamp take and it
wants a node. Keep a `Recycled<osg::ref_ptr<osg::Group>>` of emptied groups if profiling shows the allocation.

### 2.8 `SortedRows::erase` inside the walk

**Now.** `walkRings` and `sift` erase from a sorted vector while iterating, shifting the tail per drop.

**Redesign.** Mark-and-compact: mark dropped cells (`mDropped` flag on `HeldCell`), then one
`std::erase_if` after the loop. Same for `mHanded`.

---

## Phase 3 — structure

### 3.1 `FrameContext` and `ViewHost`

**Now.** `RtxRenderer` inherits `Renderer` and `ViewHost`. `FrameContext{ ViewHost& mHost; Renderer& mViews;
const SceneDesc& mScene; }` holds the renderer twice. `TracedView` uses `getBackend`, `hasScene`, `describePose`,
`deferRedraw`, `forgetView`; `StopWriter`/`checks`/`Session` use `mHost.getBackend`, `mHost.getResources`,
`mHost.getSceneRoot`, `mViews.createOffscreenView`.

**Redesign.**

```cpp
struct FrameContext            // plain data, borrowed for one stop
{
    Rtx::Renderer& mBackend;
    Renderer& mViews;          // the seam, for createOffscreenView and InventoryPreview
    Resource::ResourceSystem* mResources;
    osg::Group* mSceneRoot;
    const Rtx::SceneDesc& mScene;
    float mReach;              // 2.1
};
```

`ViewHost` loses `getResources` and `getSceneRoot` (no view calls them). `RtxRenderer::describeContext` fills the
struct from `mStage`/`mResources`. `checks.cpp` stops calling `landReach()`.

### 3.2 `SceneHeld::mScene` pointer identity

**Now.** `describeHeld(slot)` returns the `MeshTable*` the backend last built from; `SceneUploader::hand` compares
it with `&scene.getTables().mMeshes` to decide "rebuilt vs extended". A slot and a `SceneDesc` are already one to
one (world slot ↔ `WorldMirror::mScene`; each `OffscreenTrace` owns its scene and its slot).

**Redesign.** `struct SceneHeld { bool mBuilt = false; std::uint64_t mStructureRevision; std::uint32_t
mTextureCount; }`. `mine` becomes `held.mBuilt`. `ViewScene::mBuiltFrom` goes.

### 3.3 `SceneAdopter` forwarding layers

**Now.** `MirrorTraversal : SceneAdopter` forwards `adoptMesh`/`adoptMaterial`/`releaseMesh`/`releaseMaterial`
to `SceneExtractor`'s private one-liners, which forward to the resolvers; `SceneExtractor` is a friend for it.

**Redesign.** `SceneExtractor` implements `SceneAdopter` itself (it owns the resolvers) with `take(node)` as
`mWalk->take(node)`; residents get `*this`. `MirrorTraversal` stops inheriting `SceneAdopter`, the friend
declaration and the four private forwarders go. `addDrawable` is renamed to what it is (`mirrorDrawable`) and the
one-line wrapper deleted.

### 3.4 `Kept::add` returns nothing

**Now.** `MaterialResolver::adopt(const MaterialReading&)` and `MeshResolver::adopt` do `find`, `add`, `find`.

**Redesign.** `Entry Kept::add(key, held)` returns the emplaced iterator. Both `adopt`s use it.

### 3.5 Hand-written `reuse()` lists

**Now.** `PreparedModel`, `PreparedCell`, `PreparedGround`, `PreparedTexture`, `HeldGround`,
`CompositeQueue::Request`, `Session::StopProgress::restart` each list their fields; a new scalar is reset only if
its author remembers.

**Redesign.** Split each into facts and buffers:

```cpp
struct PreparedModel
{
    struct Facts { std::string_view … ; float mRadius = 0.f; std::uint32_t mLent = 0; … } mFacts;   // reset by = {}
    std::vector<PreparedPart> mParts; std::vector<osg::Vec3f> mPositions; …                             // clear()
    void reuse() { mFacts = {}; mParts.clear(); mPositions.clear(); … }
};
```

A new scalar lands in `Facts` and is reset for free; only a new buffer needs a `clear()` line, and a buffer
forgotten shows up as growth, which the allocation test catches. `std::string mPath` stays a buffer (`clear()`
keeps its room).

### 3.6 `Image::transition` and its seven flags

**Now.** `transition(commands, from, to, srcStage, srcAccess, dstStage, dstAccess)` at 30 call sites,
`describeTransition` and `transitionLevels` with the same tail; `dispatch.hpp::handOver` takes four of them.

**Redesign.**

```cpp
struct ImageUse { VkImageLayout mLayout; VkPipelineStageFlags2 mStage; VkAccessFlags2 mAccess; };
namespace Use {
    inline constexpr ImageUse sComputeWrite{ GENERAL, COMPUTE_SHADER, SHADER_STORAGE_WRITE };
    inline constexpr ImageUse sComputeRead{ GENERAL, COMPUTE_SHADER, SHADER_STORAGE_READ | SHADER_SAMPLED_READ };
    inline constexpr ImageUse sTransferSource{ TRANSFER_SRC_OPTIMAL, COPY, TRANSFER_READ };
    inline constexpr ImageUse sColourAttachment{ COLOR_ATTACHMENT_OPTIMAL, COLOR_ATTACHMENT_OUTPUT, READ|WRITE };
    inline constexpr ImageUse sPresent{ PRESENT_SRC_KHR, ALL_COMMANDS, MEMORY_READ };
    inline constexpr ImageUse sUndefined{ UNDEFINED, TOP_OF_PIPE, 0 };
    …
}
void transition(VkCommandBuffer, ImageUse from, ImageUse to) const;
VkImageMemoryBarrier2 describeTransition(ImageUse from, ImageUse to) const;
void transitionLevels(VkCommandBuffer, std::uint32_t base, std::uint32_t count, ImageUse from, ImageUse to) const;
```

The 27 spellings of `COMPUTE_SHADER_BIT, SHADER_STORAGE_WRITE_BIT` become one name; a call site that needs an
odd pair writes an `ImageUse{}` inline. `handOver` in `dispatch.hpp` takes two `BufferUse{stage, access}`.

### 3.7 `describeClouds` and `SceneUploader::hand`

- `describeClouds(const WorldReading&, const DeckLight&)` — every other argument is a field of the reading the
  caller holds.
- `SceneUploader::hand(SceneSink&, const Handing&)` with `struct Handing { SceneSlot mSlot; SceneDesc& mScene;
  Resource::ImageManager& mImages; CompositeQueue* mComposites; SeaState mSea; const TextureReadings* mReadings;
  FrameSpend* mSpend; }` — the nullable trio reads as what it is.
- `frameworld.cpp`: `describeWorld` returns `struct Described { float mExposureBias; }` or writes the bias into
  `WorldReading`'s consumer; pick the first.

### 3.8 The stop writer's map tile is not the game's

**Now.** `writeMapTile` builds its own `OffscreenViewSpec` (512 px, eye at 50000, far 150000) while `LocalMap`
holds a `MapSegment::mView` per cell with the game's resolution and z-range.

**Redesign.** `OffscreenView* LocalMap::getView(int x, int y)` (or by `CellStore`), and `writeMapTile` calls
`keepCopy`/`redraw`/`getCopy` on that view. `localmap.hpp` is already an edited file; the addition is one accessor.
Delete `sMapTileSide`, `sMapEyeHeight`, `sMapFar`.

### 3.9 `getTerrainReach` encodes "unwidened" as `fov = 0`

**Now.** `RenderingManager::getTerrainReach` calls `mRenderer.getTerrainViewDistance(setting, 0.0f)` so that
`GlRenderer`'s `cos(0/2) = 1` yields the plain distance.

**Redesign.** `float Renderer::getGroundReach() const` — the radius ground is built to, which is what the map is a
map of: `GlRenderer` answers `Settings::camera().mViewingDistance` when paged, `RtxRenderer` answers `mReach`.
`getTerrainViewDistance` stays for the frustum question.

### 3.10 Two sources for the base wind speed

**Now.** `RenderingManager::update` reads `mSky->getBaseWindSpeed()` for the rasterizer's `windSpeed` uniform;
`setWeather` stores `mWorld.mBaseWindSpeed` from the same `WeatherResult`. `SkyManager::getSkyColor()` and
`SkyManager::mWindSpeed` are unreferenced.

**Redesign.** `update` reads `mWorld.mBaseWindSpeed`; delete `SkyManager::getBaseWindSpeed`, `getSkyColor`,
`mWindSpeed`. The rasterizer receives the same number by a shorter route.

### 3.11 `Actions::mDoll` + `mDollOut`

`struct DollAction { std::string mWho; std::filesystem::path mFile; }` as `std::optional<DollAction> mDoll`, so
"asked" is `has_value()` like every sibling.

### 3.12 `renderGui` → `presentWithGui`

Rename `presentWithGui` to `renderGui` and delete the forwarder; `renderFrame` calls `renderGui()`.

---

## Phase 4 — dead code, hygiene, comments

### 4.1 Delete

`RunRecord::getExitStatus`, `RunRecord::getPlaces`, `PhysicalDevice::getProfile`, `Image::getTexelBytes`,
`Rtx::noPatches`, `SkyManager::getSkyColor`, `SkyManager::mWindSpeed`. Move `Device::canDescribeFault` and
`TimeOfDaySettings::hasSetting` under the `internals` gate or delete the tests that read them.

Stale includes: `glrenderer.cpp` (`<fstream>`, `<osgDB/ReaderWriter>`, `<osgDB/Registry>`, `<osg/Image>`,
`sdlutil/imagetosurface.hpp`, `l10n/manager.hpp`), `tracedview.cpp` (`<variant>`).

### 4.2 Include order

`rtxrenderer.cpp` (`"setup.hpp"` into the local block), `sceneframe.hpp` (`"weatherresult.hpp"` last),
`renderingmanager.hpp` (join the local block, drop the blank line after `namespace MWRender {`), `glrenderer.cpp`
(one sorted local block, blank line before `getTerrainPlan`), `main.cpp` (`<osg/Notify>` into the library
block), `gloffscreenview.cpp` (`<osgUtil/…>` its own block), `rtxbackends/renderer.cpp` (`<SDL_video.h>` before
`<components/…>`). `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh` after.

### 4.3 Comment sweep

A file-by-file pass over every file the diff adds, with one rule per comment: keep it if it states an invariant,
a workaround and its cause, or a trade-off against the obvious alternative; delete it if it states what the code
used to be, a number that was measured once, or what the line under it does. Fix the four stale ones first
(`sceneframe.hpp` ×3, `renderingmanager.hpp` `mStormParticleDirection`). Do it after Phases 1–3 so the survivors
describe the code that exists.

Rough budget: `components/rtx` 8.2k comment lines, `rtxvulkan` 5.5k, `mwrender/rtx` 1.3k, `rtxbench` 0.9k,
`rtxtool` 0.7k. Expect to keep about a third.

---

## Order and verification

| Phase | Touches | Verify |
| --- | --- | --- |
| 0 | `terrain/objectpaging`, `sceneutil/optimizer`, `opencs/editor.cpp`, `CLAUDE.md` | `ninja openmw openmw-cs`; a rasterized `screenshot` of `seyda-neen-ship` before and after 0.1 |
| 1 | `surface/material`, `mwrender/rtx/stopwriter`, `sceneframe`, `renderingmanager`, `readworld`, `rtx/renderer.hpp`, `rtxbackends`, `vulkanrenderer`, `rtxtool/main`, `compositequeue`, `sceneuploader` | `components-tests --gtest_filter='Surface*:Rtx*Composite*:Rtx*Uploader*'`; `openmw-rtxtool check` |
| 2 | `worldmirror`, `rtxrenderer`, `nodekind` (new), `sceneextractor`, `materialresolver`, `meshreader`, `posecull`, `poseupdate`, `worlddescent`, `cellring`, `mirroridentity`, `sdlinputwrapper`, `distantlights`, `vulkanrenderer`/`tracedview` (2.6) | `components-tests --gtest_filter='Rtx*'`; `repeatable.sh --pairs=2` while iterating, `--pairs=10` at the end; `bench` warm then A/B interleaved for 2.2, 2.3, 2.6 — keep 2.6 only on a measured win |
| 3 | `framereport`, `viewhost`, `stopwriter`, `checks`, `session`, `rtx/renderer.hpp`, `sceneuploader`, `residency`, `sceneextractor`, `mirroridentity`, `preparedcell`/`preparedground`/`preparedtexture`/`heldcell`/`compositequeue`, `rtxvulkan/image` + every `transition` caller, `skybuilder`, `frameworld`, `localmap`, `renderer.hpp`, `benchrun` | `components-tests --gtest_filter='Rtx*'`; `openmw-rtxtool check`; `repeatable.sh --pairs=10` (the scene columns must not move) |
| 4 | everything the diff adds | `ninja`; `CI/check_clang_format.sh`; `components-tests --gtest_filter='Rtx*'` once |

Every phase ends with the format check and stops for the diff to be inspected; nothing is committed inside a
phase.

Costs the plan names and accepts: 1.3 adds an ESM header to a header the rasterizer includes; 2.5 and 3.8
each touch one already-edited upstream file; 2.6 changes when a doll appears (a frame later) and is the only
item with a real risk of not paying for itself, which is why it is measured before it is kept.
