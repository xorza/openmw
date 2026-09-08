# Design proposal and implementation plan

This answers the findings in `.notes/review-datastruct.md`. Read that file first. Each step below
names the findings it closes.

The plan keeps the fork's own rules. It adds no dependency. It touches an upstream file only where a
step says so, and each of those steps names the file and waits for a go-ahead.

---

## Three decisions that settle most of the findings

**1. A fact has one home, and everything else borrows it.** Today a fact is copied into a second
struct whenever it crosses a boundary — the world into four structs, the moon into two, the options
into five, the channel set into three. The fix is always the same shape: keep the one type that
already holds the fact, and hand out a reference or a view of it.

**2. A caller takes what it reads, not the object that holds it.** `RtxRenderer&`,
`const SceneDesc&` and `Rtx::Renderer&` travel down the stack whole. Each caller reads three or four
things from them. Name those three or four things, and the call graph stops running both ways.

**3. A number that means a place gets a type.** Four kinds of slot travel as `std::uint32_t`. One
small header ends that class of mistake for the whole tree.

---

## Part 1 — The types to add

### 1.1 `components/rtx/slot.hpp` — four kinds of slot stop being one

```cpp
namespace Rtx
{
    /// Which scene a call is about: the world, or one a view asked for.
    ///
    /// **The world is a case and not a sentinel.** `sWorld` was `~0u` in the same value space as
    /// the indices `addViewScene` hands out, so a caller that forgot it indexed the view table out
    /// of range.
    class SceneSlot
    {
    public:
        static constexpr SceneSlot world() { return SceneSlot{ sWorldIndex }; }
        static constexpr SceneSlot view(std::uint32_t index);   // asserts index != sWorldIndex
        constexpr bool isWorld() const { return mIndex == sWorldIndex; }
        constexpr std::uint32_t getViewIndex() const;           // asserts !isWorld()
        constexpr bool operator==(const SceneSlot&) const = default;
    private:
        static constexpr std::uint32_t sWorldIndex = ~0u;
        std::uint32_t mIndex = sWorldIndex;
    };

    /// Which copy of a double-buffered table a frame writes. Always below `sFrameSlots`.
    class FrameSlot
    {
    public:
        explicit constexpr FrameSlot(std::uint32_t index);      // asserts index < sFrameSlots
        constexpr std::uint32_t get() const { return mIndex; }
        constexpr FrameSlot next() const;
        constexpr bool operator==(const FrameSlot&) const = default;
    private:
        std::uint32_t mIndex = 0;
    };

    /// One texture the GUI draws from, or nothing.
    class GuiSlot { /* the same shape, with a `none()` case */ };
}
```

`Index` stays what it is — a row in a scene table. It is already named, and it is not a slot.

Closes: every item under *`std::uint32_t` names four different kinds of slot*.

### 1.2 `Rtx::Renderer` becomes four interfaces

`VulkanRenderer` implements all four. Each caller takes the one it reads.

| Interface | Members | Callers |
|---|---|---|
| `Rtx::SceneSink` | `setScene`, `extendScene`, `placeScene`, `getTextureCount`, `dropTextures`, `addViewScene`, `dropViewScene`, `getSceneStats` | `SceneUploader`, `WorldMirror`, `OffscreenTrace` |
| `Rtx::GuiSurface` | `addGuiTexture`, `writeGuiTexture`, `lendGuiTexture`, `sendGuiTexture`, `dropGuiTexture`, `readGuiTexture`, `drawGui`, `traceGuiTexture`, `getExtents` | `MyGUIRtx::RenderManager`, `MyGUIRtx::Texture`, `TracedView`, `OffscreenTrace` |
| `Rtx::FrameSource` | `renderFrame`, `finishFrame`, `presentFrame`, `resetHistory`, `resize`, `setUpscale`, `getUpscale`, `setVerticalSync`, `getExtents` | `RtxRenderer` |
| `Rtx::Instrument` | `describeDevice`, `isValidating`, `readPixels`, `readChannel`, `readComposite`, `readAccumulated`, `getMemoryReport`, `takeValidationErrors` | `StopWriter`, `checks`, `rtxtool` |

`Rtx::Renderer` stays as the union the factory returns, and it inherits the four. Nothing else
changes shape.

The scene-facing calls take `const SceneTables&` (§1.3) rather than `const SceneDesc&`, so the
backend cannot mutate the scene it is handed.

Closes: *`Rtx::Renderer` has 33 pure virtuals over four subjects*.

### 1.3 `Rtx::SceneTables` — the read side of a scene, in one value

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

The 41 forwarding getters go. A call site changes from `scene.getPositions()` to
`tables.mMeshes.getPositions()`. About 120 sites change, all mechanically.

Closes: *`Rtx::SceneDesc` re-exports five tables one method at a time*.

### 1.4 `MWRender::TracedRun` — what a stop reads

```cpp
namespace MWRender
{
    /// What a measured stop is allowed to look at. Built once per stop by `RtxRenderer`.
    struct TracedRun
    {
        Rtx::Instrument& mInstrument;
        Rtx::GuiSurface& mGui;
        Renderer& mViews;                        ///< for `createOffscreenView` and the doll preview
        const Rtx::SceneDesc& mScene;
        const Rtx::ExtractionStats& mWalked;
        const Rtx::ExtractionStats& mWalkedAgain;
        Resource::ResourceSystem* mResources = nullptr;
        osg::Group* mSceneRoot = nullptr;
        std::uint32_t mUnreadableTextures = 0;
    };
}
```

`Session::frame`, `StopWriter::write` and `checkHolds` take `const TracedRun&` instead of
`RtxRenderer&`. The `/*internal:*/` block on `RtxRenderer` shrinks to one method,
`describeRun() const`.

Closes: the first two items under *The game-side renderer is a god object*.

### 1.5 `MWRender::ViewHost` — what an offscreen view needs from its owner

```cpp
// components/rtx/offscreentrace.hpp — the three things `rebuildSubject` already takes.
namespace Rtx
{
    struct PoseMoment
    {
        const osg::FrameStamp& mStamp;
        std::size_t mFrame = 0;
        Resource::ImageManager& mImages;
    };
}

// apps/openmw/mwrender/viewhost.hpp
namespace MWRender
{
    /// The few live links a traced view keeps to the renderer that made it.
    class ViewHost
    {
    public:
        virtual ~ViewHost() = default;
        virtual Rtx::GuiSurface& getGuiSurface() = 0;
        virtual bool hasScene() const = 0;
        virtual void deferRedraw(TracedView& view) = 0;
        virtual void forgetView(TracedView& view) = 0;

        /// What a view walks its own subtree against. Read per redraw, so it is asked for rather
        /// than stored. Nothing where the resource system has not arrived.
        virtual std::optional<Rtx::PoseMoment> describePose() = 0;
    };
}
```

`RtxRenderer` implements `ViewHost`. `TracedView` holds `ViewHost&` and `Rtx::Traversals&` instead
of `RtxRenderer&`. `getResources`, `getUpdateStamp`, `getFrame`, `getMirror` and `getSceneRoot` leave
the public surface.

Closes: the `TracedView` half of the god-object group.

### 1.6 `Rtx::RenderProfile` — one bag for a run's picture options

```cpp
namespace Rtx
{
    /// Everything a run decides once about how the picture is made. Read from the settings for a
    /// played game, and built from the command line by the harness.
    struct RenderProfile
    {
        Upscale mUpscale = Upscale::Off;
        Preset mPreset = Preset::D;
        Reorder mReorder = Reorder::Off;
        bool mMicromaps = true;
        bool mCountHits = true;
        bool mCountCrossings = false;
        float mDelight = 1.0f;
        bool mShowAlbedo = false;
        bool mFilter = true;
        bool mJitter = false;
        std::optional<float> mExposure;          ///< nothing means "measure it off the frame"
        std::optional<float> mFixedStep;

        static RenderProfile fromSettings();
    };
}
```

`RendererOptions` keeps what device creation needs — the two paths, the extent, the window, the
validation — plus one `RenderProfile`. `FrameOptions` is built from the profile and the three
per-frame values. `RtxTool::FrameRequest` becomes a `RenderProfile` plus the window extent, the field
of view, the day, and the two distant-land switches.

The harness stops writing the profile into `Settings::rtx()` and reading it back. It puts the profile
into the renderer's spec (§1.7). `mExposure` keeps its `std::optional` shape the whole way.

Closes: every item under *One run's options are spelled in five places*.

### 1.7 `MWRender::RtxSetup` — what the harness installs, without a global

```cpp
namespace MWRender
{
    /// What a harness run asks of the ray tracer, if a harness started this process.
    struct RtxSetup
    {
        Rtx::RenderProfile mProfile;
        Rtx::SessionRequest mSession;
        Rtx::SessionResult* mInto = nullptr;     ///< the caller's own; outlives `Engine::go`
    };

    struct RendererSpec
    {
        Stage& mStage;
        SceneUtil::WorkQueue& mWorkQueue;
        std::filesystem::path mResourceDir;
        std::filesystem::path mScreenshotPath;
        std::filesystem::path mCachePath;
        const RtxSetup* mRtx = nullptr;          ///< `GlRenderer` ignores it
    };
}
```

`installSession`, `takeInstalledSession`, `InstalledSession` and the file-static `sInstalled` go.
`readSessionSetting()` stays: it is how a played binary starts a session from `[RTX] session`, and
`Engine` builds an `RtxSetup` from it when the harness installed none.

Closes: the global-mailbox item.

### 1.8 `Rtx::Channel` mirrors the shader, and `GBuffer` holds an array

```cpp
// components/rtx/channel.hpp
namespace Rtx
{
    enum class Channel : std::uint32_t
    {
        Direct = Shaders::CHANNEL_DIRECT,
        Indirect = Shaders::CHANNEL_INDIRECT,
        // ... all fourteen, each equal to its shader constant
    };
    inline constexpr std::size_t sChannelCount = Shaders::CHANNEL_COUNT;

    struct ChannelSpec
    {
        std::string_view mName;
        ChannelFormat mFormat;      ///< the host half of the `GBUFFER_*` macros
    };
    inline constexpr std::array<ChannelSpec, sChannelCount> sChannels{ /* one row per channel */ };
}
```

`GBuffer` holds `std::array<Image, sChannelCount>`, built by walking `sChannels`. `getChannel(Channel)`
replaces the fourteen accessors, and `everyChannel()` becomes the array itself.
`Renderer::readChannel(Channel)` becomes one array lookup. The two images that are not GBuffer
channels get their own calls: `readComposite()` and `readAccumulated()`, with
`Reconstruction::filtered()` answering whether the second exists. `hasChannel` goes.

Closes: *The channel set is enumerated three times*.

### 1.9 `Rtx::OwnedTexture` — one type for an image built in memory

```cpp
namespace Rtx
{
    /// The shape of a mip pyramid: the levels, and the extent of the finest.
    struct MipPyramid
    {
        std::vector<MipLevel> mLevels;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        void reuse();
        std::size_t offsetOf(std::uint32_t level, std::uint32_t x, std::uint32_t y, std::size_t stride) const;
    };

    /// An image this process built and owns, which describes itself the way a file does.
    class OwnedTexture
    {
    public:
        void reuse();
        MipPyramid& getShape();
        std::vector<std::byte>& getBytes();
        void setFormat(TextureFormat format);
        void setName(std::string_view name);
        void setShading(std::span<const float> shading);
        bool isEmpty() const;
        TextureData describe() const;            ///< written once, for all five
    private:
        MipPyramid mShape;
        std::vector<std::byte> mBytes;
        TextureFormat mFormat = TextureFormat::Rgba8Unorm;
        std::string_view mName;
        std::span<const float> mShading;
    };
}
```

`MipChain`, `SpriteLightMap`, `TerrainComposite` and `ContactSheet` each become a builder that fills
one `OwnedTexture`. `AlphaImage` keeps its one-byte-per-texel storage but takes its `MipPyramid` from
the same type, so `at()` and the level walk are written once.

Closes: *Five types hold the same in-memory image*.

### 1.10 `Rtx::SlotJournal` and one debt type

```cpp
namespace Rtx
{
    /// What a table owes its readers about the rows that changed since they last looked.
    class SlotJournal
    {
    public:
        void grow(std::size_t rows);
        void arrived(Index slot);
        void freed(Index slot);
        void written(Index slot);
        std::span<const Index> getArrived() const;
        std::span<const Index> getFreed() const;
        std::span<const Index> getWritten() const;
        void clear();
    private:
        SlotSet mArrived;
        SlotSet mFreed;
        SlotSet mWritten;
    };
}
```

`SlotChanges` becomes `SlotJournal`. `MaterialTable::mWritten`, `DeformerTable::mArrivedRigs` and
`mArrivedMorphs` and `PlacementTable`'s two vectors move onto it. `MaterialTable`'s two run lists
become one `ArrivedRuns { std::vector<Run> mLayers, mMasks; }`, so the run case is named once too.

`SlotBlocks` swaps its bare `std::array<SlotSet, sFrameSlots>` for
`std::array<RowDebt, sFrameSlots>`, so both double-buffered tables owe their rows the same way and
both can owe everything.

Closes: *Change tracking has four shapes*.

### 1.11 `MWRender::EyeState`, and one world description

`WorldState` sheds its camera:

```cpp
namespace MWRender
{
    /// Where the frame is seen from. Not a fact about the world.
    struct EyeState
    {
        float mNearClip = 0.0f;
        float mViewDistance = 0.0f;
        float mFieldOfView = 0.0f;
        osg::Matrixf mProjectionMatrix;
    };

    struct SceneFrame
    {
        osg::Node& mScene;
        const osg::Camera& mCamera;
        const osg::FrameStamp& mWhen;
        const WorldState& mWorld;
        const EyeState& mEye;
        Resource::ImageManager& mImages;
        Terrain::World& mTerrain;
        const Terrain::ObjectStorage& mObjectStorage;
    };
}
```

`Rtx::FrameWorld` goes. `describeWorld` writes straight into the constants:

```cpp
namespace Rtx
{
    /// Fills the world half of `constants` and answers what the cell's light settled the exposure
    /// bias at. Leaves every camera field alone.
    float describeWorld(const WorldReading& reading, Shaders::VisibilityConstants& constants);
}
```

`applyWorld` goes with it. `WorldReading` stays: it is the host-neutral reading `readWorld` builds
from the game, and `openmw-rtxtool` builds one from the content files.

`RenderingManager` finishes its migration. `mAmbientColor`, `mNearClip`, `mViewDistance`,
`mFieldOfView`, `mFieldOfViewOverride`, `mFieldOfViewOverridden`, `mNight`, `mCloudSpeed` and
`mStormParticleDirection` move into `mWorld` and `mEye`, or into the one object that decides them.
`describeWorld()` then fills only what answers per frame.

Closes: every item under *One frame's world is described four times*.

### 1.12 One moon moment, one phase

`MWRender::MoonState` goes. `Sky::MoonMoment` gains the two fields the renderers need
(`mShadowBlend` is already there; `mMoonAlpha` becomes `mAlpha`, which it already is), and
`MoonState::Phase` is replaced by `Sky::MoonPhase` everywhere. `phaseToInt` moves to
`components/sky/moonmodel.hpp` beside the enum. `Rtx::placeMoon` takes `Sky::MoonPhase`.

`MWWorld::Weather` and `MWRender::SkyManager` change signature. `weather.cpp` and `gl/sky.cpp` are
already fork-touched, and the three `static_assert`s in `weather.cpp` go with the cast.

Closes: the two moon items.

### 1.13 Smaller shapes

- `Weather::Precipitation` holds one `Downpour mWeather` member. `setWeather` assigns it.
  `updateRainParameters` reads it. Eleven members go.
- `Rtx::OffscreenTrace` holds `std::variant<Perspective, Orthographic> mProjection` — the same two
  types `OffscreenViewSpec` declares, lifted into `components/sceneutil/offscreenframing.hpp` so
  both name one pair. It holds one `SceneUtil::FlatLight mSun` instead of three vectors.
- `Rtx::TextureTable` replaces `mPaths` and `mBaked` with one
  `std::vector<TextureName>`, where `TextureName` holds the `Kind` and a
  `std::variant<VFS::Path::Normalized, std::string>`. One name per slot.
- `SceneAcceleration` splits into `BottomLevelStore`, `StructureCompaction` and `TopLevelStore`,
  which it owns. Its five instance counters become one `InstanceCounts`, and `SceneStats` holds that
  same struct by value rather than restating its fields.
- `Rtx::FrameSlot` (the struct in `framering.hpp`, renamed `FrameRecord` once `FrameSlot` is a slot
  type) holds two `Submission { VkCommandBuffer mCommands; VkFence mFence; bool mPending; Graveyard
  mGraveyard; }` — one for the world, one for the GUI.
- `Rtx::FrameRing` takes the two counting switches by value at construction, not as
  `const bool&` into `VulkanRenderer`'s members.
- `Rtx::Image` owns its handle and its two views with `Owned<>`, the way `Buffer` does.
- `Rtx::GpuBreakdown` keeps one `std::vector<ZoneRow>` with a flat sample buffer plus a
  `std::vector<Run>` of spans, not `std::vector<std::vector<double>>`.
- `FrameSamples` and `BenchPlace` index one `std::array<_, sTimingCount>` by an
  `enum class Timing { Frame, Wait, Walk, Place }`.
- `Rtx::Pool<T>` replaces `SceneTextures`'s two vector-plus-count pairs.
- `Terrain::ObjectStorage` collapses `collectReferences` and `collectLights` into
  `collect(RefKind kind, ...)`, and writes into `std::vector<PagedCellRef>&` sorted by `RefNum`
  rather than a `std::map`. `collectPagedRefs` takes one `CellSource` interface instead of three
  `std::function`s.
- `Rtx::textureAt` is removed. `Rtx::Renderer::GuiRegion` moves beside the other GUI types.
  `SlotTable::mName` becomes a `std::string_view`. `Rtx::Traversals` is held once, by the object
  that starts a walk, and passed to whoever walks.
- `MWRender::Stage` becomes the one home for the camera, the frame stamp, the event queue, the
  statistics and the scene root. `RtxRenderer` creates them, hands them to `Stage::adopt`, and then
  reads them back through `mStage`. Its five `osg::ref_ptr` members and `getSceneRoot()` go.
- `MWRender::Renderer` sheds `setCompileOperation` — `GlRenderer` makes its own — and replaces the
  loading screen's three `getCompileOperation()` calls with
  `setPreparationBudget(std::optional<PreparationBudget>)`. `getMaxTextureUnits` becomes
  `describeShaderLimits()`, which the ray tracer answers with `Surface::sAssumedTextureUnits`.
  `installStatsOverlay` and `reportStats` move behind one `getStatsSink()`.

---

## Part 2 — Implementation plan

Each step lands on its own. Each names its verification. The order puts the leaf types first, so no
step waits on one after it.

Build with `apps/rtxtool/debug.sh`. Run the tests as gtest binaries with `--gtest_filter`. Format
with `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh`. Where a step can reach a frame, run
`apps/rtxtool/repeatable.sh` and check the scene columns against `.notes/repeatable.txt`.

### Stage A — leaf types, no behaviour changes

**A1. `components/rtx/slot.hpp` and the four slot types.**
Add the header. Change `Rtx::Renderer` and `VulkanRenderer` to take `SceneSlot`. Change
`VisibilityInputs::mSlot` and `Placing::mSlot` to `FrameSlot`. Change `GuiTextures`,
`MyGUIRtx::Texture` and `TracedView` to `GuiSlot`. Delete `Rtx::sWorld`.
*Verify:* `components-tests --gtest_filter='RtxSceneUploaderTest.*:RtxOffscreenTraceTest.*:RtxGuiDrawTest.*:RtxFramesTest.*'`, then `repeatable.sh`.
*Files:* `components/rtx/`, `components/rtxvulkan/`, `components/myguirtx/`, `apps/openmw/mwrender/rtx/`.

**A2. `Rtx::MipPyramid` and `Rtx::OwnedTexture`.**
Add both. Move `MipChain`, `SpriteLightMap`, `TerrainComposite` and `ContactSheet` onto them, then
`AlphaImage`'s pyramid.
*Verify:* `--gtest_filter='RtxMipChainTest.*:RtxAlphaImageTest.*:RtxTerrainCompositeTest.*:RtxContactSheetTest.*:RtxSpriteLightMapTest.*'`.

**A3. `Rtx::SlotJournal`, and `SlotBlocks` on `RowDebt`.**
Replace `SlotChanges`. Move `MaterialTable`, `DeformerTable` and `PlacementTable` onto it. Give
`MaterialTable` one `ArrivedRuns`. Give `SlotBlocks` `RowDebt`.
*Verify:* `--gtest_filter='RtxSlotTableTest.*:RtxSceneTableTest.*:RtxStructureStorageTest.*:RtxSceneDescTest.*'`, then `repeatable.sh`.

**A4. `Rtx::Channel` from the shader, and `GBuffer` as an array.**
Add `components/rtx/channel.hpp`. Rewrite `GBuffer`. Replace `readChannel`'s switch. Split
`readComposite` and `readAccumulated` out. Delete `hasChannel`.
*Verify:* `--gtest_filter='RtxVisibilityTest.*:RtxFramesTest.*:RtxOffscreenTraceTest.*'`, then
`openmw-rtxtool shot --views=<one> --dump=…` and compare the radiance dump against a run taken
before the change.

**A5. Small shapes with no callers outside their own file.**
`Rtx::Image` on `Owned<>`; `FrameRing`'s two booleans by value; `FrameRecord`'s two `Submission`s;
`SlotTable::mName` as a view; `GuiRegion` moved out; `textureAt` deleted; `GpuBreakdown` flattened;
`FrameSamples` and `BenchPlace` on one array; `Rtx::Pool<T>` in `SceneTextures`.
*Verify:* `--gtest_filter='RtxFrameRingTest.*:RtxGpuBreakdownTest.*:RtxFrameTimesTest.*:RtxTextureBuilderTest.*:RtxSlotTableTest.*'`,
then `repeatable.sh`.

### Stage B — the read side of a scene

**B1. `Rtx::SceneTables`, and the backend takes it.**
Add `SceneTables` and `SceneDesc::getTables()`. Change `Rtx::Renderer::setScene`, `extendScene` and
`placeScene` to take `const SceneTables&`. Change `SceneAcceleration`, `SceneBuffers`,
`SceneMicromaps`, `SkinTables`, `InstanceRecord` and `SceneDigest` to take it.
*Verify:* `--gtest_filter='RtxSceneDescTest.*:RtxSceneUploaderTest.*:RtxSceneDigestTest.*:RtxInstanceRecordTest.*:RtxSceneExtractorTest.*'`,
then `repeatable.sh`. The scene columns must be identical.

**B2. Delete the 41 forwarders.**
Rewrite the remaining call sites — `StopWriter`, `checks`, `WorldMirror`, `SceneUploader`,
`TextureBuilder`, `CompositeQueue`. Keep the four derived answers on `SceneDesc`.
*Verify:* the same as B1.

**B3. `Rtx::TextureTable` holds one name per slot.**
*Verify:* `--gtest_filter='RtxSceneDescTest.*:RtxTextureBuilderTest.*'`.

**B4. Split `SceneAcceleration`.**
Move the compaction into `StructureCompaction`, the bottom level into `BottomLevelStore` and the top
level into `TopLevelStore`. Introduce `InstanceCounts`, and hold it in `SceneStats`.
*Verify:* `--gtest_filter='RtxSceneDescTest.*:RtxStructureStorageTest.*:RtxMicromapBakeTest.*:RtxMicromapCurveTest.*'`,
then `repeatable.sh` and one `bench --views=one-cell-walk` against the reading in
`.notes/bench.txt`.

### Stage C — the four interfaces

**C1. Split `Rtx::Renderer` into `SceneSink`, `GuiSurface`, `FrameSource` and `Instrument`.**
`Renderer` inherits all four and gains nothing of its own. `VulkanRenderer` is unchanged apart from
the base list.
*Verify:* it compiles, `--gtest_filter='RtxSceneUploaderTest.*:RtxOffscreenTraceTest.*:RtxGuiDrawTest.*'`,
then `repeatable.sh`.

**C2. Narrow each caller.**
`SceneUploader` and `WorldMirror` take `SceneSink&`. `MyGUIRtx` takes `GuiSurface&`.
`OffscreenTrace` takes both. `StopWriter` and `checks` take `Instrument&`.
*Verify:* the same as C1.

### Stage D — the game-side owner

**D1. `MWRender::TracedRun`.**
Add it. Change `Session::frame`, `StopWriter::write` and `checkHolds` to take it. Add
`RtxRenderer::describeRun()`. Remove eight of the twelve internal accessors.
*Verify:* `openmw-rtxtool check` over the whole suite, then `verify` against a stored run.

**D2. `MWRender::ViewHost` and `Rtx::PoseMoment`.**
Add both. Change `TracedView` to hold `ViewHost&`. Remove the last four internal accessors.
*Verify:* `openmw-rtxtool shot --map-tile=…` and `--doll=…`, compared against pictures taken before
the change.

**D3. `Rtx::RenderProfile`, `MWRender::RtxSetup`, and the end of the global.**
Add both. Put the profile and the session into `RendererSpec::mRtx`. Delete `installSession`,
`takeInstalledSession`, `InstalledSession` and `sInstalled`. Stop `rtxtool` writing the profile into
`Settings::rtx()`.
*Verify:* `openmw-rtxtool bench --views=one-cell-walk` and `view --frames=120`, then
`repeatable.sh`. **This step touches `apps/openmw/engine.cpp`, which is upstream. Wait for a
go-ahead.**

**D4. `MWRender::Stage` becomes the one home.**
Remove `RtxRenderer`'s five `osg::ref_ptr` members and `getSceneRoot()`.
*Verify:* `openmw-rtxtool view --frames=60` and one played frame — the intersection visitor has to
still find a door.

### Stage E — the world description

**E1. `MWRender::EyeState`.**
Split the camera out of `WorldState`. Add it to `SceneFrame`.
*Verify:* `openmw-tests --gtest_filter='MWRenderStageTest.*:RtxWorldMirrorTest.*'`, then
`repeatable.sh`.

**E2. Delete `Rtx::FrameWorld`.**
`describeWorld(const WorldReading&, Shaders::VisibilityConstants&)` writes straight in and returns
the exposure bias. Delete `applyWorld`. Rewrite the tests in
`apps/components_tests/rtx/frameworld.cpp` to build a `WorldReading` and check the constants.
*Verify:* `--gtest_filter='RtxFrameWorldTest.*:RtxVisibilityTest.*:RtxSkylightTest.*:RtxRoomLightTest.*'`,
then `repeatable.sh` **with `--exposure=1`** and a ten-pair reading, per the fork's rule on
determinism.

**E3. Finish `RenderingManager::mWorld`.**
Move the nine loose members into `mWorld` or `mEye`. `describeWorld()` fills only what answers per
frame.
*Verify:* the same as E2, plus a rasterized run — the old renderer reads the same members.
**This step touches `apps/openmw/mwrender/renderingmanager.{hpp,cpp}`, which is upstream. Wait for a
go-ahead.**

**E4. One moon moment and one phase.**
Delete `MWRender::MoonState`. Use `Sky::MoonMoment` and `Sky::MoonPhase` everywhere.
*Verify:* `--gtest_filter='SkyMoonTest.*:RtxMoonBuilderTest.*:RtxFrameWorldTest.*'`, plus a night
view in both renderers.
**This step touches `apps/openmw/mwworld/weather.{hpp,cpp}`, which is upstream. Wait for a
go-ahead.**

### Stage F — the seam and the terrain store

**F1. `Weather::Precipitation` holds one `Downpour`.**
*Verify:* `--gtest_filter='WeatherPrecipitationTest.*:WeatherDownpourTest.*'`, plus a rain view in
both renderers.

**F2. `Rtx::OffscreenTrace` on the shared projection variant and `FlatLight`.**
*Verify:* `openmw-rtxtool shot --map-tile=…` and `--doll=…`.

**F3. `Terrain::ObjectStorage` collapses to one `collect`, one `CellSource` and a flat output.**
*Verify:* `--gtest_filter='*ObjectPaging*:*CellGrid*:RtxDistantLightsTest.*'`, then a walk that
crosses a cell boundary in both renderers. **This step touches `components/terrain/objectpaging.cpp`, which the fork lifted
from upstream. Name it in the commit.**

**F4. `MWRender::Renderer` sheds its OpenGL vocabulary.**
`setPreparationBudget` replaces the loading screen's three `getCompileOperation()` calls.
`GlRenderer` makes its own compile operation. `describeShaderLimits()` replaces
`getMaxTextureUnits`. `getStatsSink()` replaces the two stats calls.
*Verify:* a rasterized load with a loading screen, and a traced load. **This step touches
`apps/openmw/engine.cpp` and `apps/openmw/mwgui/loadingscreen.cpp`, which are upstream. Wait for a
go-ahead.**

---

## Part 3 — What this plan does not do

- **It does not split `VulkanRenderer`.** The class has 55 members, but the passes it owns are
  already separate types and the frame path reads most of them. Splitting it needs a reason beyond
  the member count, and the four interfaces of §1.2 already give every caller a smaller view.
- **It does not touch the shader headers' layout.** `VisibilityConstants` is scalar-packed with a
  `static_assert` on its size, and §1.11 only stops a second struct restating it.
- **It does not change what a frame draws.** Every step is a shape change. The gate on each one is
  that the scene columns of `repeatable.sh` are identical, which is what the fork already asks of a
  change a frame reads.
- **It does not rename `Rtx::Index`.** It is already a named row index, and it is not one of the
  four slots.

## Part 4 — Order of value

If only part of this lands, land it in this order:

1. **A1** — the slot types. It is cheap, it is mechanical, and it removes a whole class of mistake.
2. **D1 and D2** — the god object. They stop the call graph running both ways, which is what makes
   every later step easier to reason about.
3. **A4** — the channels. Three enumerations of one set is the finding most likely to go wrong
   silently.
4. **E2 and E3** — the world. Four descriptions of one frame is the largest volume of duplicated
   code in the diff.
5. **B1 and B2** — the scene facade. The largest mechanical diff, and the smallest risk.
