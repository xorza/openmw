# One world: implementation plan

The design is `one-world.md`. This is the order of work, the files each step touches, and what
proves each step before the next starts. Every step leaves the tree building and every remaining
verb working.

## Rules for every step

- **Changes land in RTX-owned places.** `apps/rtxtool/`, `apps/openmw/mwrender/rtx/`,
  `components/rtx*/`, `apps/components_tests/{rtx,rtxtool,rtxbench}/`, `files/rtx/`, `.notes/`,
  and the RTX blocks of the two `CMakeLists.txt` files that already carry them.
- **Two upstream edits are named below and each waits for a go-ahead.** No step depends on the
  second one.
- **Verification is the same chain each time.** Build the targets the step touched. Run the test
  binary with a filter. Run `shot` at the reference view and compare its figures with the step
  before. Run the format check.

```
cmake --build build-debug -j32 --target openmw-rtxtool components-tests
./build-debug/components-tests --gtest_filter='Rtx*'
cd build-debug && ./openmw-rtxtool shot --validation=false --out=/tmp/shot.png
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

- **The reference view is `seyda-neen-ship`.** Today it reports 85.7% of primary rays hit, 8,544
  instances and 569 textures. The hosted world merges the active grid's statics, so the instance
  count will drop the first time the game's world is traced. The hit fraction must stay within a
  point. That difference is the divergence the design removes, and step 0 records it.

## The one description

A session is described once, as a struct in the RTX-owned game code. The harness fills it. The
renderer reads it. Nothing parses a second spelling of it.

```cpp
// apps/openmw/mwrender/rtx/session.hpp
namespace MWRender
{
    /// Where a stop stands. A cell and an eye, or a savegame.
    struct Stand
    {
        std::optional<ESM::RefId> mCell;
        std::optional<std::filesystem::path> mSave;
        std::optional<osg::Vec3f> mEye;
        std::optional<osg::Vec3f> mLook;
    };

    /// What the sky does at a stop.
    struct Sky
    {
        std::optional<float> mHour;
        std::optional<int> mDay;
        std::optional<std::string> mWeather;
        std::vector<std::string> mTurnThrough;
    };

    /// How long a stop runs and whether it goes anywhere.
    struct Schedule
    {
        std::uint32_t mWarm = 0;
        std::uint32_t mMeasured = 0;
        std::optional<Route> mRoute;   // to, speed
        bool mFrozen = false;          // simulation time scale nought
        std::uint32_t mAccumulate = 0; // frames averaged into one picture
    };

    /// What a stop writes or checks.
    struct Actions
    {
        std::optional<std::filesystem::path> mCapture;   // the last measured frame, as a PNG
        std::optional<std::filesystem::path> mDump;      // linear radiance
        std::optional<std::string> mDoll;                // an NPC id
        std::optional<std::filesystem::path> mMapTile;
        bool mTileEveryFrame = false;
        bool mHash = false;
        bool mTail = false;
        bool mDigest = false;         // what `scene` prints
        bool mDigestTwice = false;
        std::optional<std::filesystem::path> mSheet;     // what `textures` writes
        std::vector<CheckId> mChecks;
    };

    struct Stop
    {
        std::string mName;
        Stand mStand;
        Sky mSky;
        Schedule mSchedule;
        Actions mActions;
    };

    struct SessionRequest
    {
        std::vector<Stop> mStops;
        bool mHeadless = true;
        bool mQuitAtEnd = true;
        std::optional<std::filesystem::path> mJson;
        std::optional<std::filesystem::path> mHashes;
        std::optional<std::filesystem::path> mAgainst;
        std::optional<std::filesystem::path> mPerfControl;
    };

    /// What the harness reads back after `Engine::go` returns.
    struct SessionResult
    {
        int mExitStatus = 0;
        std::vector<Rtx::BenchPlace> mPlaces;
    };

    /// The channel between a launcher and the renderer the engine builds for itself.
    ///
    /// A process-wide slot, filled once before `Engine::go` and taken once by `RtxRenderer`'s
    /// constructor. It exists because `RendererSpec` is filled inside `Engine::go`, and adding a
    /// field there is an upstream edit for a value only one launcher sets.
    void installSession(SessionRequest request);
    std::optional<SessionRequest> takeInstalledSession();
    void publishSessionResult(SessionResult result);
    SessionResult takeSessionResult();
}
```

The renderer's own knobs are not in the request. `--delight`, `--albedo`, `--filter`, `--exposure`,
`--jitter`, `--reorder`, `--crossings`, `--upscale`, `--preset` and `--distant-cells` become `[RTX]`
settings, and the harness sets them in memory before `go()`. `pageTerrainFrom` already sets
`mDistantLandCells` that way. One description of the renderer's configuration, read by both
binaries.

The plain `openmw` binary runs the bench subset from `[RTX] session = 10s:2s@12000`, which is the
spelling `OPENMW_RTX_BENCH` takes today. That string is parsed into one `Stop` by the same parser
the harness uses for `--seconds`, `--warmup` and a route's speed.

## Step 0: the spike

Two halves. The first needs no upstream edit and answers two of the three risks. The second needs
the fixed step and answers the third.

### Half one: a hosted shot

| file | change |
| --- | --- |
| `apps/rtxtool/CMakeLists.txt` | `openmw-rtxtool` links `openmw-lib`. `openmw-rtxtool-lib` does not, so `components-tests` stays engine-free. Require `BUILD_OPENMW`. |
| `apps/openmw/mwrender/rtx/session.hpp`, `session.cpp` | the structs above, the slot, and a `Session` that handles one stop with `mCapture` only |
| `apps/openmw/mwrender/rtx/rtxrenderer.cpp` | take the installed request in the constructor. `createWindow` adds `SDL_WINDOW_HIDDEN` and `RendererOptions::mWindow = nullptr` for a headless request. `renderGui` skips `presentFrame` with no window. `fitToWindow` keeps the requested extent. Feed `mSession` where `mBench` is fed. |
| `apps/rtxtool/main.cpp` | a `hosted` switch on `shot`: build an `OMW::Engine` the way `apps/openmw/main.cpp` does, set `--skip-menu`, `--start`, `--no-sound`, `--no-grab` and the seed, set `[Video] framerate limit` to nought, install the request, call `go()`, read the result |
| `apps/rtxtool/engine.hpp`, `engine.cpp` | the forty lines of `parseOptions` that copy the parsed values into the engine, repeated here until the optional lift lands |

The hosted `shot` places the eye with `Camera::setMode(Mode::Static)`, `setStaticPosition`,
`setYaw` and `setPitch` on the first frame in `State_Running` with a scene, and moves the player
to the eye with `MWBase::World::changeToCell` so the ring loads around it. The built-in camera
script leaves a static camera alone. It warms 120 frames and captures the next one.

What half one reports:

- wall time of the hosted shot against the 4.4 s of today's, both in release
- whether a hidden window pauses the game through `WindowManager::isWindowVisible`
- the hit fraction and the instance count at the reference view

If the hidden window pauses the game, the fallback is a shown window the session closes. Note it
and carry on.

### Half two: the fixed step

**Upstream edit one, named here and waiting for a go-ahead.** `apps/openmw/engine.cpp`, in `go()`:
the measured frame duration is replaced by `[RTX] fixed step` where that setting is not nought.
One line and one setting read. The setting lives in `components/settings/categories/rtx.hpp` and
`files/settings-default.cfg`, which are both already the fork's.

What half two reports: two hosted shots of the same view hash equal.

### Gate

Go on when the wall time is within two seconds of today's and the two hashes are equal. Stop and
rethink the design if either fails.

## Step 1: the instruments move to `components/rtxbench/`

Engine-free and independent of the spike. Everything here is a move, and the tests move with it.

| from `apps/rtxtool/` | to `components/rtxbench/` | what it is |
| --- | --- | --- |
| `gpuclock.*` | `gpuclock.*` | `nvidia-smi` read around a run |
| `perfcontrol.*` | `perfcontrol.*` | perf's control fifo |
| `framehashes.*` | `framehashes.*` | one hash a frame and the compare |
| `bench.hpp` `Crossings`, `BenchPlace`; `bench.cpp` `report`, `asJson`, `writeJson` | `benchrecord.*` | what a place came to, printed and recorded |
| `apps/openmw/mwrender/rtx/bench.cpp` `Span`, `readSpan`; `bench.cpp` `getMeasured`, `getWarmup` | `benchspec.*` | `10s:2s@12000` and frames-from-seconds |

`Rtx::FrameSamples`, `GpuBreakdown` and the row formatting stay in `components/rtx/frametimes.*`.

| tests | move |
| --- | --- |
| `apps/components_tests/rtxtool/{gpuclock,perfcontrol}.cpp` | `apps/components_tests/rtxbench/` |
| `apps/components_tests/rtxtool/verify.cpp` hash cases | `apps/components_tests/rtxbench/framehashes.cpp` |

New `components/rtxbench/CMakeLists.txt`, added under the RTX block of `components/CMakeLists.txt`.
`openmw-rtxtool-lib` and `openmw-lib` both link it.

Verification: the harness `bench --views=seyda-neen-ship --frames=60` prints the same report as
before the move, and the moved tests pass under their new filter.

## Step 2: the session, and `shot`, `bench` and `verify` on it

### The session

`Session` grows from the spike's one action into the full state machine.

| part | does |
| --- | --- |
| `Session::beforeFrame` | called from `RtxRenderer::renderFrame` before the walk. Starts the next stop when the previous one ended: teleport, place the eye, set the hour, day and weather, reset the schedule. Advances a route and a weather turn by the frame index. Freezes or thaws the simulation time scale. |
| `Session::afterFrame` | called where `Bench::frame` is called today, with the same five arguments. Records samples during the measured frames. Runs the stop's actions on the frame they are due. Ends the stop. |
| `Session::finish` | reports every place through `Rtx::describePlace`, writes the JSON, writes and compares the hashes, publishes the result, calls `requestQuit` when asked. |

A stop's frames are counted only where `drawsWorld()` and `hasScene()` hold, so loading screens
count for nothing.

The route flies the player with `moveObjectBy` as `Bench::fly` does, and moves the static camera
with it. The weather turn calls `changeWeather` at the frame its blend begins, and the game's own
`WeatherManager` runs the transition. A hashed run sets `SceneUploader::setSettled(true)` on the
mirror.

| file | change |
| --- | --- |
| `apps/openmw/mwrender/rtx/session.*` | the state machine |
| `apps/openmw/mwrender/rtx/rtxrenderer.*` | `mSession` replaces `mBench`. `OPENMW_RTX_SHOT` and `mCapture.keep` go. `mCountHits` and the frame options come from `[RTX]` settings. |
| `apps/openmw/mwrender/rtx/bench.*` | deleted |
| `apps/openmw/mwrender/rtx/framecapture.*` | `keepFrames`, `keep` and `sKeepAtMost` deleted |
| `components/settings/categories/rtx.hpp`, `files/settings-default.cfg` | `session`, `fixed step`, `count hits`, `count crossings`, `reorder`, `delight`, `show albedo`, `filter`, `exposure`, `jitter` |
| `apps/openmw/CMakeLists.txt`, root `CMakeLists.txt` | the `OPENMW_RTX_BENCH` option and its definition deleted |

### The harness

`shot`, `bench` and `verify` build a `SessionRequest` and run the engine. Their staged-world paths
are deleted. `doll`, `map`, `textures`, `scene` and `view` keep the staged world until step 3.

| file | change |
| --- | --- |
| `apps/rtxtool/main.cpp` | `commandShot`, `commandBench`, `commandVerify` fill a request. `chooseView`, `chooseBenchViews` and `applyConditions` stay and resolve `views.cfg` into stops. |
| `apps/rtxtool/session.hpp`, `session.cpp` (new) | `requestFrom(...)` for each of the three verbs, and `runHosted(request)` which sets the renderer's settings, builds the engine and reads the result |
| `apps/rtxtool/shot.*` | deleted. `--repeat` becomes a frozen stop of that many measured frames. `--accumulate` becomes a frozen stop with `mAccumulate`. `--tail` and `--dump` are actions. |
| `apps/rtxtool/bench.*` | deleted. `--saves=<dir>` adds one stop per `*.omwsave`, which is what `bench.sh` ran. |
| `apps/rtxtool/verify.*` | keeps `compareFrames` and the directory walk; the rendering half goes |
| `apps/rtxtool/bench.sh` | deleted |
| `apps/rtxtool/release.sh`, `profile.sh` | `release.sh bench` and the profile call the verb as before; the comment about `bench.sh` goes |

The harness keeps `FrameHashes` and the report reading through `SessionResult`, so what it prints
after `go()` is the summary line and the exit status.

Verification: `bench --suite=default --seconds=5` prints rows for three places with `frame ms`,
`wait ms`, `walk ms` and `place ms`. `verify --out=a` then `verify --out=b --against=a` reports
every view the same. `shot --repeat=30` reports a best and a median.

## Step 3: the pictures and the window; the world goes

### The pictures

| verb | how the session answers |
| --- | --- |
| `doll` | a `MWWorld::ManualRef` of the NPC placed in front of the eye with `MWBase::World::placeObject`, then `MWRender::InventoryPreview` on that `Ptr`. `getTexture()` is a `MyGUIRtx::Texture`, whose slot `Rtx::Renderer::readGuiTexture` reads. `--clothes=false` unequips through the inventory store. |
| `map` | an `OffscreenViewSpec` framed the way `frameMapTile` frames one today, through `RtxRenderer::createOffscreenView`, then `keepCopy` and `getCopy`. `LocalMap` is private to `WindowManager`, so its own tile cannot be read without an upstream accessor. `frameMapTile` stays as the one copy of that framing, and is the one place upstream edit two would remove. |
| `textures` | `WorldMirror::getScene()` texture table into `contactsheet` |
| `scene` | `scenedigest` over `WorldMirror::getScene()` and the walk's `ExtractionStats`. `--twice` walks the mirror again inside the frame and prints what that added. `--find` walks the active cells' stores. |
| `view` | a stop with no end, `mHeadless = false`, collision off. The spot is printed through `viewpoint.cpp` on quit, from `Camera::getPosition`, `getYaw` and `getPitch`. |

**Upstream edit two, optional and waiting for a go-ahead.** `apps/openmw/mwbase/windowmanager.hpp`
gains `getLocalMap()`, so `map` reads the tile the game drew. Without it the framing stays
duplicated in twenty lines.

### The world goes

| deleted from `apps/rtxtool/` | lines |
| --- | ---: |
| `content`, `world`, `exteriorindex`, `objectstorage`, `terrainstorage`, `waterplane` | 1,514 |
| `stagedworld`, `cellscene`, `lighting`, `find` | 1,657 |
| `npc`, `actor`, `posedactors`, `motion` | 1,593 |
| `window`, `worldclock`, `framing`, `framerequest`, `picture`, `textures`, `scene`, `view` | 1,760 |

`placement.cpp` stays for a view with no `pos`, over `SceneDesc::getBounds`. `cellchoice.cpp`
shrinks to the `x,y` against name spelling, which becomes `ESM::RefId::esm3ExteriorCell` or the
interior's id.

Options deleted with the world: `--actor`, `--npc` for rows, `--people`, `--props`, `--actor-time`,
`--sea-time`, `--distant-terrain`, `--distant-statics`, `--data`, `--data-local`, `--content`,
`--fallback`, `--fallback-archive`, `--encoding`. The engine's option table supplies the last six.
`--npc` stays for `doll`.

### The tests become checks

A `check` verb runs named checks against the running game and exits non-zero on the first failure.
Each check is a stop with `mActions.mChecks`, and each `CheckId` maps to one function in
`apps/openmw/mwrender/rtx/checks.cpp` over the world, the mirror and the renderer.

| former test file | checks | claim kept |
| --- | --- | --- |
| `staging.cpp` | 4 | one cell visited twice digests the same; a distant flame stands where it stands; a drop's motion is its own fall; nothing falls under water |
| `crossing.cpp` | 7 | the next cell appends; the sea is one sheet; walking every frame leaves the scene where it was; paged ground survives the frames after it; residents leave with their cell; lamps burn across a crossing; a long walk holds a grid |
| `retire.cpp` | 2 | what the walk stops finding leaves and what is shared stays; a compacted scene renders as one that lost nothing |
| `lamps.cpp` | 3 | a light with no mesh burns; a light off by default is not placed; a lamp stands at its wick |
| `props.cpp` | 1 | a cell's emitters run |
| `material.cpp` | 1 | every surface is described and agrees with its state |
| `pagedterrain.cpp` | 8 | the eight claims about paged ground, distant lights and composites, as named in the file |
| `stability.cpp` | 1 | a still camera resolves to a still picture |

Deleted with their subject: `actor.cpp`, `framing.cpp`, `worldclock.cpp`, `terrainstorage.cpp`,
`installation.*`, and `crossing.cpp`'s "a region read after another stands on its own ground".

`CI/check_rtx_checks.sh` runs `openmw-rtxtool check --all` in `build-debug`. One process visits
every check's stop, so the suite costs one engine start and a cell load per stop.

Verification: every verb in `--help` runs. `check --all` passes. `components-tests` builds without
`openmw-rtxtool-lib` linking anything of the world.

## Step 4: cleanup

| file | change |
| --- | --- |
| `AGENTS.md` | the verification paragraph: `shot` takes about five seconds and traces the game's world; `bench` is the one bench; `check --all` is the world gate. The traps: `OPENMW_RTX_BENCH` goes. The owned places: `components/rtxbench/`, `apps/components_tests/rtxbench/`. A line recording upstream edit one, and two where it landed. |
| `apps/rtxtool/debug.sh`, `release.sh` | `game` runs `openmw-rtxtool view --load-savegame=...` |
| `.notes/todo.txt` | "dedupe rtxtool, bench and game" goes |
| `.notes/bench.txt` | the heading says which verb the rows come from |
| `files/rtx/views.cfg` | the header no longer says the window has its own keys |

## Order and what works after each step

| after step | `shot` | `bench` | `verify` | `doll`, `map`, `textures`, `scene` | `view` | tests |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | staged, plus a hosted switch | staged | staged | staged | staged | all |
| 1 | staged | staged, new report code | staged | staged | staged | moved ones pass |
| 2 | hosted | hosted, one bench | hosted | staged | staged | world tests still pass |
| 3 | hosted | hosted | hosted | hosted | hosted | `check --all` |
| 4 | hosted | hosted | hosted | hosted | hosted | `check --all` |

Step 1 can run before or beside step 0. Step 2 needs both. Step 3 needs step 2.

## Sizes

| step | added | removed |
| --- | ---: | ---: |
| 0 | ~600 | 0 |
| 1 | ~100 of CMake and moves | 0 |
| 2 | ~900 | ~1,900 |
| 3 | ~700 | ~6,600 |
| 4 | ~50 | ~150 |

The tree ends about 7,000 lines smaller, with one bench, one frame path and one world.


## Where the work stands

Steps 0 and 1 are done and verified. What follows is what the spike measured and what it changed
about the plan above.

### Step 1, complete

`components/rtxbench/` holds the clock, perf's fifo, the frame hashes, the run spec and the record.
Both hosts read it. `Rtx::describePlace` prints the same table for a staged place and for the game
measuring itself, and each line only appears where the host has something to put in it. The
harness's `bench` report is byte-identical in shape to what it printed before the move. Nineteen
moved and new tests pass, and 596 RTX tests pass whole.

### Step 0, complete, and what it cost

A hosted `shot` runs. It starts an engine, teleports to the view, warms up, freezes the world,
captures the last measured frame and quits.

| gate | result |
| --- | --- |
| the camera reaches the same place | 85.0% of primary rays hit, against 85.7% staged |
| two runs draw the same frame | the two PNGs hash equal |
| the wall time | **not measured** — the release build was stopped part way |

The instance count is 4666 hosted against 8544 staged, which is the near-statics merge
`gameMergesActiveGridStatics` names. That is the divergence this work removes, not a fault.

### Three things the spike found, none of them in the plan

**A teleport cannot be made from a render callback.** `Engine::frame` lets the Lua worker run
between `updateTraversal` and `renderFrame`, so a cell change made from the render fires a
cell-change event into a Lua state another thread is already in — a segmentation fault inside sol's
own error handler, within a second of the first stop. Moved earlier, into `updateTraversal`, it
hangs instead: `LoadingScreen::draw` runs an update traversal per tick of its progress bar, so
`changeToCell` re-enters the call it was made from once per loaded cell and never returns.

The fix is a second upstream edit, in the same function as the first: `Engine::frame` calls
`mRenderer->tickSchedule()` after `mStateManager->update`. `MWRender::Renderer::tickSchedule` is a
defaulted no-op on this fork's own seam, so the rasterizer is untouched. That is one line in
`apps/openmw/engine.cpp` and eleven in `renderer.hpp`, and it is where the game's own state changes
already happen.

**Two runs are not the same run until three clocks are stated.** The plan named one, and there are
three:

| what | fixed by |
| --- | --- |
| how far the simulation steps | `[RTX] fixed step`, read in `Engine::go` — the plan's edit |
| how long the renderer is told a frame took | `Rtx::FrameOptions::mSinceLast`, from the same setting |
| which sample the trace takes | the stop's own frame count, not the game's frame number |

The third was the one that mattered. With the first two fixed and a 600-frame warm-up, two runs
still differed over 47% of the frame by up to 38 of 255 — the game's frame number carries every
frame a loading screen happened to draw, so the two runs sat at different points in the Halton
sequence. Walking the sequence by the stop's own count made them equal.

**A hidden window does not pause the game.** `WindowManager` starts with its visibility true and SDL
sends no event for a window that was never shown, so the fallback the plan held in reserve is not
needed.

### Step 2, complete

`shot`, `bench` and `verify` all drive a real game. One `Stop` is built from a view file entry, one
`Rtx::BenchSpec` says how long every stop runs, and one report prints them.

| gone | replaced by |
| --- | --- |
| `apps/openmw/mwrender/rtx/bench.*` | `MWRender::Session` |
| `apps/rtxtool/bench.cpp`, `bench.hpp` | the same |
| `apps/rtxtool/bench.sh` | `bench --load-savegame=<file>` |
| `OPENMW_RTX_BENCH`, and the build option that gated it | `[RTX] session` |
| `OPENMW_RTX_SHOT`, and the frame-keeping half of `FrameCapture` | `shot --out` |
| `--map-tile` | the game's own compass, which traces one every frame |

**Every knob the two hosts disagreed about is a setting now.** `delight`, `show albedo`, `filter`,
`exposure`, `jitter`, `count crossings` and `reorder` join `upscale` and `preset` under `[RTX]`. The
game used to hard-code each of them and the harness used to take each as an option, so a picture
taken by one and a frame drawn by the other were traced by two differently configured renderers.
The command line still names them; what it does now is write the setting the renderer reads.

**The validation layers are the exception, and they ride on the request.**
`Rtx::sValidationByDefault` says why they must not be a setting: a developer's diagnostic in a
player's configuration file is a build whose quoted numbers were measured through the layers because
somebody left a line behind. So a launcher states them for the one run it is making, and a played
session has only the build to go on.

### Step 3, complete

Every verb drives a real game. The harness went from 78 files to 21, and the world it staged is
gone: its content reader, its cell loader, its NPC assembler, its animation poser, its weather
lighting, its water, its terrain storages, its window and its fly camera.

| verb | how it answers now |
| --- | --- |
| `scene` | `WorldMirror::getScene`, and the walk's own account of what it met |
| `textures` | the same scene's texture table, through the shared contact sheet |
| `map` | an `OffscreenViewSpec` framed as `MWRender::LocalMap` frames one |
| `doll` | `MWRender::InventoryPreview` of an NPC the world placed |
| `view` | the game, with collision off and the player's own camera |
| `check` | a new verb, below |

`contactsheet` and `scenedigest` moved to `components/rtxbench/` with them: what a sheet shows and
what a digest names are facts about the content, not about whichever host staged it.

### The fixture tests became a verb

Thirty-one tests stood on the deleted world. `openmw-rtxtool check` asks the same kind of claim of
the running game, at every place of a suite, and exits non-zero on the first failure.

| check | what it asserts |
| --- | --- |
| `walk-twice` | a second walk over one graph adds no mesh and no material, and resolves every drawable |
| `surfaces-described` | no placement wears a material nothing described |
| `lights-placed` | the world holds lights to cast |
| `crossings-append` | a route crossed boundaries and not every crossing rebuilt |
| `picture-settles` | a still camera over a still world draws the same picture twice |

**That is five claims where there were thirty-one, and the gap is real.** What the deleted tests
also asserted, and what a check has yet to be written for: a lamp stands at its wick; a light off by
default is not placed; a light with no mesh still burns; a cell's emitters run; the sweep drops what
the walk stopped finding and keeps what is shared; a compacted scene renders as one that lost
nothing; the eight claims about paged ground, distant lights and composites; a drop's reported
motion is its own fall; nothing falls where the eye is under water; the sea is one sheet; residents
leave with their cell. Each is still true of the code and none is guarded.

### What is left

- Time the hosted `shot` in release against the 4.4 s the staged one took, and close the spike gate.
- Write the twelve checks listed above, and a `CI/check_rtx_checks.sh` that runs `check --all`.
- Step 4: the guidance file and the two run scripts.
- `view` lost the keys that ran its clock, turned its weather and printed a `views.cfg` block. The
  console answers the first two with `set gamehour to` and `changeweather`; the third wants a Lua
  script under `files/rtx/` or a console command, and `describeSpot` and `describeBlock` are kept
  for whichever it turns out to be.
