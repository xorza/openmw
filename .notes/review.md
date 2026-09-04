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

## Closed

Sections 1 to 3 are settled. What survives of them is here: the reason a thing was not done, so that
nobody does it again.

**1. Structures that allocate.** The whole heap costs about 0.4% of the main process's cycles, on a
still cell and on a streaming crossing alike. Nothing in that section moved a frame time. Its one
remaining item — `osgGA::EventQueue::Events` is a `std::list`, so `RtxRenderer::eventTraversal` pays
a node per event drained — cannot be fixed inside the RTX places, and it is one node per function key
pressed.

**2. Per-frame computations.** About 8% of the main process at Vivec, half of it work that has to
happen. `SpriteShade::layDown` alone is 24% of a still frame and `TerrainComposite`'s constructor is
18% of a crossing, both outside it. Three items were built, measured against the build without them
over three interleaved A/B pairs, and taken back out:

- **The light grid's boxes are slower kept than recomputed.** 1.69% before against 1.90% after, every
  run of the second leg above every run of the first. `boxAround` is twenty floating-point operations
  in registers; the scratch is 24 bytes a lamp stored once and read twice, which for Vivec's 614
  lamps is 15 KB through the cache.
- **Caching the state-set updater never engages on the population that pays for it.** Most
  callback-bearing nodes carry a keyframe controller and no `StateSetUpdater`, so they paid a failed
  hash lookup on top of the chain walk they already paid.
- **Working the placement out per transform buys nothing measurable.** Vivec enters 36,931 transforms
  and reaches 99,299 drawables, so it is under two fifths of the multiplies — and `osg::Matrixf::mult`
  and `addDrawable` were both flat. It keeps a 64-byte matrix saved and restored around every
  transform to get there.

**On this frame, at this size, storing an answer to avoid recomputing it lost every time it was
tried.** What stands from that section does less work rather than remembering more: one deformer
lookup instead of two, `osg::Array::getType()` instead of RTTI, the game's blend maps read along the
row, and a flag instead of a linear search per pose.

**3. Single responsibility.** Five of seven built — `WorldMirror`, `readWorld` and `FrameCapture` out
of `RtxRenderer`; `MeshResolver`, `MaterialResolver` and `EmitterResolver` out of `SceneExtractor`;
`TextureTable` and `PlacementTable` out of `SceneDesc`; `FrameRing` out of `VulkanRenderer`;
`ShadingCache` out of `CompositeQueue`. Three were refused:

- **`SceneUploader::hand`.** Where the gather, the collect and the describe sit relative to the
  upload *is* the decision the type exists to make. Moving the queue and the loader out puts that
  ordering in every caller — the game, the harness, the doll and the map.
- **`StagedWorld`.** `moveTo` already delegates both halves to free functions in `cellscene`; what is
  left is the order it does them in, and a `Streaming` type would be six references bundled to
  reproduce one method.
- **`ReadbackQueue`.** There is no queue — three forwarding calls into `Image::read` and
  `GuiTextures::read`, with no pending list and no staging ring.

**4. Ownership and arguments.** Nothing here was ever cycles: every item was an argument that
travelled, a reference held twice, or a fact told more often than it changed. Six landed and removed
24 parameters — `Rtx::MirrorPass` for the walk's stamp and counts, `Rtx::Placing` for a placement's
four, `RtxTool::RegionRequest` and `RtxTool::SkyMoment` for the harness's, `Traversals` held by value
in `OffscreenTrace`, and `TracedView` asking its owner for the backend. Four directions were wrong:

- **The Vulkan four do not always travel together.** Five pass records carry commands and a timer
  with no slot and no graveyard; three placements carry all four; two batched builds carry `Batch&`,
  a timer and a graveyard and record through the `Batch`. `Placing` is given to the placements alone
  — one context over all ten would hand seven signatures two fields they never read.
- **Always-owned `Traversals` would cost 79 edits to save twelve bytes.** It is one `unsigned int`,
  and 79 of the 81 places that construct a `SceneExtractor` pass none.
- **A settled sky is not a transition of no length**, so the two `relight` overloads stay two.
  `Sky::cloudBlend` answers a *full* crossing for a weather the content gives no
  `Clouds_Maximum_Percent` — ash and blight — so a settled sky asked as a blend of nothing reports a
  deck that has crossed. `clouds.cpp` pins `cloudBlend(0, 0) == 1`, which is the assertion that
  records it.
- **`WorldState` does not want lifting**, which is 4.5 below.

## Still open from section 2

Measured shares are from `profile.sh --view=vivec`, three runs a leg.

- **`SceneDesc::orderLights` sorts on a nine-field tuple.** 0.12%. A `std::uint64_t` folded from the
  position and the intensity as the light is added would sort in one compare, and would make the
  order a fact about a light rather than a rule spread across a comparator. Do it when `Light` is
  next touched.
- **`SceneBuffers::place` converts every light, sprite and emitter each frame.** 1.57%. Making it
  incremental needs a per-frame identity for a light, which the walk does not carry. That is a change
  to what a light *is*: not before the renderer draws everything.
- **`DistantLights::collect` does (2r+1)² map lookups a frame.** 0.01%, and the finding is the
  container rather than the cost — see 5.7.

## Still open from section 4

### 4.5 `CellLighting` and `Rtx::WorldReading` — refused

Not a duplication. `Rtx::WorldReading` is already the shared answer, both hosts build one, and
`frameworld.hpp` says so: the game reports what a live weather system settled and the harness derives
it from content files at an hour it was told. Every piece of arithmetic under them is already in
`components/rtx/`. What differs is the assembly, because the inputs do — `readWorld` reads forty
fields a weather system, a `FogManager` and `configureAmbient` settled, and the harness has an hour
and a weather name. Lifting `WorldState` would hand the harness a type it still could not fill, in
exchange for moving a header the rasterizer reads.

### 4.8 Residents receive per-cell facts every frame

`WorldMirror::mirror`. Five setters on `DistantLights` and two on `TerrainResidency` per frame, and
`SceneExtractor::follow` reassigns per frame. The values change per cell, not per frame.

**Measured at nothing** — all seven are inline assignments and `DistantLights::follow` compares two
members and returns — so this is shape and not cost. Direction, when something else touches the
residency: one `Viewpoint { eye, grid, outdoors }` set per frame, with `follow` and `setReach` run
when the world changes.

### 4.4 `measurePlace` still takes seven arguments with three out-parameters

`apps/rtxtool/bench.cpp`. `BenchRun` already exists and can carry `samples` and `pixelScratch`.

### 4.9 `RendererOptions::mWindow` is a raw `SDL_Window*`

`components/rtx/renderer.hpp`. Documented as deliberate. Leave.

## 5. Non-canonical data structures

**Investigated. `.notes/structures.md` is the design proposal and the plan.** It measures the whole
section and finds the list ranked wrong: three `std::unordered_map`s cost 4.4% of the harness's CPU
at Vivec and two of them are not named here, while eight of the nine items below cost 0.02% between
them. It also corrects 5.4's and 5.5's directions, and finds 5.8 needs no go-ahead.


### 5.1 Texture slots are seven parallel structures

`components/rtx/texturetable.hpp`. They are one type now, with the invariant stated in one place —
but they are still seven parallel structures inside it: `mPaths`, `mBaked`, `mRefs`, `mFree`,
`mChanges`, `mPathIndex` and `mBakedIndex`. Two own a string per slot, and the two maps own a second
copy of each string as their key.

Direction: one `TextureSlot` row (kind, name offset into one string arena, refs) and one map keyed on
a view into the arena.

### 5.2 A per-slot flag has two spellings left

Two left. `SlotChanges` took the third: the mesh table and the texture table share one
`std::vector<SlotNews>` through it. What remains is `std::vector<char>` (`mKeptMeshes`,
`mKeptMaterials`, `mDeformedFlags`) beside `std::vector<std::uint8_t>` (`mMaterialWritten`), all in
`scenedesc.hpp`.

Direction: one type for a per-slot flag. `mMaterialWritten` is the odd one — it is a list kept
duplicate-free, which is `SlotChanges` restricted to arrivals, except that a material row is
*written* rather than arriving or going.

### 5.4 `mPlacements` is keyed on a derived hash

`SceneExtractor::mPlacements`, an `unordered_map<std::size_t, Known>` keyed on a hash of the path
and the drawable address. The sweep is an `erase_if` over the map. A collision silently merges two
placements.

Direction: key on the placement identity itself, or keep the row in the instance table and let the
sweep read the epoch there.

### 5.5 `GpuBreakdown` is two parallel vectors

`components/rtx/frametimes.hpp:137-140`. A `vector<string>` beside a `vector<vector<double>>`.

Direction: one `vector<double>` with a stride. Names as `string_view` into the pass table.

### 5.6 `CompositeQueue` keeps node containers

`components/rtx/compositequeue.hpp:178-179` (`std::deque`), `:186` (`unordered_map<Index,
TerrainComposite>`). The third — the estimate keyed by file — is now `ShadingCache`'s, and it is
node-based on purpose: a `CompositeLayer` spans into it while the bake reads.

Direction: `mPending` and `mDone` as ring buffers over the request pool the queue already keeps.
`mFinished` as a vector indexed by material slot.

### 5.7 `DistantLights::mCells` is a `std::map`

`components/rtx/distantlights.hpp:114`. A flat grid indexed by cell offset. The lookups cost 0.01%,
so this is the container and not the cost.

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
