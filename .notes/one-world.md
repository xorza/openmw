# One world: the harness as a driver of the game

## The proposal in one paragraph

Delete the harness's own world and drive the game's instead. `openmw-rtxtool` links `openmw-lib`,
starts `OMW::Engine` headless at a place, and every verb becomes a request to a running game: put
the eye here, set the hour and the weather, step sixty frames, capture, measure, quit. The in-game
bench, `bench.sh` and `openmw-rtxtool bench` collapse into one bench. `RtxRenderer` becomes the only
frame path. About seven thousand lines go, and the known ways the harness's picture differs from the
game's go with them.

## Why this is the big redesign and not a smaller one

Three hosts drive one renderer today.

| host | where | lines | what it owns |
| --- | --- | ---: | --- |
| the game | `apps/openmw/mwrender/rtx/` | 2,316 | `RtxRenderer`, `WorldMirror`, `readWorld`, `Bench`, `FrameCapture`, `TracedView` |
| the harness | `apps/rtxtool/` | 11,837 | a second world, nine verbs, the reports |
| the harness's tests | `apps/components_tests/rtxtool/` | 5,054 | 31 tests that stand on that second world |

The harness's 11,837 lines split into two halves. About 4,900 lines stand in for engine systems the
game already has. About 6,900 lines are verbs, options, reports and the window.

| stand-in | lines | what it re-implements |
| --- | ---: | --- |
| `content`, `world`, `exteriorindex` | 1,115 | the store, the resource system, the terrain, the paging |
| `stagedworld`, `cellscene` | 1,302 | `MWWorld::Scene` loading a ring of cells, `MWRender::Objects` placing them |
| `npc`, `actor`, `posedactors` | 1,566 | `NpcAnimation` dressing a body, `Animation` posing it |
| `lighting` | 304 | `WeatherManager` lighting a cell at an hour |
| `waterplane`, `objectstorage`, `terrainstorage` | 399 | `MWRender::Water`, the game's two storages |
| `placement`, `find` | 194 | camera placement, cell search |

The tree already says this is a copy. `objectstorage.hpp` calls itself "the second implementation
of the seam". `world.hpp` says a row taken in the harness "cannot be read against one taken" in the
game, because the game merges its near statics and the harness cannot. `actor.hpp` says it is "a
pose and not an animation system". The in-game `bench.hpp` says every renderer defect of the last
stretch "was invisible to `bench` and obvious the moment the game was measured". `todo.txt` lists
"dedupe rtxtool, bench and game".

Two benches exist because of that copy. The harness bench measures a world that is not the
game's. The in-game bench measures the right world but reports through the log, and `bench.sh`
scrapes the log with `awk` to get a table back. The two report paths share `Rtx::FrameSamples`
and nothing above it: the run spec, the warm-up, the crossing count, the report and the JSON
record are written twice.

The AGENTS.md rule is one answer shared with the game, never a second copy of a fact the game
states. The harness world is the largest second copy in the tree. Smaller consolidations, such as a
shared frame driver under both worlds, keep the copy and keep the divergences.

## The measurement that changes the trade

The reason for a separate world was speed. The numbers no longer support it.

| run | wall time | source |
| --- | ---: | --- |
| `openmw-rtxtool shot` at Seyda Neen, release, headless | 4.4 s | measured now |
| the game to its first traced frame at Balmora, debug build | 5.3 s | `openmw.log` timestamps |

The harness shot spends 0.7 s on the device and 1.9 s on the build. The game spends 1.0 s to the
device and 3.0 s to its first cell load. A release game build closes most of the remaining gap.

## The design

### Shape

```
apps/rtxtool/                      the launcher: verbs, options, views.cfg, benches.cfg, reports, PNG compare
apps/openmw/mwrender/rtx/session.* the driver inside the game: what a verb asks of a running world
components/rtx/                    the instruments both once had: bench records, report, JSON, hashes, clocks, perf
```

`openmw-rtxtool` links `openmw-lib`. Its `main` builds an `OMW::Engine` the way `apps/openmw/main.cpp`
does, then hands the engine a `Session` and calls `go()`. The option table comes from
`OpenMW::makeOptionsDescription`, which is already in the library. The forty lines that copy the
parsed values into the engine are repeated in the harness's own `main`.

`Session` replaces `Bench` and the `OPENMW_RTX_SHOT` half of `FrameCapture`. `RtxRenderer` feeds it
once per traced frame, exactly as it feeds `Bench` today. The session reaches the world through
`MWBase::Environment`, which is the route `Bench::fly` already takes.

### What a session does

A session is a small script over a running game. It waits for `State_Running`. On that frame it
places the eye, sets the sky and starts a schedule. Every later frame it steps the schedule and
records what the renderer reports. When the schedule ends it reports, writes what it was asked to
write, and calls `requestQuit()`.

Every step uses an interface that exists and that the RTX-owned code may call.

| need | reached through |
| --- | --- |
| stand at a cell | `--start <cell>` with `--skip-menu`, or `--load-savegame` |
| stand at a point | `MWBase::World::moveObject` and `toggleCollisionMode` |
| look along a direction | `RenderingManager::getCamera()`, `Mode::Static`, `setStaticPosition`, `setYaw`, `setPitch` |
| the hour, the day | `DateTimeManager::setHour` |
| the weather, a transition | `MWBase::World::changeWeather` |
| a person in front of the eye | `MWWorld::ManualRef` and `MWBase::World::placeObject` |
| freeze the world for a reference | `DateTimeManager::setSimulationTimeScale(0)` |
| fly a route | `MWBase::World::moveObjectBy`, as `Bench::fly` does |
| the frame without the GUI | `Rtx::Renderer::readPixels`, as `FrameCapture` does |
| the inventory doll | `MWRender::InventoryPreview`, then `OffscreenView::keepCopy` |
| a map tile | `LocalMap::getMapTexture`, then `Rtx::Renderer::readGuiTexture` |
| what the renderer was handed | `WorldMirror::getScene` |
| stop | `StateManager::requestQuit` |

### Each verb, before and after

| verb | today | after |
| --- | --- | --- |
| `info` | a renderer with no world | unchanged |
| `shot` | stage a region, trace, write a PNG | start the game at the view, step N frames, capture frame N, quit |
| `view` | a fly camera in an SDL window of the tool's own | the game at the view with collision off; the spot is printed on quit |
| `bench` | stage each place, measure, report | teleport to each place, measure, report; routes fly the player |
| `bench.sh` | run the game per save, scrape the log | deleted; `bench --saves=<dir>` is the same run with a report |
| `OPENMW_RTX_BENCH` | an env var the game reads | deleted; the game reads `[RTX] session` from settings |
| `verify` | render every view, compare PNGs | unchanged above the capture |
| `doll` | assemble a body the harness's own way | the game's own `InventoryPreview` of that person |
| `map` | a tile of the staged region | the tile the game's `LocalMap` drew |
| `textures` | the staged scene's texture table | the mirror's texture table |
| `scene` | a report off the staged world | a report off `WorldMirror` |

### What the game gains

The renderer's knobs become one set. The harness holds `--delight`, `--albedo`, `--filter`,
`--exposure`, `--jitter`, `--reorder`, `--crossings` and `--tail`, and the game hard-codes each of
them. After the change `RtxRenderer` reads all of them from `[RTX]` and a session sets them.

The rasterizer becomes a reference. The same session runs under `[RTX] enabled = false`, and
`Renderer::capture` works for both renderers. A shot of the rasterizer and a shot of the trace from
one world state is the comparison AGENTS.md names as the point of keeping the rasterizer untouched.
The harness cannot make that picture today.

The harness measures the frame a player feels. Input, scripts, mechanics, physics and the GUI are
inside `frame ms`, which is what the in-game bench was added to see.

## Where it touches upstream

One edit is required. `OMW::Engine::go()` takes `dt` from the wall clock. A reproducible run needs a
fixed step, so `go()` has to read one setting and use it in place of the measured duration. That is
`apps/openmw/engine.cpp`, one line and one setting read, with the setting itself in the RTX category
that is already in the tree. Nothing in the RTX-owned code can reach that value.

One lift is optional. `parseOptions` in `apps/openmw/main.cpp` is a static function. Moving it into
`options.cpp` lets the harness call it instead of repeating it. Without the lift the harness repeats
forty lines.

Everything else goes through interfaces upstream already exposes: `MWBase::World`,
`RenderingManager`, `DateTimeManager`, `StateManager`, `InventoryPreview`, `LocalMap`.

## Risks, and the spike that settles them

Three things are not proven. One spike answers all three before anything is deleted.

**A hidden window must not pause the game.** `Engine::frame` returns early when
`WindowManager::isWindowVisible()` is false, and SDL reports visibility through `SDL_WINDOWEVENT_HIDDEN`.
`RtxRenderer::createWindow` is RTX-owned and can pass `SDL_WINDOW_HIDDEN`. The backend already runs
without a presenter, and `drawGui` draws into the frame target and not the swapchain. What is not
known is whether SDL sends a hidden event for a window that was never shown. If it does, the
fallback is a shown window that closes when the session ends, which is what `bench --window` does
today anyway.

**Two runs must draw the same frame.** The seed is `--random-seed`. The step is the one upstream
edit. Physics, AI and scripts step from `dt` and are deterministic under a fixed step. The loading
threads decide when a cell's resources arrive, not what arrives, so a still frame after a warm-up
is the same frame twice. A route pays for cache state in `read ms`, as the harness does now, and
`SceneUploader::setSettled` still waits the composites out for a hashed run.

**A shot must stay fast enough to look at.** The measured gap is one second on a debug game build.
The spike times a release build.

The spike is `shot` alone, hosted in the engine: link the library, hide the window, fix the step,
capture frame N, quit. It costs a day. It reports the wall time and whether two runs hash equal.

## What goes, what moves, what stays

| fate | files | lines |
| --- | --- | ---: |
| deleted | the stand-ins listed above, plus `motion`, `window`, `worldclock`, `framing`, `framerequest`, most of `shot`, `view`, `picture`, `textures`, `scene` | ~7,000 |
| moved to `components/rtx/` | `bench` records, report and JSON, `gpuclock`, `perfcontrol`, `framehashes` | ~1,200 |
| replaced by `Session` | `apps/openmw/mwrender/rtx/bench.*`, the `OPENMW_RTX_SHOT` half of `framecapture.*` | 367 out, ~1,000 in |
| kept in `apps/rtxtool/` | `main`, `options`, `verbs`, `views`, `benchsuite`, `viewpoint`, `validationchoice`, `verify`, `contactsheet`, `scenedigest`, `parsefloat` | ~3,500 |
| kept in the game | `rtxrenderer`, `worldmirror`, `readworld`, `tracedview`, the capture half of `framecapture` | ~1,900 |

`placement.cpp` stays for a view that names no `pos`: the establishing shot from outside a cell
still needs the scene's bounds, and `WorldMirror::getScene().getBounds()` gives them.

## The tests

Thirty-one tests in `apps/components_tests/rtxtool/` stand on the harness world. They test
`components/rtx` against real content: a crossing appends, a sweep drops what left, a lamp stands
at its wick, a paged world's ground reaches the mirror, a still camera resolves to a still picture.

Those claims are worth keeping and their fixture is not. They become session checks: a `check` verb
that runs a named list of assertions against the running game and exits non-zero on the first
failure. `scene --twice` is already one of these. One process teleports through every check, so the
suite costs one engine start plus a cell load per check.

Four of them are about the stand-ins themselves and go with them: the three actor tests and
"a region read after another stands on its own ground". The game's `NpcAnimation` is what dresses a
body now, and the game holds one ground.

The tests of `Framing`, `WorldClock` and the harness `TerrainStorage` go with that code. The 56 test
files in `apps/components_tests/rtx/` do not use the harness world and are not affected.

## Phases

1. **Spike.** Engine-hosted `shot`, as above. Decide on its numbers.
2. **Session and bench.** `Session` in `apps/openmw/mwrender/rtx/`, fed by `RtxRenderer`. The bench
   instruments move to `components/rtx/`. `bench` and `verify` run on the session. Delete
   `bench.sh`, the in-game `Bench`, `OPENMW_RTX_BENCH` and `OPENMW_RTX_SHOT`.
3. **The pictures.** `doll`, `map`, `textures` and `scene` on the session. `view` is the game with
   collision off. Delete the harness world and its window. Port the tests to `check`.
4. **Cleanup.** Rewrite the harness section of AGENTS.md. `debug.sh game` and `release.sh game`
   become `openmw-rtxtool view`.

Each phase leaves the tree building and every remaining verb working.

## The alternative, and why not

The smaller redesign keeps both worlds and shares everything below the graph: a `TracedWorld` in
`components/rtx/` that owns the scene, the extractor, the uploader, the sky content and the
residencies, and offers `mirror`, `hand`, `trace` and `settle`. A shared `Bench` beside it takes
the run spec, the warm-up, the crossings, the report and the JSON. `WorldMirror` and
`StagedWorld::mirror` collapse onto the first, and the two benches onto the second.

It saves about 1,500 lines. It leaves 4,900 lines of stand-ins and every divergence they carry:
near statics merged in one host and not the other, a body dressed by two different rules, a cell lit
by two different weather systems, no `CellPreloader` on one side. It also leaves the reason for a
second bench in place. The tree's own comments say those divergences are what hid the last
stretch of defects.

Both designs move the bench instruments into `components/rtx/`. That part is worth doing first
either way, and phase 2 does it.

## Open questions

- Whether a session's options ride on `[RTX]` settings, on the command line, or both. Settings let
  the plain `openmw` binary run a session from `settings.cfg`, which replaces the env vars with one
  mechanism.
- Whether `view`'s clock and weather keys are worth keeping. The console has `set gamehour to` and
  `changeweather`. A Lua player script under `files/rtx/` could bind keys, and that is the only
  place a new interactive control fits without touching `MWInput`.
- Whether `check` runs one process for the suite or one per check. One process is faster; one per
  check isolates a crash.
