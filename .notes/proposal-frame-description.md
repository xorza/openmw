# Proposal: the frame is described by its owners

Status: Phases 0 to 3 (redesigns A and B) are in the working tree, uncommitted, verified
2026-09-19. Phase 4 is not started. Phase 3 landed with one change to the shape below: the
engine hands the renderer a reference to its `Misc::FrameClock` once (`Renderer::setFrameClock`)
and `advance(simulationTime)` keeps its signature, because a loading screen advances the
renderer with no new time and the tracer reads the clock's own step for its interface. No
step arithmetic was added, so nothing new needed a test beyond the moved clock's.

Companion to `review-upstream-diff.md`. This addresses the items I would act on first, as one
redesign with a phased plan. Items it closes are named at the end of each phase; delete them from
the review as each phase lands.

## What is wrong, in one paragraph

`RenderingManager` was turned into a record-keeper. `WeatherManager::update` calls ten setters on
it, six of them added by the fork, and each writes one field of `SkySettled`. Every frame,
`describeWorld` copies that record into `WorldState`, and `GlWorld::describe` replays it into
`SkyManager` through upstream's own setters, with an `Applied` struct to find the edges. To make
the record complete, the fork took the sky's clocks out of `SkyManager` and gave them to the game,
copied `WeatherResult` (five strings) every frame, and put a second clock beside the first for the
tracer. The frame ended up carrying 24 sky fields, two `osg::Node*` for the rain, a view matrix
written in a second phase, and a `WeatherResult` pointer into a copy. The rule that the
rasterizer is the path not taken was broken in `sky.cpp` for the record's sake, and four of the
review's "two sources" items are the same record seen from four sides.

The fix is not a smaller record. It is no record: the weather manager already holds every one of
those facts, so the frame hands a reference to the weather manager's own state to both renderers,
and each derives what it draws from that. The game keeps only what the game decides (the toggles,
the water level, the fog bands, the sun light it owns), and each renderer keeps the clock it
draws by.

## A. The sky, described once by the weather manager

### The type

`apps/openmw/mwrender/skystate.hpp`, a fork file, beside `sceneframe.hpp`. `MWRender`, because
`WeatherResult` is `MWRender` and the weather manager fills that too.

```cpp
struct SkyState
{
    /// What the weather settled this update — `WeatherManager::mResult`, owned here.
    WeatherResult mWeather;

    /// `WeatherManager::mTimeSettings`, owned here: the hours a renderer ramps its dawn on.
    Sky::TimeOfDaySettings mTimes;

    /// Whether the weather ran: false indoors, where nothing below is written.
    bool mOutdoors = false;

    /// The orbit's direction, before the disc's warp (`Sky::sunDiscPosition`) and before the
    /// `match sunlight to sun` choice, which are each renderer's own.
    osg::Vec3f mSunDirection;
    bool mNight = false;

    /// `Sky::sunUp(hour, mTimes)`: whether the disc is drawn at this hour.
    bool mSunUp = true;
    float mGlareFade = 1.0f;

    Sky::MoonState mMoons[2] = {};

    /// What drives the particle effect, which is not the deck's direction.
    osg::Vec3f mStormParticleDirection;

    /// The script ids the world answers today through four `MWBase::World` virtuals.
    int mWeatherId = 0;
    std::optional<int> mNextWeatherId;
    float mTransition = 0.0f;

    /// The gust, `WeatherManager::mWindSpeed`; the base is in `mWeather`.
    float mWindSpeed = 0.0f;
};
```

### The owner

`WeatherManager` holds `SkyState mSky` and exposes `const SkyState& getSkyState() const`.
`MWBase::World` gains one pure virtual, `getSkyState()`, and `World` forwards it. That is the same
channel `describeWorld` already reads the weather ids through, and it is the one upstream
interface this proposal touches.

To keep `weather.cpp` reading as upstream reads, `mResult` and `mTimeSettings` become reference
members initialised from `mSky.mWeather` and `mSky.mTimes`. Every `mResult.` and
`mTimeSettings.` line in `calculateWeatherResult` is then untouched. Name it for what it is: a
diff-minimising alias in an upstream file. If reference members are unwelcome, rename the two
fields instead and accept the larger diff.

In `WeatherManager::update`, the six fork setters become six writes:

| today                                                   | after                                            |
| ------------------------------------------------------- | ------------------------------------------------ |
| `mRendering.setStormParticleDirection(mStormDirection)` | `mSky.mStormParticleDirection = mStormDirection` |
| `mRendering.setSunEnabled(Sky::sunUp(...))`             | `mSky.mSunUp = Sky::sunUp(...)`                  |
| `mRendering.setGlareFade(glareFade)`                    | `mSky.mGlareFade = glareFade`                    |
| `mRendering.setMoonStates(a, b)`                        | `mSky.mMoons[0] = a; mSky.mMoons[1] = b`         |
| `mRendering.setWeather(mResult)`                        | nothing: `mResult` is `mSky.mWeather`            |
| `mRendering.setNight(isNight)` (upstream, stays)        | also `mSky.mNight = isNight`                     |

`mSky.mSunDirection = sunDir` goes beside the upstream `setSunDirection(sunDir)` call, which
stays because it writes the sun light. `mSky.mOutdoors = isExterior` at the top of `update`, and
the four id fields where the transition is settled.

### What the frame carries

`SceneFrame` gains two references and `WorldState` shrinks to what the game itself decides:

```cpp
struct SceneFrame
{
    osg::Node& mScene;
    const osg::FrameStamp& mWhen;
    const SkyState& mSky;               // the weather manager's own
    const Precipitation& mPrecipitation; // the game's particle systems, asked rather than copied
    const WorldState& mWorld;
    const EyeState& mEye;
    Terrain::World& mTerrain;
    const Terrain::ObjectStorage& mObjectStorage;
    float mDeltaTime;
    bool mPaused;
};

struct WorldState
{
    /// The sun light as `mSunLight` holds it, which `setSunDirection`, `setSunColour`,
    /// `setAmbientColour` and `configureAmbient` write: read off the light, not recorded twice.
    osg::Vec4f mSunLightPosition;
    osg::Vec4f mSunColour;
    osg::Vec4f mAmbientColour;
    osg::Vec4f mNightEye;

    /// `setSunColour`'s third argument, the one number the light does not hold.
    float mSunVisibility = 0.0f;

    /// The game's toggles: outdoors and `tsky`; `Moons_Script_Color` painted on.
    bool mSkyShown = false;
    bool mMoonRed = false;

    Location mLocation = Location::Interior;
    bool mWaterEnabled = false;
    float mWaterHeight = 0.0f;
    bool mUnderwater = false;
    FogBand mAir;
    FogBand mWaterFog;
    osg::Vec3f mPlayerPosition;
    std::optional<ESM::Cell::AMBIstruct> mRoom;
    float mGameHour = 0.0f;

    /// The `timescale` global, for the clock a renderer steps its sky by.
    float mTimeScale = 0.0f;
};
```

`SkySettled` is deleted. `EyeState` loses `mView`: the tracer adopted the camera and reads
`getCamera().getViewMatrix()` in `renderFrame`, which is the moment it is valid. `describeFrame`
then fills every field of the frame in one phase.

### What each renderer derives

Two functions are lifted to `components/sky/`, beside `sunUp` and `skyStep`:

- `Sky::sunDiscPosition(direction)`: the two lines of `RenderingManager::setSunDirection` that
  warp the orbit into where the disc is drawn. `RenderingManager` calls it for the light, and
  both renderers call it for the disc, so the warp is written once.
- `Sky::SkyClock`: `{ double mSeconds; float mCloudScroll; float mStarRoll; }` with
  `step(dt, timeScale, cloudSpeed)`, stepping the scroll by `skyStep(dt, timeScale) * cloudSpeed / 400`
  and the roll by upstream's `timeScale * dt * 2π / (3600 * 96)`. The tracer's clock, and only
  the tracer's.

`GlWorld::describe` reads `frame.mSky` and feeds `SkyManager` exactly as `WeatherManager` fed it
upstream, in upstream's order, every frame the weather ran:

```cpp
const SkyState& sky = frame.mSky;
mSky->setEnabled(world.mSkyShown);          // idempotent; the edge tracking goes
if (world.mSkyShown && sky.mOutdoors)
{
    sky.mSunUp ? mSky->sunEnable() : mSky->sunDisable();
    mSky->setSunDirection(Sky::sunDiscPosition(sky.mSunDirection));
    mSky->setGlareTimeOfDayFade(sky.mGlareFade);
    mSky->setMasserState(sky.mMoons[0]);
    mSky->setSecundaState(sky.mMoons[1]);
    mSky->setWeather(sky.mWeather);
    mSky->setMoonColour(world.mMoonRed);
}
if (!frame.mPaused)
    mSky->update(frame.mDeltaTime);          // upstream's clocks, inside upstream's class
```

The shader chain's uniforms come from the same two records: `setSunPos(sunDiscPosition(...),
sky.mNight)`, `setSunVec(-world.mSunLightPosition)`, `setSkyColor(sky.mWeather.mSkyColor)`,
`setWindSpeed(sky.mWindSpeed)`, the base wind from `sky.mWeather.mBaseWindSpeed`.

`SkyManager` gets back `update(float duration)`, `mCloudAnimationTimer`, `mAtmosphereNightRoll`
and `mTimescaleClouds`, as upstream has them. The precipitation lines stay out (see "kept as
is"). `git diff upstream/master -- apps/openmw/mwrender/sky.cpp` then shows one removed block and
no edited line inside `update` or `setWeather`.

`SkyReader::read(const SceneFrame&, seconds, reach)` reads `frame.mSky` for the weather, the
moons, the glare, the disc, `mTimes` and the cloud crossing, `frame.mWorld` for the room, the fog
bands, the toggles and the water, and its own `Sky::SkyClock` for `mScroll`, `mStarRoll` and
`mSkySeconds`. `RtxRenderer::renderFrame` steps that clock on unpaused frames with the sky shown,
from `frame.mDeltaTime`, `frame.mWorld.mTimeScale` and `frame.mSky.mWeather.mCloudSpeed`. The
NaN guard on `mCloudBlendFactor` stays in `SkyReader`, where it was; the tracer is the renderer
that mixes by it.

`Precipitation::setWeather(const SkyState&)` replaces `setWeather(const WeatherResult&)` plus
`setStormParticleDirection`, and `RenderingManager::update` calls it before `mPrecipitation->update()`
whenever `sky.mOutdoors`. `Precipitation` gets `const` getters and the frame hands it by reference.

### What is deleted from `RenderingManager`

`setWeather`, `setMoonStates`, `setSunEnabled`, `setGlareFade`, `setStormParticleDirection`,
`updateSkyClocks`, `mSky`, `mWeather` and `mTimescaleClouds` go. `mFrameWorld` and `mFrameEye`
become the two values `mFrame` refers to, filled in `describeFrame` alone. The three water fields
and `isUnderwater` stay (see "kept as is"). `setSkyEnabled` keeps `mSkyShown` and the precipitation enable. `skySetMoonColour`
keeps `mMoonRed`. `setSunColour` keeps `mSunVisibility`. `setSunDirection` becomes upstream's
body with the disc warp read from `Sky::sunDiscPosition` and the `mSky->setSunDirection` line
gone, because `GlWorld` makes that call.

`Sky::TimeOfDaySettings::shared()` is deleted; the frame carries the weather manager's copy.
`fromFallback()` stays for the weather manager and the tests.

### Upstream files this touches, named

- `apps/openmw/mwbase/world.hpp`: one pure virtual, `getSkyState()`.
- `apps/openmw/mwworld/worldimp.{hpp,cpp}`: its override, one line each.
- `apps/openmw/mwworld/weather.{hpp,cpp}`: already touched; the six setter calls become writes,
  two members become aliases, one accessor is added.
- `apps/openmw/mwrender/sky.{hpp,cpp}`: already touched; the clocks return, the diff shrinks.
- `apps/openmw/mwrender/renderingmanager.{hpp,cpp}`: already touched; the diff shrinks by the
  setters and the record.

### Why not a smaller step

Handing the frame a `const WeatherResult&` alone would remove the copy and nine derived fields
but keep the six setters, the second record and the game-owned clocks. The clocks are the part
that changed `sky.cpp`, and the setters are the part that made `RenderingManager` a record. Both
go only if the weather manager's state is the description.

## B. The run's clock and schedule belong to the host

Today the seam carries `tickSchedule()` and `beginFrame(double)` for one implementation, the
tracer owns `Rtx::FrameClock` and the engine reads its step back through the seam, and the
harness's per-frame hook reaches the world through the renderer. The played game installs a
file-static `PlayedRun`. The run is the host's; the renderer draws.

### The shape

`OMW::Engine` gets one host interface in place of `RendererFactory`, a fork addition in the same
place:

```cpp
class EngineHost
{
public:
    virtual ~EngineHost() = default;
    virtual std::unique_ptr<MWRender::Renderer> createRenderer(const MWRender::RendererSpec& spec) = 0;

    /// How long every frame stands for, or nothing to follow the wall.
    virtual std::optional<float> getFrameStep() const { return std::nullopt; }

    /// The one point in the frame where the world is the calling thread's alone.
    virtual void beforeFrame() {}
};

void setHost(EngineHost& host);
```

`FrameClock` moves to `components/misc/frameclock.hpp` unchanged: it has no ray-tracing
dependency, and `Engine` is built without `components/rtx` under `-DOPENMW_RTX=OFF`. `Engine::go`
makes one from `mHost ? mHost->getFrameStep() : std::nullopt` and drives the loop:

```cpp
clock.advance(measured);
const double dt = clock.getStep() * timeManager.getSimulationTimeScale();
mRenderer->advance(clock.getNow(), timeManager.getRenderingSimulationTime());
```

`Renderer::advance(referenceTime, simulationTime)`: the rasterizer's viewer keeps stamping the
wall itself, as upstream did, and ignores the first argument; the tracer stamps both and keeps
the difference of two reference times as the step its GUI ages by. `tickSchedule` and `beginFrame`
leave the seam; `Engine::frame` calls `mHost->beforeFrame()` where it called `tickSchedule`.

`RtxRun` loses `beforeFrame`. `RtxTool::Session` implements `RtxRun` and `EngineHost`: the
game-side hook and the renderer-side hooks were always two things. `hosted.cpp` calls
`engine.setHost(session)`. `RtxRenderer` reads the stated step from `mInstalled.mSetup.mStep` for
`mSettled` and `FrameOptions`, and holds its `PlayedRun` as a member instead of a file static.

### Upstream files this touches, named

`apps/openmw/engine.{hpp,cpp}`: already touched; `RendererFactory` and its setter become
`EngineHost` and its setter, and the loop reads the clock the engine owns.

## C. Dirty regions on the pictures the game paints

The global map's overlay and the local map's fog of war are `osg::Image`s the game writes into
and marks dirty whole. Under GL that re-uploads the whole overlay per explored cell, which
upstream's GPU blit never did; under the tracer `SharedTexture::refresh` compares every row to
find the change. One rectangle, kept beside the image by the thing that painted it, serves both.

`SceneUtil::PaintedTexture`, a fork file in `components/sceneutil/`: an `osg::Texture2D` that
owns its image and a dirty rectangle, with `paint(rect)` to mark and `apply()` overridden to
subload the rectangle when one is pending, else upstream's whole upload. `GlobalMap` and
`LocalMap` make their overlay and fog textures this type and call `paint` where they call
`dirty()` today. `MyGUIPlatform::GuiRenderManager::shareTexture` gains an overload for
`PaintedTexture&`; the tracer's mirror reads the rectangle and sends those rows, and the plain
overload keeps the row comparison for the save thumbnail and the video. A rectangle nobody
reads is the whole picture, so the two upload paths agree by construction.

This is the smallest of the three and independent of A and B.

## Kept as is, and why

- **The `Precipitation` lift.** Both renderers walk the game's particle systems and only one
  draws the dome, so the particles are content and the dome is geometry. The diff in `sky.cpp`
  is a move; review it with `git diff -M20% --color-moved=dimmed-zebra upstream/master --
  apps/openmw/mwrender/sky.cpp apps/openmw/mwrender/precipitation.cpp`, which colours moved lines
  across the two files.
- **`Water`'s own `mEnabled`, `mToggled`, `mTop`.** Upstream's class, fed from the frame by
  `GlWorld`. The copies inside it are the price of not editing it. `RenderingManager` keeps the
  three fields as the game's decision and answers `isUnderwater` for the frame; `Water::isUnderwater`
  has no caller and stays because the file is upstream's.
- **`optimizer.cpp` and `CompareCellStores`.** Both make an order deterministic across processes
  without changing any picture. They are bug fixes to upstream, and the mirror cannot sort its way
  around the first (merged vertex order is decided inside the optimizer). Keep them, each as its
  own commit whose message says the picture is unchanged, and take the `is_transparent` line out.
- **`objectpaging.cpp`.** Mechanical; the review item stands on its own.

## Implementation plan

Every phase is one commit, verified before the next starts. Each phase ends with the two
questions the fork can answer without a window: did the tracer draw the same pictures, and does a
run still repeat. Never run a gate beside a build.

### Phase 0: the baseline

1. `apps/rtxtool/rtx release build`.
2. `rtx release shot --views=all --out=<scratch>/before --exposure=1`.
3. `rtx release bench --views=one-cell-walk --hashes=<scratch>/before.hashes --window=false`.
4. `rtx debug check`. Keep the report.

### Phase 1: `SkyState`

Files: new `mwrender/skystate.hpp`; `mwworld/weather.{hpp,cpp}`; `mwbase/world.hpp`;
`mwworld/worldimp.{hpp,cpp}`; `mwrender/sceneframe.{hpp,cpp}`; `mwrender/renderingmanager.{hpp,cpp}`;
`mwrender/precipitation.{hpp,cpp}`; `mwrender/glworld.cpp`; `mwrender/rtx/skyreader.{hpp,cpp}`;
`mwrender/rtx/worldmirror.cpp`; `mwrender/rtx/rtxrenderer.cpp`; `components/sky/sundisc.{hpp,cpp}`.

1. Add `SkyState`. Give `WeatherManager` the member, the aliases, the accessor and the six
   writes. Add the `MWBase::World` virtual and the `World` override.
2. Add `Sky::sunDiscPosition`; call it from `RenderingManager::setSunDirection`.
3. Put `const SkyState&` and `const Precipitation&` on `SceneFrame`. Rewrite `WorldState` to the
   list above. Delete `SkySettled` except its four clock fields, which move to `WorldState` for
   this phase only (Phase 2 removes them).
4. `GlWorld::describe` reads the two records; drop `Applied::mSkyEnabled`. `SkyReader::read` takes
   the frame. `WorldMirror::mirror` asks `frame.mPrecipitation` for its nodes.
5. `Precipitation::setWeather(const SkyState&)`; call it from `RenderingManager::update`.
6. Delete the six setters, `mWeather`, `RenderingManager::setWeather`, `TimeOfDaySettings::shared`.
7. Tests. `apps/openmw_tests/mwrender/readworld.cpp` builds a `SkyState` beside its `WorldState`;
   its six cases keep their expected values. Add one case: a `SkyState` with `mOutdoors == false`
   leaves `GlWorld::describe`'s dome untouched (assert through `SkyManager::isEnabled` and the
   sun's state). `components_tests/sky/sundisc.cpp` gains `sunDiscPosition` with hand-computed
   values: direction `(-400, 75, -100)` gives `(400, -75, 0)`; direction `(0, 75, -100)` gives
   `(0, -75, 400)`.
8. Verify: `rtx debug test`; `rtx release shot --views=all --exposure=1 --against=<scratch>/before`
   must say every picture is the same; `rtx debug repeat --pairs=10`; `rtx debug gate`.

Closes: "`RenderingManager::setWeather` copies `WeatherResult`", "derives nine more `SkySettled`
fields", "two readings of `TimeOfDaySettings`", "`SkySettled` has 24 fields", "`WorldState`
carries the rasterizer's precipitation", "`EyeState::mView` is written in `renderFrame`",
"`RenderingManager` keeps `mFrameWorld` … beside the seam".

### Phase 2: each renderer keeps the clock it draws by

Files: `mwrender/sky.{hpp,cpp}`; `components/sky/skyclock.{hpp,cpp}`; `mwrender/glworld.cpp`;
`mwrender/rtx/skyreader.{hpp,cpp}`; `mwrender/rtx/rtxrenderer.cpp`; `mwrender/sceneframe.{hpp,cpp}`;
`mwrender/renderingmanager.{hpp,cpp}`.

1. Restore `SkyManager::update(float)`, its two timers and `mTimescaleClouds` from
   `upstream/master`, minus the precipitation lines. Confirm with
   `git diff upstream/master -- apps/openmw/mwrender/sky.cpp` that no line inside `update` differs.
2. Add `Sky::SkyClock`. `SkyReader` owns one; `RtxRenderer::renderFrame` steps it.
3. Add `WorldState::mTimeScale`. Delete `updateSkyClocks`, the four clock fields and
   `mTimescaleClouds` from `RenderingManager`.
4. Tests. `components_tests/sky/skyclock.cpp`: one step at `dt = 1/60`, `timeScale = 30`,
   `cloudSpeed = 400` advances the scroll by `1/60` and the roll by `30 * (1/60) * 2π / 345600`;
   at `timeScale = 60` the scroll advances by `2/60` and the roll by twice as much; the scroll
   wraps at four. `readworld.cpp`'s "the deck and the fog read the sky's clock" reads the reader's
   clock instead of the frame.
5. Verify as Phase 1. The tracer's formulas are unchanged, so `--against` must still say the same.

Closes: "`SkyManager` lost its clocks", "two cloud clocks that agree unless `Weather_Timescale_Clouds`".

Trade-off, stated: the star-roll constant now exists in `SkyManager::update` (upstream's text)
and in `Sky::SkyClock`. The test above pins the second to the first's numbers. The tracer keeps
ignoring `Weather_Timescale_Clouds`, as it does today.

### Phase 3: the host owns the run

Files: `engine.{hpp,cpp}`; `components/misc/frameclock.hpp` (moved from `components/rtx/`);
`mwrender/renderer.{hpp,cpp}`; `mwrender/glrenderer.{hpp,cpp}`; `mwrender/rtx/rtxrenderer.{hpp,cpp}`;
`mwrender/rtx/rtxrun.hpp`; `apps/rtxtool/session.{hpp,cpp}`; `apps/rtxtool/hosted.cpp`;
`components/rtx/CMakeLists.txt`; `components/CMakeLists.txt`.

1. Move `FrameClock`. Add `EngineHost`; replace `setRendererFactory`.
2. `Renderer::advance(referenceTime, simulationTime)`; delete `tickSchedule` and `beginFrame`.
3. `RtxRenderer`: drop `mClock`; keep the last reference time for the GUI step; read the stated
   step from the setup; hold `PlayedRun` as a member.
4. `Session` implements both interfaces; `RtxRun::beforeFrame` goes.
5. Tests. `components_tests` for `FrameClock` move with it. `openmw_tests/mwrender/rtxrenderer.cpp`
   gains: two `advance` calls one stated step apart hand the GUI that step.
6. Verify: `rtx debug repeat --pairs=10` (the clock is what makes a run repeat), `rtx debug check`
   (`queue-held` and `frames-overlap` read the step), `rtx debug view --frames=120`, gate.

Closes: "`Renderer::tickSchedule()` and `Renderer::beginFrame(double)`", "`RtxSetup` … two
construction paths".

### Phase 4: dirty regions

Files: new `components/sceneutil/paintedtexture.{hpp,cpp}`; `mwrender/globalmap.cpp`;
`mwrender/localmap.cpp`; `components/myguiplatform/guirendermanager.hpp` and both backends'
`shareTexture`; `components/myguirtx/sharedtexture.{hpp,cpp}`.

1. Add `PaintedTexture`. Use it for the overlay and the fog of war.
2. The overload on `shareTexture`; the mirror reads the rectangle.
3. Tests. `components_tests`: painting two rectangles yields their union; `apply` with nothing
   pending uploads nothing (count through a fake `osg::State` is heavy; test the rectangle
   arithmetic and the "whole picture when nobody painted" rule). `openmw_tests/mwrender`: exploring
   one cell marks exactly that cell's rectangle.
4. Verify: `rtx release shot --map --against=<scratch>/before`; `rtx debug check` (the first
   place writes the tile).

Closes: "`GlobalMap::exploreCell` calls `mOverlayImage->dirty()`", and halves the reason for
"`MapWindow::paintExplored` runs every frame".

### Order and dependencies

Phase 1 before Phase 2: the clocks can only leave the game once the frame carries the weather
manager's state, because the tracer's clock needs `mCloudSpeed` from there. Phase 3 is independent
of 1 and 2 but shares `rtxrenderer.cpp`, so it goes after them to keep each diff readable. Phase 4
is independent.

### What the review diff looks like afterwards

| file                                  | today       | after                                          |
| ------------------------------------- | ----------- | ---------------------------------------------- |
| `mwrender/sky.cpp`                    | 5+ / 519−   | one moved block; `update` and `setWeather` upstream's |
| `mwrender/sceneframe.hpp`             | 312 new     | about 170: `WorldState`, `EyeState`, `SceneFrame` |
| `mwrender/sceneframe.cpp`             | 230 new     | about 120: `describeWorld`, `describeEye`, `describeFrame` |
| `mwrender/renderingmanager.hpp`       | 79+ / 39−   | about 40+: the seam, the toggles, `describeFrame` |
| `mwworld/weather.cpp`                 | 17+ / 123−  | same removals; six calls become six writes     |
| `mwrender/renderer.hpp`               | 399 new     | two virtuals fewer                             |
| `mwrender/rtx/rtxrenderer.cpp`        |             | no clock, no static run                        |

The numbers are estimates from the field lists above, not measurements.
