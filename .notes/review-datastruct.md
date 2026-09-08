# Data structure review — the whole diff against `upstream/master`

Scope: every file the fork adds or changes against `aaf769f511`. The review looks at data
structures, ownership and call graphs. It ignores test structure and the shapes tests use.

**Delete an item when you address it.** This file lists open findings only. Do not mark an item as
done, and do not keep a history section.

---

## The game-side renderer is a god object, and the harness reaches back into it

`MWRender::RtxRenderer` owns the window, the camera, the frame clock, the scene mirror, the GUI, the
offscreen views, the capture and the session. Every other part of the harness takes a reference to it
and asks it for what it needs. One class holds eight responsibilities, and the call graph runs both
ways through it.

- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp:159` opens an `/*internal:*/` block of twelve
  accessors. `Session`, `StopWriter`, `checkHolds` and `TracedView` read them. Hand each caller the
  few facts it needs, then remove the accessors.
- [ ] `Session::frame` (`session.hpp:107`), `StopWriter::write` (`stopwriter.hpp:44`) and
  `checkHolds` (`checks.hpp:15`) each take `RtxRenderer& owner`. `StopWriter` makes thirteen
  `mOwner.` calls. Name a record of what a stop reads — the backend, the mirror, the two walk
  statistics, the unreadable count, the frame number — and pass that record.
- [ ] `apps/openmw/mwrender/rtx/session.cpp:49` holds a file-static `sInstalled`. `runHosted` writes
  a `SessionRequest` and a raw `SessionResult*` into it, and the `RtxRenderer` constructor takes them
  out (`session.cpp:103`, `:108`). A global mailbox carries the ownership of a result the tool still
  holds. Pass the request through `RendererSpec`.
- [ ] `RtxRenderer::traceWorld` (`rtxrenderer.cpp:656`) holds the whole frame in one function: the
  wait, the hand-over, the log, the deferred views, the camera, the world reading, the submit, the
  timing and the report. Separate the frame's orchestration from the renderer that owns the device.
- [ ] `Rtx::SceneExtractor` makes `isFirstPerson` (`sceneextractor.hpp:161`), `addLight` (`:267`),
  `addDrawable` (`:282`) and `animate` (`:296`) public. `MirrorTraversal` in the same `.cpp` is the
  only caller (`sceneextractor.cpp:327`, `:345`, `:348`, `:551`). Make the walk a friend, or move
  the state it needs into it.

## `std::uint32_t` names four different kinds of slot

Nothing in the type says which kind. A scene slot, a frame slot, a GUI texture id and a texture table
index all travel as `std::uint32_t`, and three of them use the member name `mSlot`.

- [ ] `VulkanRenderer::sceneAt(std::uint32_t slot)` takes a **scene** slot
  (`vulkanrenderer.hpp:167`). `VisibilityInputs::mSlot` (`visibilitypass.hpp:39`) and
  `Placing::mSlot` (`placing.hpp:24`) take a **frame** slot. `vulkanrenderer.cpp:1303` writes
  `options.mScene == sWorld ? mWorldSlot : 0`, which mixes both meanings in one expression. Give
  each kind its own type.
- [ ] `TextureData::mSlot` (`texturedata.hpp:97`) is a texture table index.
  `MyGUIRtx::Texture::mSlot` (`myguirtx/texture.hpp:100`) is a GUI texture id. Neither is a scene
  slot or a frame slot, and `GuiTextures::add`, `lend`, `send`, `write` and `drop` all name their
  parameter `slot` (`guitextures.hpp`).
- [ ] `Rtx::sWorld` is `~0u` in the same value space as the slots `addViewScene()` hands out
  (`renderer.hpp:32`, `:820`). A caller that forgets the sentinel indexes `mViewScenes` out of
  range. Make the world a named case of a slot type.
- [ ] `VulkanRenderer` holds the world scene as the member `mWorld` (`vulkanrenderer.hpp:361`) and
  the view scenes as `std::vector<std::unique_ptr<ViewScene>>` (`:403`). One scene lives in two
  shapes, and `sceneAt` branches on the sentinel to choose. Put the world in the vector.

## One frame's world is described four times

The same weather, sky, water and camera pass through four structs on the way to the shader. Three of
the four copy fields across without changing them.

- [ ] `MWRender::WorldState` (`sceneframe.hpp:79`) → `Rtx::WorldReading` (`frameworld.hpp:176`) →
  `Rtx::FrameWorld` (`frameworld.hpp:100`) → `Shaders::VisibilityConstants`. `WorldReading` and
  `FrameWorld` share `mWaterLevel`, `mSeconds`, `mRainOnWater`, `mMoons` and the sky, and
  `describeWorld` copies them straight over. `applyWorld` then copies `FrameWorld` field by field
  into the constants. Each function has one caller. Collapse the two intermediates into one.
- [ ] `RenderingManager` keeps `WorldState mWorld` (`renderingmanager.hpp:402`) **and** the loose
  members `mAmbientColor`, `mNearClip`, `mViewDistance`, `mFieldOfView`, `mFieldOfViewOverride`,
  `mFieldOfViewOverridden`, `mNight`, `mCloudSpeed` and `mStormParticleDirection`. `describeWorld`
  copies each into a `WorldState` every frame (`renderingmanager.cpp:866`). The header's own comment
  calls a second set of mirroring members the failure mode. Finish the migration.
- [ ] `WorldState` mixes three subjects: the sky and the weather, the water, and the camera
  (`mNearClip`, `mViewDistance`, `mProjectionMatrix`, `mFieldOfView`). A camera is not a fact about
  the world. Separate it.

## The renderer seam carries each renderer's private vocabulary

`MWRender::Renderer` has about 36 members. Fourteen of them mean something to one renderer only. The
other renderer answers with a no-op or with a base-class default.

- [ ] `getMaxTextureUnits` (`rtxrenderer.hpp:80`), `suspendDraw` (`:129`), `resumeDraw` (`:130`),
  `getCompileOperation` (`:134`), `setCompileOperation` (`:135`), `reloadChangedShaders` (`:146`),
  `installStatsOverlay` (`:156`), `reportStats` and `getPostProcessor` are OpenGL's, and
  `RtxRenderer` answers all nine with a no-op. `tickSchedule`, `beginFrame`,
  `notifyWorldSpaceChanged`, `setUpscale` and `getUpscale` are the ray tracer's, and the base class
  defaults them for `GlRenderer`. Move each group behind a question the caller can ask, or into a
  smaller interface the caller asks for.
- [ ] `MWGui::LoadingScreen` calls `mRenderer.getCompileOperation()` three times
  (`loadingscreen.cpp:138`, `:179`, `:299`). The call always returns null under the ray tracer. Ask
  the renderer what the loading screen needs, not for an OpenGL object.
- [ ] `MWRender::Stage` holds the camera, the frame stamp, the event queue, the statistics and the
  scene root (`stage.hpp:92-96`). `RtxRenderer` holds the same five as `osg::ref_ptr` members
  (`rtxrenderer.hpp:317-322`) and adds `getSceneRoot()` (`:180`) beside `Stage::getSceneRoot()`.
  One of the two is the home. Choose it.

## One run's options are spelled in five places

The same set of switches appears as a settings category, as a command-line record, as two option
bags in the core, and as loose members on the renderer. The settings singleton carries them from the
tool to the renderer.

- [ ] `Settings::RTXCategory` (`categories/rtx.hpp:19`), `RtxTool::FrameRequest`
  (`framerequest.hpp:39`), `Rtx::RendererOptions` (`renderer.hpp:88`), `Rtx::FrameOptions`
  (`renderer.hpp:425`) and the loose `RtxRenderer` members `mDelight`, `mShowAlbedo`, `mFilter`,
  `mJitter` and `mExposure` (`rtxrenderer.hpp:372-379`) hold overlapping copies of one set.
- [ ] `apps/rtxtool/main.cpp:353-367` writes `FrameRequest` into the global settings, and
  `rtxrenderer.cpp:142-241` reads it back out. The settings singleton is a message channel between
  two objects that one record could join.
- [ ] `main.cpp:363` writes `frame.mExposure.value_or(0.0f)` and `rtxrenderer.cpp:241` reads
  `exposure > 0.0f`. The `std::optional` shape is lost and then rebuilt through a sentinel.

## The same fact is stored twice under two names

- [ ] `Sky::MoonMoment` (`moonmodel.hpp:27`) and `MWRender::MoonState` (`weatherresult.hpp:73`) are
  the same six fields under different names: `mAlongArc` against `mRotationFromHorizon`,
  `mAxisOffset` against `mRotationFromNorth`, `mAlpha` against `mMoonAlpha`. The enums
  `Sky::MoonPhase` and `MoonState::Phase` are converted by a cast that three `static_assert`s guard
  (`weather.cpp:265-275`). Keep one moment type and one phase enum.
- [ ] `Rtx::placeMoon` takes `int phase` (`moonbuilder.hpp:123`) and `readworld.cpp:104` casts an
  enum into it. Take the enum.
- [ ] `Weather::Precipitation` copies every field of `Downpour` into its own members —
  `mRainEffect`, `mRainSpeed`, `mRainDiameter`, `mRainMinHeight`, `mRainMaxHeight`,
  `mRainEntranceSpeed`, `mRainMaxRaindrops`, `mPrecipitationAlpha`, `mWindSpeed`, `mBaseWindSpeed`,
  `mIsStorm` (`precipitation.hpp:193-208`). Hold one `Downpour` member instead.
- [ ] `RtxTool::View` (`views.hpp:32`), `Rtx::Stop` (`benchrun.hpp:290`) and `RtxTool::Viewpoint`
  (`viewpoint.hpp:19`) are three records of one named place. `Rtx::Standing` (`benchrun.hpp:98`)
  and `Session::Note` (`session.hpp:211`) are two records of where the player stands, with different
  types for the facing and for the weather.
- [ ] `Stop::mCell` and `Stop::mStand.mCell` both hold the cell name, and `views.cpp:169` and `:171`
  write both from `view.mCell`. One struct stores the same string twice.
- [ ] `Rtx::SceneStats` (`renderer.hpp:240`) restates counters `SceneAcceleration` already keeps,
  and `readPlacedStats` and `readStats` copy them across (`vulkanrenderer.hpp:191`, `:194`). Report
  the source, not a copy of it.
- [ ] `Rtx::OffscreenTrace` stores the projection as `mPerspective`, `mFieldOfView`, `mBoxWidth`,
  `mBoxHeight`, `mNear` and `mFar` (`offscreentrace.hpp:218-223`). `OffscreenViewSpec` already
  carries `std::variant<Perspective, Orthographic>` (`offscreenview.hpp:72`), and `TracedView`
  unpacks it. `OffscreenTrace` also unpacks `SceneUtil::FlatLight` (`offscreenview.hpp:81`) into
  `mSunPosition`, `mSunIrradiance` and `mAmbient` (`offscreentrace.hpp:227-229`). Keep both whole.

## Five types hold the same in-memory image

Each one holds a byte buffer, a `std::vector<MipLevel>`, a width and a height, and describes itself
as a `TextureData`.

- [ ] `Rtx::MipChain` (`mipchain.hpp:31`), `Rtx::SpriteLightMap` (`spritelight.hpp:43`),
  `Rtx::AlphaImage` (`alphaimage.hpp:33`), `Rtx::TerrainComposite` (`terraincomposite.hpp:162`) and
  `Rtx::ContactSheet` (`contactsheet.hpp:13`). Name one owned-image type, and let each of the five
  become a builder that fills it.

## The channel set is enumerated three times

- [ ] `Shaders::CHANNEL_*` names fourteen channels (`shaders/gbuffer.h:91`). `GBuffer` names the
  same fourteen as separate `Image` members, with fourteen accessors and an `everyChannel()` that
  rebuilds the array (`gbuffer.hpp:34`). `Rtx::Channel` names nine of them again
  (`renderer.hpp:340`), and `VulkanRenderer::readChannel` maps one onto the other with a switch.
  Hold `std::array<Image, sChannels>` indexed by one enum.

## Change tracking has four shapes

Every table has to say which rows changed. No two tables say it the same way.

- [ ] `MeshTable` and `TextureTable` use `SlotChanges`. `MaterialTable::mWritten`
  (`materialtable.hpp:108`) and `DeformerTable::mArrivedRigs` use a bare `SlotSet`.
  `MaterialTable::mArrivedLayers` and `mArrivedMasks` use raw `std::vector<Run>` with no
  de-duplication (`materialtable.hpp:111-112`). `PlacementTable` uses two plain
  `std::vector<Index>`. Choose one mechanism.
- [ ] `SlotTable` tracks its debt with `RowDebt`, which can owe everything
  (`slottable.hpp:209`). `SlotBlocks` tracks the same debt with a bare `SlotSet` and cannot
  (`slottable.hpp:327`). Two double-buffered tables in one header, two mechanisms.

## Classes that carry more than one responsibility

- [ ] `Rtx::SceneAcceleration` has 48 members (`sceneacceleration.hpp:86`). It holds the
  bottom-level store, the compaction query and copy machinery (`mCompactable*`, `mCompacted*`,
  `mCompactionAt`, `mCompactionCopies`, `mBuiltSize`, `mCompactedSizes`), the top-level refit, the
  pose blocks, the instance row table and five instance counters. Separate the compaction at least.
- [ ] `Rtx::VulkanRenderer` has 55 members (`vulkanrenderer.hpp:61`) over a 1527-line `.cpp`. It
  holds the device, the frame ring, ten passes, the GUI, the view scenes, the upscaler and the
  presenter. The GUI half shares nothing with the trace half except the device.
- [ ] `Rtx::Renderer` has 33 pure virtuals over four subjects: the scene, the frame, the GUI surface
  and the readback (`renderer.hpp:557`). A caller that only draws the GUI takes the whole of it.
- [ ] `Rtx::SceneDesc` re-exports five tables one method at a time — 41 forwarding getters
  (`scenedesc.hpp:132`). Every new table field needs a new forwarder. Hand out the tables.
- [ ] `Rtx::FrameSlot` holds two parallel submissions in one struct: `mCommands`, `mFence`,
  `mPending` and `mGraveyard` for the world, and `mGuiCommands`, `mGuiFence`, `mGuiPending` and
  `mGuiGraveyard` for the GUI (`framering.hpp:42`). Name the submission once and hold two of them.

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
- [ ] `Rtx::TextureTable` sizes `mPaths` **and** `mBaked` to the slot count
  (`texturetable.hpp:112-113`), so every slot pays for a `VFS::Path::Normalized` and a
  `std::string`, while `Slot::mKind` says that only one of the two is live.
- [ ] `Terrain::ObjectStorage::collectReferences` and `collectLights` write into
  `std::map<ESM::RefNum, PagedCellRef>&` (`objectstorage.hpp:123`, `:136`). A node-based map
  allocates once for each reference on a loading path. `Rtx::DistantLights::mCells` is a
  `std::map<osg::Vec2i, osg::ref_ptr<osg::Group>>` beside it (`distantlights.hpp:114`).

## The terrain's object store asks one question through two virtuals and three callbacks

- [ ] `collectReferences` and `collectLights` differ only by the predicate they pass —
  `Terrain::pagedType` against `Terrain::litType` (`objectstorage.cpp:99`, `:116`). One virtual that
  takes the kind is enough.
- [ ] `Terrain::collectPagedRefs` takes three `std::function` parameters — `cellAt`, `typeOf` and
  `wanted` (`objectstorage.hpp:101`). Two of them close over one `ESMStore`. Take the store behind
  one small interface.

## Ownership is stated in more than one way

- [ ] `Rtx::Buffer` owns its handle with `Owned<VkBuffer, vkDestroyBuffer>` (`buffer.hpp:22`).
  `Rtx::Image` holds a raw `VkImage` and two raw `VkImageView`, and writes its own destructor
  (`image.hpp:20`). `Owned<>` exists for this.
- [ ] `Rtx::FrameRing` holds `const bool& mCountHits` and `const bool& mCountCrossings`
  (`framering.hpp:173-174`), which are references into `VulkanRenderer`'s own members. The ring's
  correctness depends on the address of two booleans. Pass them at the call that reads them.
- [ ] `Rtx::MeshTable` takes `DeformerTable&` (`meshtable.hpp:46`) and `MaterialTable` takes
  `TextureTable&` (`materialtable.hpp:30`). `SceneDesc` declares the four members in the order those
  references need (`scenedesc.hpp:593-617`), so a member moved breaks the initialization. State the
  dependency where a reorder cannot break it.

## Small items

- [ ] `Rtx::textureAt` (`texturedata.hpp:125`) has no production caller, and it is a linear scan
  over a span. Remove it.
- [ ] `Rtx::Renderer::GuiRegion` is nested inside the interface (`renderer.hpp:749`), while every
  other GUI type — `GuiVertex`, `GuiBatch`, `GuiBlend`, `GuiTraceOptions` — sits beside it. Move it
  out.
- [ ] `Rtx::SlotTable::mName` is a `std::string` for each table (`slottable.hpp:205`). `open` sets
  it once, and only a debug name reads it. A `std::string_view` onto a literal is enough.
- [ ] `Rtx::Traversals` is a class with one `unsigned int` and one method (`traversals.hpp`).
  `SceneExtractor` (`sceneextractor.hpp:326-327`) and `OffscreenTrace` (`offscreentrace.hpp:195-196`)
  each hold both an owned counter and a reference to a borrowed one. One counter, two homes, twice.
