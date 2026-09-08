# Data structure review — the whole diff against `upstream/master`

Scope: every file the fork adds or changes against `aaf769f511`. The review looks at data
structures, ownership and call graphs. It ignores test structure and the shapes tests use.

**Delete an item when you address it.** This file lists open findings only. Do not mark an item as
done, and do not keep a history section.

---

## Classes that carry more than one responsibility

- [ ] `Rtx::SceneAcceleration` has 48 members (`sceneacceleration.hpp:86`). It holds the bottom-level
  store, the compaction query and copy machinery (`mCompactable*`, `mCompacted*`, `mCompactionAt`,
  `mCompactionCopies`, `mBuiltSize`, `mCompactedSizes`), the top-level refit, the pose blocks, the
  instance row table and five instance counters.

  **The compaction does not come out on its own.** `prepareCompaction` reads `mBottomLevel`,
  `mBottomLevelRooms`, `mBottomLevelStorage`, `mBottomLevelAddresses` and `mBuiltSize`, and it
  replaces structures in place — it is part of what a bottom-level store does. So the split is
  `BottomLevelStore` taking about twenty members and six methods with it, leaving the top level, the
  refit, the poses and the indices behind.
- [ ] `Rtx::VulkanRenderer` has 55 members (`vulkanrenderer.hpp:61`) over a 1527-line `.cpp`. It
  holds the device, the frame ring, ten passes, the GUI, the view scenes, the upscaler and the
  presenter. The GUI half shares nothing with the trace half except the device.
- [ ] `Rtx::Renderer` has 33 pure virtuals over four subjects: the scene, the frame, the GUI surface
  and the readback (`renderer.hpp`). A caller that only draws the GUI takes the whole of it.

  Four interfaces fall out — `SceneSink`, `GuiSurface`, `FrameSource`, `Instrument` — and
  `VulkanRenderer` implements all four. **The cost is that they interleave**: the members sit in the
  header in ten contiguous runs by subject, so the split is ten block moves through the most heavily
  documented header in the fork, and its prose cross-references itself ("the three calls below take
  it too").
- [ ] `Rtx::SceneDesc` re-exports five tables one method at a time — 41 forwarding getters
  (`scenedesc.hpp:132`). Every new table field needs a new forwarder. A `SceneTables` value of five
  const references, returned by `getTables()`, would let the backend take the read side and stop it
  being able to mutate what it is handed. About 120 call sites change, all mechanically.
- [ ] `Rtx::FrameRecord` holds two parallel submissions in one struct: `mCommands`, `mFence`,
  `mPending` and `mGraveyard` for the world, and `mGuiCommands`, `mGuiFence`, `mGuiPending` and
  `mGuiGraveyard` for the GUI (`framering.hpp:42`). Name the submission once and hold two of them.

## The same fact is stored twice under two names

- [ ] `RtxTool::View` (`views.hpp:32`), `Rtx::Stop` (`benchrun.hpp:290`) and `RtxTool::Viewpoint`
  (`viewpoint.hpp:19`) are three records of one named place. `Rtx::Standing` (`benchrun.hpp:98`)
  and `Session::Note` (`session.hpp`) are two records of where the player stands, with different
  types for the facing and for the weather.
- [ ] `Stop::mCell` and `Stop::mStand.mCell` both hold the cell name, and `views.cpp:169` and `:171`
  write both from `view.mCell`. One struct stores the same string twice.
- [ ] `Rtx::SceneStats` (`renderer.hpp`) restates counters `SceneAcceleration` already keeps, and
  `readPlacedStats` and `readStats` copy them across. One `InstanceCounts` held by both would end
  it — which needs the split above.

## Parallel arrays and nested collections stand in for one row

- [ ] `Rtx::GpuBreakdown` keeps `mNames`, `mTimes` and `mSeen` as three parallel vectors, and
  `mTimes` is `std::vector<std::vector<double>>` — one allocation per zone
  (`frametimes.hpp:151-159`). `add()` runs on every measured frame.
- [ ] `Rtx::FrameSamples` holds four `std::vector<double>` named `mFrame`, `mWait`, `mWalk` and
  `mPlace` (`frametimes.hpp:48`), and `BenchPlace` holds four `FrameTimes` with the same four names
  (`benchrecord.hpp:88-91`). Index one array by a named enum.
- [ ] `Rtx::SceneTextures` keeps `mSpriteLights` with `mSpriteLightCount`
  (`texturebuilder.hpp:151-152`) and `mChains` with `mChainCount` (`:161-162`) — the same
  pool-and-live-count pattern written twice. Name the pool.
- [ ] `Terrain::ObjectStorage::collectReferences` and `collectLights` write into
  `std::map<ESM::RefNum, PagedCellRef>&` (`objectstorage.hpp:123`, `:136`). A node-based map
  allocates once for each reference on a loading path. `Rtx::DistantLights::mCells` is a
  `std::map<osg::Vec2i, osg::ref_ptr<osg::Group>>` beside it (`distantlights.hpp:114`).
- [ ] `MaterialTable::mArrivedLayers` and `mArrivedMasks` are raw `std::vector<Run>`
  (`materialtable.hpp:111-112`), where every other change list in the scene is a `SlotSet`. Runs are
  not slots, so `SlotSet` cannot hold them — but the pair can be one named `ArrivedRuns`.

## The terrain's object store asks one question through two virtuals and three callbacks

- [ ] `collectReferences` and `collectLights` differ only by the predicate they pass —
  `Terrain::pagedType` against `Terrain::litType` (`objectstorage.cpp:99`, `:116`). One virtual that
  takes the kind is enough.
- [ ] `Terrain::collectPagedRefs` takes three `std::function` parameters — `cellAt`, `typeOf` and
  `wanted` (`objectstorage.hpp:101`). Two of them close over one `ESMStore`. Take the store behind
  one small interface.

## The renderer seam still carries some of each renderer's private vocabulary

- [ ] `getMaxTextureUnits` is OpenGL's, and the ray tracer answers it with
  `Surface::sAssumedTextureUnits`. `installStatsOverlay` and `reportStats` are OpenGL's too, and the
  ray tracer answers both with a no-op. `suspendDraw` and `resumeDraw` are a third pair.
  `describeShaderLimits()` and one `getStatsSink()` would carry the first two groups.
- [ ] `MWRender::Stage` holds the camera, the frame stamp, the event queue, the statistics and the
  scene root (`stage.hpp:92-96`). `RtxRenderer` holds the same five as `osg::ref_ptr` members
  (`rtxrenderer.hpp`) and adds `getSceneRoot()` beside `Stage::getSceneRoot()`. One of the two is
  the home. Choose it.

## Ownership is stated in more than one way

- [ ] `Rtx::MeshTable` takes `DeformerTable&` (`meshtable.hpp:46`) and `MaterialTable` takes
  `TextureTable&` (`materialtable.hpp:30`). `SceneDesc` declares the four members in the order those
  references need (`scenedesc.hpp:593-617`), so a member moved breaks the initialization. State the
  dependency where a reorder cannot break it.
