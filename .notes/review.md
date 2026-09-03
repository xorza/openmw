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

**Investigated, measured and settled.** Every item below was read again in the code and profiled;
three were built and then taken back out because an interleaved A/B said they bought nothing or cost
something. Method: `build-release`, `profile.sh --view=vivec --seconds=10`, three A/B pairs with a
binary of each build swapped into place between runs. **A single pair says nothing here** — the first
one taken made two symbols look 0.3 to 0.8 points cheaper, and three pairs put both inside the
spread.

| item | outcome |
| --- | --- |
| 2.1 `animate` finds the updater every frame | built, measured at nothing, reverted |
| 2.2 `placed()` multiplies per drawable | built, measured at nothing, reverted |
| 2.3 `resolveMesh` re-validates a deforming drawable | **done**, in the part the review got right |
| 2.4 `notePosed` searches per pose | **done** |
| 2.5 `LightGrid::rebuild` computes each box three times | built, measured *worse*, reverted |
| 2.6 `orderLights` sorts on a nine-field tuple | open, 0.12%: shape work when `Light` is next touched |
| 2.7 `SceneBuffers::place` converts everything each frame | open: needs a per-frame identity for a light |
| 2.8 `traceWorld` rebuilds the sky each frame | closed: absent from the profile, and `landReach` is not a settings read |
| 2.9 `DistantLights::collect` does map lookups | open at 0.01%, and it belongs with 5.7 |
| 2.10 `readMask` reads one texel at a time | **done** |

Why the three failed, so that nobody builds them again:

- **2.5, the light grid's boxes, is slower.** 1.69% before against 1.90% after, and every run of the
  second leg above every run of the first. `boxAround` is about twenty floating-point operations that
  stay in registers; the scratch is 24 bytes a lamp stored once and read twice, which for Vivec's 614
  lamps is 15 KB pushed through the cache. The arithmetic was cheaper than the memory.
- **2.1's cache never engages on the population that pays for it.** Most callback-bearing nodes in a
  cell carry a keyframe controller and no `StateSetUpdater` at all, so they paid a failed hash lookup
  on top of the chain walk they already paid. What the cache saved on the nodes that do carry one, it
  spent on the nodes that do not.
- **2.2 is the right direction and still buys nothing.** A tally says Vivec enters 36,931 transforms
  and reaches 99,299 drawables, so the product per transform is under two fifths of the multiplies —
  and `osg::Matrixf::mult`, `addDrawable` and `MirrorTraversal::apply` were all flat. It kept a
  64-byte matrix saved and restored around every transform to get there, which is 2.5's shape.

Two things the section as a whole taught, which are worth more than any of its items:

- **Section 2 is about 8% of the main process at Vivec, and half of that is work that has to
  happen.** Where the time actually is, from the same profiles: `SpriteShade::layDown` 24% of a
  still frame and `osg::Group::traverse` 9%; `TerrainComposite`'s constructor 18% of a crossing,
  `ShapeFold` 10% and `paintedLight` 6%. All of it outside this section.
- **On this frame, at this size, storing an answer to avoid recomputing it lost every time it was
  tried.** Three of three caches measured at nothing or worse against the arithmetic they replaced.
  The three changes that stand all do *less work* rather than *remember more*.

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
