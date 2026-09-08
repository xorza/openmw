# Design proposal and implementation plan

This answers the findings still open in `.notes/review-datastruct.md`. Read that file first.

The plan keeps the fork's own rules. It adds no dependency. Each step names its verification.

**Delete a step when you land it.** This file lists what is left.

---

## The decisions the landed half settled

These three settled every step already landed, and they settle what is left.

**1. A fact has one home, and everything else borrows it.** A fact copied into a second struct
whenever it crosses a boundary is a field added to one host and forgotten in the other. Keep the one
type that already holds the fact, and hand out a reference or a view of it.

**2. A caller takes what it reads, not the object that holds it.** Name the three or four things a
caller actually reads, and the call graph stops running both ways.

**3. A number that means a place gets a type.** `Rtx::SceneSlot`, `FrameSlot` and `GuiSlot` are that,
and they ended a whole class of mistake.

---

## Part 1 — The types to add

### 1.1 `Rtx::SceneTables` — the read side of a scene, in one value

```cpp
namespace Rtx
{
    /// What a reader of a scene is handed. Borrowed, and valid for one call.
    struct SceneTables
    {
        const MeshTable& mMeshes;
        const DeformerTable& mDeformers;
        const PlacementTable& mPlacements;
        const MaterialTable& mMaterials;
        const TextureTable& mTextures;
        std::span<const Light> mLights;
        std::span<const Sprite> mSprites;
        std::span<const SpriteEmitter> mEmitters;
        std::uint64_t mStructureRevision = 0;
    };
}
```

`SceneDesc::getTables() const` returns it. `SceneDesc` then keeps only its mutating vocabulary —
what the extractor says — plus the few derived answers that need every table at once
(`getTriangleCount`, `getBounds`, `getContentBoundsWithin`, `getGeometryBytes`).

`Rtx::Renderer::setScene`, `extendScene` and `placeScene` take `const SceneTables&`, so the backend
cannot mutate the scene it is handed. The 41 forwarding getters go, and a call site changes from
`scene.getPositions()` to `tables.mMeshes.getPositions()`.

### 1.2 `Rtx::BottomLevelStore` — the acceleration structures, with their compaction

`SceneAcceleration` splits in two. The store takes the bottom-level handles, their rooms and
addresses, the build batch and its sizes, the arrival list, the update flags, the micromap flags,
and the whole compaction — the query pool, the built and compacted sizes, the copy list and the
cursor. What is left keeps the top level, the refit, the pose blocks and the index blocks.

The five instance counters become one `InstanceCounts`, which `Rtx::SceneStats` holds by value
rather than restating field by field.

### 1.3 Four interfaces, and `Rtx::Renderer` as their union

| Interface | Members | Callers |
|---|---|---|
| `Rtx::SceneSink` | `setScene`, `extendScene`, `placeScene`, `getTextureCount`, `dropTextures`, `addViewScene`, `dropViewScene`, `getSceneStats` | `SceneUploader`, `WorldMirror`, `OffscreenTrace` |
| `Rtx::GuiSurface` | `addGuiTexture`, `writeGuiTexture`, `lendGuiTexture`, `sendGuiTexture`, `dropGuiTexture`, `readGuiTexture`, `drawGui`, `traceGuiTexture`, `getExtents` | `MyGUIRtx`, `TracedView`, `OffscreenTrace` |
| `Rtx::FrameSource` | `renderFrame`, `finishFrame`, `presentFrame`, `resetHistory`, `resize`, `setUpscale`, `getUpscale`, `setVerticalSync` | `RtxRenderer` |
| `Rtx::Instrument` | `describeDevice`, `isValidating`, `readPixels`, `readChannel`, `readFrameImage`, `getMemoryReport`, `takeValidationErrors` | `StopWriter`, `checks`, `rtxtool` |

`getExtents` sits in `GuiSurface` alone: a GUI texture is sized against the frame's extents, and
declaring it in two bases would make it ambiguous in `Renderer`.

### 1.4 Smaller shapes

- `Rtx::FrameRecord` holds two `Submission { VkCommandBuffer mCommands; VkFence mFence; bool
  mPending; Graveyard mGraveyard; }` — one for the world, one for the GUI.
- `Rtx::GpuBreakdown` keeps one `std::vector<ZoneRow>` with a flat sample buffer plus a
  `std::vector<Run>` of spans, not `std::vector<std::vector<double>>`.
- `FrameSamples` and `BenchPlace` index one `std::array<_, sTimingCount>` by an
  `enum class Timing { Frame, Wait, Walk, Place }`.
- `Rtx::Pool<T>` replaces `SceneTextures`'s two vector-plus-count pairs.
- `MaterialTable`'s two run lists become one `ArrivedRuns`.
- `Terrain::ObjectStorage` collapses `collectReferences` and `collectLights` into
  `collect(RefKind kind, ...)`, and writes into `std::vector<PagedCellRef>&` sorted by `RefNum`
  rather than a `std::map`. `collectPagedRefs` takes one `CellSource` interface instead of three
  `std::function`s.
- `MWRender::Renderer` gains `describeShaderLimits()` in place of `getMaxTextureUnits`, and one
  `getStatsSink()` in place of `installStatsOverlay` and `reportStats`.
- `MWRender::Stage` becomes the one home for the camera, the frame stamp, the event queue, the
  statistics and the scene root. `RtxRenderer` creates them, hands them to `Stage::adopt`, and reads
  them back through `mStage`; its five `osg::ref_ptr` members and `getSceneRoot()` go.
- `SceneDesc` states its members' dependency where a reorder cannot break it.
- `RtxTool::View`, `Rtx::Stop` and `RtxTool::Viewpoint` collapse toward one named place, and
  `Rtx::Standing` and `Session::Note` toward one standing. `Stop::mCell` goes; `Stop::mStand.mCell`
  is the one.

---

## Part 2 — Implementation plan

Build with `apps/rtxtool/debug.sh`. Run the tests as gtest binaries with `--gtest_filter`. Format
with `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh`. Where a step can reach a frame, run
`apps/rtxtool/repeatable.sh` and check the scene columns against `.notes/repeatable.txt`.

### A. The read side of a scene

**A1. `Rtx::SceneTables`, and the backend takes it.**
Add `SceneTables` and `SceneDesc::getTables()`. Change `Rtx::Renderer`'s three scene calls to take
`const SceneTables&`. Change `SceneAcceleration`, `SceneBuffers`, `SceneMicromaps`, `SkinTables`,
`InstanceRecord` and `SceneDigest` to take it.
*Verify:* `--gtest_filter='RtxSceneDescTest.*:RtxSceneUploaderTest.*:RtxSceneDigestTest.*:RtxInstanceRecordTest.*:RtxSceneExtractorTest.*'`,
then `repeatable.sh`. The scene columns must be identical.

**A2. Delete the 41 forwarders.**
Rewrite the remaining call sites — `StopWriter`, `checks`, `WorldMirror`, `SceneUploader`,
`TextureBuilder`, `CompositeQueue`. Keep the four derived answers on `SceneDesc`.
*Verify:* the same as A1.

### B. The acceleration structures

**B1. Split `BottomLevelStore` out of `SceneAcceleration`.**
Take the compaction with it. Introduce `InstanceCounts`, and hold it in `SceneStats`.
*Verify:* `--gtest_filter='RtxSceneDescTest.*:RtxStructureStorageTest.*:RtxMicromapBakeTest.*:RtxMicromapCurveTest.*'`,
then `repeatable.sh` and one `bench --views=one-cell-walk` against the reading in `.notes/bench.txt`.

### C. The four interfaces

**C1. Split `Rtx::Renderer`.**
`Renderer` inherits all four and gains nothing of its own. `VulkanRenderer` is unchanged apart from
the base list. **Move each member's whole documented block**, and fix the prose that
cross-references its neighbours.
*Verify:* it compiles, `--gtest_filter='RtxSceneUploaderTest.*:RtxOffscreenTraceTest.*:RtxGuiDrawTest.*'`,
then `repeatable.sh`.

**C2. Narrow each caller.**
`SceneUploader` and `WorldMirror` take `SceneSink&`. `MyGUIRtx` takes `GuiSurface&`.
`OffscreenTrace` takes both. `StopWriter` and `checks` take `Instrument&`.
*Verify:* the same as C1.

### D. The smaller shapes

**D1. `FrameRecord`'s two submissions, `GpuBreakdown` flattened, `FrameSamples` and `BenchPlace` on
one array, `Rtx::Pool<T>`, `MaterialTable`'s `ArrivedRuns`.**
*Verify:* `--gtest_filter='RtxFrameRingTest.*:RtxGpuBreakdownTest.*:RtxFrameTimesTest.*:RtxTextureBuilderTest.*:RtxSceneDescTest.*'`,
then `repeatable.sh`.

**D2. `MWRender::Stage` becomes the one home.**
Remove `RtxRenderer`'s five `osg::ref_ptr` members and `getSceneRoot()`.
*Verify:* `openmw-rtxtool view --frames=60` and one played frame — the intersection visitor has to
still find a door.

**D3. `MWRender::Renderer` sheds the rest of its OpenGL vocabulary.**
`describeShaderLimits()` replaces `getMaxTextureUnits`. `getStatsSink()` replaces the two stats
calls. `suspendDraw` and `resumeDraw` go behind whatever asks for them.
*Verify:* a rasterized load with a loading screen, and a traced load.

**D4. The named place and the standing.**
Collapse `View`, `Stop` and `Viewpoint`; collapse `Standing` and `Note`; delete `Stop::mCell`.
*Verify:* `openmw-rtxtool check` over the whole suite, and `view` printing its block on close.

### E. The terrain's object store

**E1. One `collect`, one `CellSource`, a flat output.**
*Verify:* `--gtest_filter='*ObjectPaging*:*CellGrid*:RtxDistantLightsTest.*'`, then a walk that
crosses a cell boundary in both renderers.

---

## Part 3 — What this plan does not do, and why

- **It does not split `VulkanRenderer`.** The class has 55 members, but the passes it owns are
  already separate types and the frame path reads most of them. The four interfaces of §1.3 give
  every caller a smaller view without moving anything.
- **It does not give `Rtx::TextureTable` one name per slot.** A slot pays for a
  `VFS::Path::Normalized` and a `std::string` while `Slot::mKind` says only one is live, which over
  the 725 textures of a cell is about 46 KB — against a change that reshapes what `ScenePartDigests`
  hashes and touches every reader of the two spans.
- **It does not unify the change tracking.** `SlotChanges` is a pair of `SlotSet`s, and
  `MaterialTable::mWritten` and `DeformerTable::mArrivedRigs` are single ones — which is already one
  mechanism in the two shapes two different jobs need. `PlacementTable`'s two plain vectors are a
  measured choice its own header states, and `SlotBlocks` cannot take `RowDebt`: "owes everything"
  has no meaning for a table whose rows a dispatch fills, and `sync` could not serve it.
- **It does not finish `RenderingManager::mWorld`.** The camera four were the only mirror, and they
  are now `MWRender::EyeState`. `mAmbientColor`, `mNearClip`, `mViewDistance`, `mFieldOfView`,
  `mFieldOfViewOverride`, `mFieldOfViewOverridden`, `mNight`, `mCloudSpeed` and
  `mStormParticleDirection` are working state that `describeWorld` *reduces* — a fade subtracted, an
  override chosen, a roll advanced — rather than a second copy of what it reports.
- **It does not make `ContactSheet` an `OwnedTexture`.** It is a single-level RGBA buffer with a
  grid count, written straight to a PNG; it has no mip chain and describes no `TextureData`.
- **It does not change what a frame draws.** Every step is a shape change, gated on the scene
  columns of `repeatable.sh` being identical.

## Part 4 — Order of value

1. **A1 and A2** — the scene facade. The largest mechanical diff, and the smallest risk.
2. **B1** — the acceleration split. The largest single class left.
3. **C1 and C2** — the four interfaces.
4. **E1** — the terrain store, which is the last `std::function` chain and the last node-based map
   on a loading path.
5. **D1 to D4** — the smaller shapes.
