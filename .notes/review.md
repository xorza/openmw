# RTX review against upstream

Scope: the full diff against merge base `97c3f81abae4350d7265005ef37249154ac5a3b9`. Read in full:
`components/rtx/`, `components/rtxvulkan/`, `components/rtxbackends/`, `components/myguirtx/`,
`components/surface/`, `components/sky/`, `components/weather/`, `apps/openmw/mwrender/rtx/`,
`apps/rtxtool/`, and the upstream seam (`renderingmanager`, `characterpreview`, `localmap`,
`sceneframe`, `stage`, `renderer`).

Order inside each section: the game frame path first, then the arrival frame, then load time and
the harness. Each item gives the place, the cost, and a direction.

**What the allocation tests see.** `apps/components_tests/rtx/visibility/framecost.cpp` counts heap
calls over a steady frame and over a texture arrival. Beside it, the extractor, the sprite shade and
the weather builder each pin their own steady call at nought. An item below that no test can fail on
says so.

## 1. Structures that allocate

**Measured.** The whole heap costs about 0.4% of the main process's cycles, on a still cell and on a
streaming crossing alike. Nothing in this section moves a frame time.

### 1.1 `RtxRenderer::eventTraversal` builds an event list per frame

`apps/openmw/mwrender/rtx/rtxrenderer.cpp:330`. `osgGA::EventQueue::Events` is a `std::list`, so each
event drained costs a node.

**This one cannot be fixed inside the RTX places.** A `std::list` node is not recycled by `clear()`,
and the queue that hands the list out is `osgGA`'s. It is one node per function key pressed, and
nothing presses one on a steady frame.

## 2. Per-frame computations that can be precomputed

### 2.1 `SceneExtractor::animate` finds the updater with `dynamic_cast` every frame

`components/rtx/sceneextractor.cpp:880-887, 891-898`. `findUpdater` walks both callback chains and
casts each callback, per animated node per frame. `Animated` (`sceneextractor.hpp:647`) keeps only
the state set and the epoch.

Direction: store the updater pointer in `Animated` on arrival. The node owns the callback, so the
pointer lives as long as the entry.

### 2.2 `MirrorTraversal::placed()` multiplies per drawable and per light

`components/rtx/sceneextractor.cpp:318`, called at `:665` and `:444`. One 4x4 multiply per
placement per frame.

Direction: compute the product once per transform push and keep it beside `mHere`.

### 2.3 `resolveMesh` validates a deforming drawable again every frame

`components/rtx/sceneextractor.cpp:1478, 1487, 1492, 1509, 1515`. Per posed body part per frame the
code runs `vertexCountOf` (a `dynamic_cast`), `baseOf`, two `mRigs.find` and two `mMorphs.find`.

Direction: `Known` (`sceneextractor.hpp:580`) keeps the vertex count and the deformer index. The
walk stamps the deformer's epoch through that index.

### 2.4 `SceneDesc::notePosed` does a linear search per pose

`components/rtx/scenedesc.cpp:340`. `std::find` over `mDeformed` per posed mesh. With N posed
meshes a frame does N²/2 comparisons. `release` (`:799`) erases from the same vector.

Direction: a per-mesh flag beside `mKeptMeshes`, cleared with the frame.

### 2.5 `LightGrid::rebuild` computes each light's box three times

`components/rtx/lightgrid.cpp:105, 115, 122`. `boxAround` runs in the sizing loop, the count pass
and the put pass.

Direction: one pass into a scratch of `CellBox`. The count and the put read the scratch.

### 2.6 `SceneDesc::orderLights` sorts on a nine-field tuple every frame

`components/rtx/scenedesc.cpp:700-708`. Called from `sceneuploader.cpp:50` each frame.

Direction: one 64-bit key per light, computed as the light is added. Sort on the key.

### 2.7 `SceneBuffers::place` converts every light, sprite and emitter every frame

`components/rtxvulkan/scenebuffers.cpp:413-438`, then `mLightGrid.rebuild`. The walk refills the
lists each frame, so nothing carries identity between frames. The cost is linear and by design. A
light that did not move still pays its conversion, its sort key and its grid cells each frame.

Direction: a per-frame identity for lights is the change that makes any of this incremental. Not
worth it before the renderer draws everything.

### 2.8 `RtxRenderer::traceWorld` rebuilds the sky every frame

`apps/openmw/mwrender/rtx/rtxrenderer.cpp:818-935`. Every frame builds the room light, the
skylight, the fog, both moons and `FrameWorld` from `WorldState`. `sunShareAloft` reaches
`getSetting("Sun")` (`components/sky/sun.cpp:114`), a string-keyed map. `landReach()` (`:105`)
reads two settings and runs at `:281`, `:679` and `:871`. The inputs change per weather tick, per
hour and per cell.

Direction: keep the previous `WorldState` and rebuild when a field differs. `landReach` is a
constant for the run and belongs in a member.

### 2.9 `DistantLights::collect` does (2r+1)² map lookups per frame

`components/rtx/distantlights.cpp:94-104`. Each cell in reach is a `std::map::find`.

Direction: a flat grid indexed by cell offset.

### 2.10 `SceneExtractor::readMask` reads one texel at a time

`components/rtx/sceneextractor.cpp:238`. `getColor` per texel. Load path, not the frame.

Direction: read the alpha channel through a row pointer.

## 3. Single responsibility

### 3.1 `RtxRenderer`

`apps/openmw/mwrender/rtx/rtxrenderer.hpp`. Owns the SDL window and events, the stage, the mirror
(scene, extractor, residents, uploader, sky content, moon faces), the frame-world description
(`rtxrenderer.cpp:724-988`), the bench, screenshot capture, `OPENMW_RTX_SHOT` keeping, and the
deferred `TracedView` list.

Direction: three types. A `WorldMirror` owns scene, extractor, residents and uploader and answers
`mirror(frame)`. A `readWorld(const WorldState&)` in its own file returns a `Rtx::WorldReading`, so
the game and the harness `CellLighting` path share one builder. A capture type owns screenshot and
keep.

### 3.2 `SceneExtractor`

`components/rtx/sceneextractor.cpp`, 1871 lines. Walks the graph, resolves meshes, rigs and morphs,
reads surface, terrain and water materials, takes textures, steps emitters, places sprites, adds
lights, animates state sets and sweeps.

Direction: split by what is resolved. `MeshResolver` (meshes, rigs, morphs, shape fold),
`MaterialResolver` (surface, terrain, water, textures, animation), `EmitterResolver` (emitters,
sprites). The walk and the sweep stay.

### 3.3 `SceneDesc`

`components/rtx/scenedesc.hpp`, 1198 lines. Geometry tables, deformers, placements, materials and
layers, textures, per-frame lights and sprites, change lists and revisions.

Direction: `TextureTable` and `PlacementTable` as members with their own files. Each change list
lives with the table it describes.

### 3.4 `VulkanRenderer`

`components/rtxvulkan/vulkanrenderer.cpp`, 1605 lines. The frame ring, the GUI ring, the view
scenes, the targets, the upscaler, the readbacks and the stats.

Direction: `FrameRing` (slots, fences, graveyards) and `ReadbackQueue` as members.

### 3.5 `SceneUploader::hand`

Decides Placed, Extended or Rebuilt, owns the composite queue, owns the texture loader and logs
timing.

Direction: the decision is the one thing here nothing else can make. The queue and the loader are
things it holds, and each could belong to whatever holds the uploader instead.

### 3.6 `StagedWorld` (harness)

`apps/rtxtool/stagedworld.cpp`. Staging, streaming, weather, warm-up, relight, motion and framing.

Direction: `Streaming` (`moveTo`, `dropCellsOutside`) and `Lighting` (`relight`) as members.

### 3.7 `CompositeQueue`

A queue, a thread, a shading cache and a collector.

Direction: `mPainted` becomes a `ShadingCache` type.

## 4. Ownership and arguments

### 4.1 `SceneExtractor::addLight` has an unused parameter

`components/rtx/sceneextractor.cpp:926`. `path` is never read. The caller (`:444`) computes
`getNodePath()` for it.

Direction: remove the parameter.

### 4.2 `ExtractionStats&` threads through eleven methods

`components/rtx/sceneextractor.cpp:926, 1041, 1184, 1242, 1250, 1328, 1448, 1766, 1790, 1822,
1848`. `MirrorTraversal` already holds `mStats` (`:360`).

Direction: the extractor holds `ExtractionStats* mStats` for the walk. `extract` sets it and clears
it after.

### 4.3 The Vulkan frame context travels as four loose arguments

`recordPlacement` (`vulkanrenderer.hpp:238`) takes seven arguments, `SceneAcceleration::place`
(`sceneacceleration.hpp:83`) eight, `SceneMicromaps::bake` (`scenemicromaps.hpp:74`) nine,
`SceneBuffers::binSprites` (`scenebuffers.hpp:102`) eight. Commands, slot, timer and graveyard
always travel together.

Direction: a `FrameContext { VkCommandBuffer, std::uint32_t slot, GpuTimer*, Graveyard& }` passed
by reference.

### 4.4 Harness loaders take long argument lists

`loadRegion` (`apps/rtxtool/cellscene.hpp:188`) takes ten arguments. `measurePlace`
(`apps/rtxtool/bench.cpp:242`) takes seven with out-parameters. `readRegion` (`cellscene.hpp:144`)
takes five.

Direction: a `RegionRequest` struct for the load. `BenchRun` already exists and can carry the
measure's inputs.

### 4.5 `CellLighting` duplicates `Rtx::WorldReading`

`apps/rtxtool/lighting.hpp:18`. The fields mirror `WorldReading`, and `applyLighting` (`:98`) copies
them across one by one.

Direction: `CellLighting` holds a `WorldReading` plus the harness-only fields (seconds, water level,
rain).

### 4.6 Optional ownership of `Traversals` in two places

`SceneExtractor` (`sceneextractor.hpp:667-668`) and `OffscreenTrace` (`offscreentrace.hpp:195-196`)
both keep `mOwnTraversals` and `mTraversals&`.

Direction: the caller always owns `Traversals`, and both types take a reference. The one caller
that has none makes one.

### 4.7 `TracedView` holds two owners

`apps/openmw/mwrender/rtx/tracedview.hpp:52-53`. The view holds `RtxRenderer&` and `Rtx::Renderer&`.
The owner keeps raw pointers and a `forgetView` protocol.

Direction: the view holds the owner only and asks it for the renderer. The owner hands out a
`std::unique_ptr<TracedView>` and prunes its list when a destructor reports.

### 4.8 Residents receive per-cell facts every frame

`apps/openmw/mwrender/rtx/rtxrenderer.cpp:677-681`. Five setters on `DistantLights` and two on
`TerrainResidency` per frame. `follow(span)` (`sceneextractor.hpp:406`) reassigns per frame. The
values change per cell, not per frame.

Direction: one `Viewpoint { eye, grid, outdoors }` set per frame. `follow` and `setReach` run when
the world changes.

### 4.9 `RendererOptions::mWindow` is a raw `SDL_Window*`

`components/rtx/renderer.hpp:107`. Documented as deliberate. Leave.

## 5. Non-canonical data structures

### 5.1 Texture slots are six parallel structures

`components/rtx/scenedesc.hpp`: `mTextures` (`:1046`), `mBaked` (`:1050`), `mFreeTextures`
(`:1072`), `mTextureRefs` (`:1144`), `mTextureNews` (`:1168`), `mTextureIndex` (`:1184`) and
`mBakedIndex` (`:1196`). Two of them own a string per slot. The two maps own a second copy of each
string as their key.

Direction: one `TextureSlot` row (kind, name offset into one string arena, refs, news) and one map
keyed on a view into the arena.

### 5.2 A per-slot flag has three spellings

`std::vector<char>` (`scenedesc.hpp:1080-1081`), `std::vector<std::uint8_t>` (`:1175`),
`std::vector<SlotNews>` (`:1168-1169`).

Direction: one type for a per-slot flag.

### 5.3 `mDeformed` is a vector used as a set

`components/rtx/scenedesc.hpp:1004`. See 2.4.

### 5.4 `mPlacements` is keyed on a derived hash

`components/rtx/sceneextractor.hpp:592`. An `unordered_map<std::size_t, Known>` keyed on a hash of
the path and the drawable address. The sweep (`sceneextractor.cpp:793`) is an `erase_if` over the
map. A collision silently merges two placements.

Direction: key on the placement identity itself, or keep the row in the instance table and let the
sweep read the epoch there.

### 5.5 `GpuBreakdown` is two parallel vectors

`components/rtx/frametimes.hpp:137-140`. A `vector<string>` beside a `vector<vector<double>>`.

Direction: one `vector<double>` with a stride. Names as `string_view` into the pass table.

### 5.6 `CompositeQueue` keeps node containers

`components/rtx/compositequeue.hpp:186-187` (`std::deque`), `:194` (`unordered_map<Index,
TerrainComposite>`), `:220` (`unordered_map<std::string, ShadingMap>`).

Direction: `mPending` and `mDone` as ring buffers over the request pool the queue already keeps.
`mFinished` as a vector indexed by material slot.

### 5.7 `DistantLights::mCells` is a `std::map`

`components/rtx/distantlights.hpp:114`. See 2.9.

### 5.8 `TimeOfDaySettings::mSunriseTransitions` is a string-keyed map of five constants

`components/sky/timeofday.hpp:32`. `getSetting` (`:40`) takes a `std::string`. `sun.cpp:114` asks
for `"Sun"` per call. Lifted shared code, so the rasterizer reads it too.

Direction: an enum-indexed array. Needs a go-ahead.

### 5.9 `LoadedCells` is keyed on a string

`apps/rtxtool/cellscene.hpp:93`. A `std::map<std::string, LoadedCell>`. `dropCellsOutside` builds a
`std::set<std::string>` per call (`cellscene.cpp:106`). Harness load path.

Direction: key on the cell's grid position.

### 5.10 `MyGUIRtx::RenderManager::update` keeps its clock in function statics

`components/myguirtx/rendermanager.cpp:182-183`.

Direction: members.

## Read and not flagged

`ShapeFold`, `RunList`, `SpanAllocator`, `RowDebt`, `Graveyard`, `FrameSamples`, `SlotTable`, the
per-pass descriptor writes in `components/rtxvulkan/`, `Sky::MoonModel::at`, `Sky::sunAt`,
`Weather::stormEffect`, and the `MyGUIRtx` vertex and batch buffers. Each keeps its scratch and
does no work on the frame that a previous frame did not.
