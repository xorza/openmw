# Redesign: what remains of the upstream-diff review

The items are in `.notes/review-upstream-diff.md`. This document is the code behind each item, the
structural answer, and the order to build it in. Delete a phase here and its item there when it
lands.

Two items are sweeps and not designs: the comment pass over the RTX places, and the pass that
separates a lift from an edit in ~130 upstream files. They stay in the review document and are not
covered here.

## 1. Pictures inside the interface ride the frame

### What the code does

`VulkanRenderer::traceGuiTexture` drains the queue twice per picture: `mRing.finishAll()` before it
records, and `mPool.submitAndWait` after. Measured at 2.5–12 ms per `TracedView::redraw` against a
frame of about 5.5 ms. A doll pays it on every equipment change and on every frame of a race
slider drag; a cell entry pays it once per map tile, three on a crossing and nine on a fresh load.
The global map's copy (`readGuiTexture`) is a third drain per explored cell.

Each wait covers a real hazard. Every one has a structural answer already present in the code:

| Hazard | Why the wait covers it today | What answers it instead |
| --- | --- | --- |
| A picture of the world bins sprites into the world's `Tables[mWorldSlot]`, which the frame in flight reads (`SceneBuffers::binSprites` writes `mSprites` from the host at record time). | `finishAll` | The camera's cull mask reaches the trace (§1.3). The local map's has no particle bit, so the tile bins nothing; the field is ignored today. |
| A picture's hits land in `mRing.recording().mHitCount`, the frame's counter. | Zeroed by `renderFrame` after the picture | A counter of its own that nothing reads (§1.4). |
| A subject scene (a doll) has one copy of every table; a placement rewrites what the last trace may still read. | `finishAll` | The world's slot discipline — `sFrameSlots` copies, a current `FrameSlot`, `mReadBy` per copy — moved onto `ViewScene` so every scene has it (§1.1). |
| The picture must reach the GUI texture before the GUI draw samples it. | `submitAndWait` | The recording is a `Batch` and `defer()`s. The GUI draw is the next pool submit and carries it in order (§1.2). `GuiTextures` already does this for uploads. |
| `readGuiTexture` needs the write finished. | `Image::read` is a `submitAndWait` | The copy to a host-readable buffer is recorded at the end of the same batch; the read answers "not yet" until the carrying frame's fence has been waited (§1.5). |
| A world view recorded, then the slot it read rewritten by a placement before the batch is submitted. | Impossible today: the trace is synchronous | The game records pictures only between `handOver` and `trace`, where the frame's slot is final (§1.6). The harness records them after the frame and then flushes (§1.7). |

### 1.1 One slot discipline for every scene

`mWorldSlot` and `mReadBy` are renderer members and apply to the world alone; `setScene` builds a
view scene with `slots = 1`, and `placeScene` has an `if (!slot.isWorld())` branch with a discipline
of its own ("one copy, traced and waited for once").

- `ViewScene` gains `FrameSlot mSlot;` and `std::array<std::uint64_t, sFrameSlots> mReadBy;`.
  The renderer's two members go.
- `setScene` builds every scene with `sFrameSlots` copies. `SceneAcceleration`, `SceneBuffers` and
  `SkinTables` already take the count.
- `placeScene` is one path: `into = held.mSlot.next(); mRing.finishThrough(held.mReadBy[into]);
  record; held.mSlot = into;`. The world's placement keeps its own submit (`takePlaceCommands`,
  `mPool.submit` without a fence) for now; a view's placement is a deferred `Batch` as today. The
  only branch left is that submit. Folding the world's placement into a deferred batch too is a
  follow-up gated on `bench` (`place ms`, frame ms): it would delete `FrameRecord::mPlaceCommands`
  and `takePlaceCommands`. `SceneUploader::hand` places once per call, so the "placed more than
  once" reason on `mPlaceCommands` no longer holds; assert `frame.mPlacements <= 1` while building
  this and delete the vector if it never fires.
- Every trace — the frame's and a picture's — stamps `held.mReadBy[held.mSlot] =
  mRing.getRecording()`. A deferred batch rides the next pool submit, which is either that frame's
  or an earlier one's GUI submit; a later frame's fence covers both by queue order, so the stamp is
  conservative and sound.
- `describeInputs(traced, slot, …)` reads `traced.mSlot`; the `isWorld() ? mWorldSlot : FrameSlot{}`
  expression goes.

Memory: a doll's scene doubles its instance, material, light, sprite and skin tables — a single
NPC, kilobytes.

### 1.2 A picture is a deferred batch

`traceGuiTexture` records into `Batch trace(mPool)` and ends with `trace.defer()`. No `finishAll`,
no `submitAndWait`. The graveyard is `mRing.recording().mWorld.mGraveyard`, as today.

Ordering inside one submit is already handled: `TraceChain::record` transitions `mColour` and the
target from `UNDEFINED` with an `ALL_COMMANDS` source scope; `GBuffer::begin` does the same for the
channels; `VisibilityPass::writeConstants` is a `vkCmdUpdateBuffer` with barriers both ways; the
tone pass pushes descriptors. The only host write at record time in the whole chain is
`binSprites`, which §1.3 removes for a camera without the particle bit and §1.1 makes safe for a
subject.

Several pictures in one frame each take a batch; they run in the order recorded.

### 1.3 A camera's cull mask reaches the trace

**Root cause.** The rasterizer answers "what does this camera draw" with the cull mask: every node
on a path states a mask, and a camera draws what every stated mask on the path intersects.
`LocalMap` hands `Mask_Scene | Mask_SimpleWater | Mask_Terrain | Mask_Object | Mask_Static` — no
actors, no player, no effects, no particles, no first-person arms. The RT path collapsed that
question into one camera's answer: the frame's eye. `SceneExtractor` classes a node by its mask
for exactly one class — `isFirstPerson` (`carriesOnly(mask, mFirstPersonMask)`) — carries it as
`MeshInstance::mFirstPerson`, `InstanceRecord` turns it into the instance-mask bit
`MASK_FIRST_PERSON`, and `visibility.rgen` casts the eye's rays with a constant `EYE_MASK =
MASK_SOLID | MASK_WATER | MASK_FIRST_PERSON`. A second camera has no way to state a mask, so
`OffscreenViewSpec::mMask` is ignored for a picture of the world and a map tile shows the rain,
the smoke, every NPC and the player. That is the image bug in `.notes/ISSUES.md`, and the same
missing answer is why the bin into the frame's sprite tables exists at all.

The mechanism is already there for one class. The fix is to make it the general one, so that both
renderers read the one mask.

**Instance classes.** `MeshInstance::mFirstPerson` becomes `InstanceClass mClass` — `Static`
(default; the ground and the distant statics never state one), `Actor`, `Effect`, `FirstPerson`.
Water stays a material fact (`worn.mKind`), medium stays a material fact. `InstanceRecord::mMask`
carries one class bit, plus `MASK_WATER` in place of the class for water, plus `MASK_MEDIUM` beside
it as today:

```
MASK_STATIC       0x01   (today's MASK_SOLID)
MASK_WATER        0x02
MASK_FIRST_PERSON 0x04
MASK_MEDIUM       0x08
MASK_ACTOR        0x10
MASK_EFFECT       0x20
MASK_PARTICLE     0x40   a camera bit, never on an instance: whether the sprites are binned
```

Seven of the eight bits `VkAccelerationStructureInstanceKHR::mask` has. `Mask_Actor` and
`Mask_Player` are one class: no camera in the engine draws one without the other, and a bit for
the split is the last one.

**The walk.** `SceneExtractor::setFirstPersonMask` becomes `setClassMask(InstanceClass,
NodeMask)`; `WorldMirror` states `Actor = Mask_Actor | Mask_Player`, `Effect = Mask_Effect`,
`FirstPerson = Mask_FirstPerson`. `MirrorTraversal` keeps the innermost stated class on the path
where it keeps `mFirstPerson` today (saved and restored around `descend`, as `mPathHash` is), and
`addDrawable` hands it on. `carriesOnly` is the test, for the reason it gives: a node mask's default
is all ones, so a class is what a node *states*, not what it happens to include.

The innermost stated class stands for the path. The rasterizer culls on every stated mask along
it, so the two differ for a class stated under another that a camera includes while excluding the
outer one — an effect under an actor, drawn by a camera with `Mask_Effect` and without
`Mask_Actor`. No camera in the engine has that mask. Stated on `InstanceClass`.

**The camera.** `Shaders::VisibilityConstants` gains `uint mRayMask`. Eye rays cast with it
(`EYE_MASK` goes); every other ray derives from it — `mRayMask & ~MASK_FIRST_PERSON` for shadow,
bounce and probe rays (the arms are met by the eye alone, as today), medium rays `MASK_MEDIUM`
alone (unchanged). The sites are `visibility.rgen`, `traversal.glsl` (3), `hitstage.glsl`,
`shading.glsl`, `water.glsl`, `fog.glsl`. One word read per ray, the same for every lane.
Shadows follow the mask too: a camera that does not draw the actors does not draw their shadows,
which is what "draw" means to a ray tracer and what the flat-lit map has anyway.

`binSprites` runs when `mRayMask & MASK_PARTICLE`; otherwise `TraceRecording::mSpriteBin` is
null, `TraceChain::record` skips the bin, and `describeInputs` binds a two-word device buffer
`{ SPRITE_LIST_UNBINNED, 0 }` — the shader reads `spriteTileListAt(0) == SPRITE_LIST_UNBINNED`
then walks `[0, spriteTileListAt(1))`, so this is an empty list at every extent. Written once at
construction. This is what removes the world-view bin into the frame's tables (the first row of the
table above).

**The game side** owns the translation, in `apps/openmw/mwrender/rtx/raymask.{hpp,cpp}`:
`Rtx::Shaders::RayMask rayMaskOf(osg::Node::NodeMask cullMask)` — `Object | Static | Terrain |
Groundcover → STATIC`, `Actor | Player → ACTOR`, `Effect → EFFECT`, `FirstPerson → FIRST_PERSON`,
`Water | SimpleWater → WATER`, `ParticleSystem | WeatherParticles → PARTICLE`, `MEDIUM` always.
`RtxRenderer::aim` fills the frame's from `mStage.getCamera().getCullMask()`, so the frame follows
whatever the game decides the eye sees, the way the rasterizer does. `TracedView::makeTrace`
fills a picture's from `spec.mMask` and hands it to `OffscreenTrace`, whose two constructors take
a `RayMask`; `describeCamera` writes it. `makeCamera*` defaults to every class, so the harness's
`shot`/`bench` and every test are unchanged until they ask.

`OffscreenViewSpec::mMask`'s comment and the one in `localmap.cpp` are rewritten: the mask reaches
the ray tracer, and how.

**Tests.** `extractor/` tests for the class on the walk (an actor root, an effect under it, a
static, nothing stated). `instancerecord.cpp` for the bits. A GUI trace test with a scene holding a
static and an actor: the frame's mask shows both, `STATIC` alone shows one. The sprite test in §1's
list.

### 1.4 A picture counts into nothing

`Buffer mViewCounts` — `sizeof(FrameCounts)`, device local, never read. `TraceRecording::mCounts`
points at it for a picture. The frame's count then excludes pictures without depending on the
order the two were recorded in, and the "zeroed here and not where the frame opened" reason in
`renderFrame` goes.

### 1.5 The copy rides the batch

- `GuiTraceOptions` gains `bool mReadBack = false`. `GuiTextures` keeps, per slot, a
  `Buffer::staging` made on the first trace that asks, sized to the texture, and
  `std::uint64_t mTracedOn`.
- After `writeWith`, a trace with `mReadBack` records `sample → copyRead`,
  `vkCmdCopyImageToBuffer` of the whole texture, `→ sample`. The read is then a `memcpy` off the
  mapped buffer.
- `GuiSurface` gains `bool takeGuiCopy(GuiSlot, std::span<std::uint8_t> into)`: false until
  `mRing` has finished `mTracedOn` (or `finishGuiTraces` has run since the trace), then the copy.
  It never waits and never submits. `readGuiTexture` stays as the synchronous read for tests.
- `GuiSurface` gains `void finishGuiTraces()`: `mPool.finishDeferred()` and mark every pending
  slot landed. For the harness and the tests; the game never calls it.
- `TracedView::redraw` sets `mReadBack = mKeepCopy`; `getCopy()` calls `takeGuiCopy` straight into
  `mCopy->data()` when `!mCopyIsCurrent`. `mPixels` and the `memcpy` go (the "double copy" item).
  `OffscreenView::getCopy()` loses `const` — for the tracer it is a pull. `GlOffscreenView` follows.
  `LocalMap::getMapImage` already polls: "the first ask starts it and comes back with nothing, and
  the caller asks again".

### 1.6 The game draws pictures in the frame's window

`RtxRenderer::traceWorld` already calls `drawDeferredViews()` between `handOver` and `trace`, for
the pictures asked before there was a world. Every picture goes that way:

- `OffscreenView::redraw()` on a `TracedView` only queues: `mHost.redraw(*this)`. The work moves to
  `TracedView::draw()`, which the host calls. `hasScene` is checked by the host before it flushes,
  so a view never re-queues itself.
- `RtxRenderer::drawViews()` (the renamed flush) runs where `drawDeferredViews` runs. It draws every
  subject view, then at most `sWorldViewsPerFrame` world views (a constant, start at 3: a
  crossing's row of tiles in one frame, a fresh load's nine over three). The rest stay queued in
  order. Frames cost the same as each other; a load is not a frame that may cost nine tiles.
- `ViewHost` gains `void flushRedraws()` for the harness (§1.7) and loses `deferRedraw`
  (`redraw` is the one entry). `forgetView` stays.
- The frame's CPU spend on pictures gets a row: `Timing::Views`, timed around `drawViews()`. It is
  the number that says whether this section did what it claims, in `bench` and in the game alike.

In this window the world's slot is final for the frame, so a world view reads what the frame
reads, and the frame's own `mReadBy` stamp covers the batch. A frame that traces no world
(`aim` refused, no placements) still submits the GUI, which carries the batches.

### 1.7 What still drains, and why

- **The view chain grows.** `growViewTargets` recreates `mView`'s images and `mViewTarget`; a
  batch deferred or in flight may name the old ones. Before a grow: `mPool.finishDeferred();
  mRing.finishAll();`. This is the first picture of a new size — a handful of times a session.
- **`dropViewScene`** keeps `waitIdle; finishAll; emptyGraveyards` and adds `mPool.finishDeferred()`
  first, because a doll can be traced and closed in the same frame. Once per inventory close.
- **`setScene`/`extendScene` on a view scene** keep their `finishAll` (an arrival writes the
  single-copy attribute tables) and add `finishDeferred()` before it. Once per equipment change
  that brings a new mesh, not per redraw.
- **The harness.** `StopWriter::writeView` and `writeMapTile` run after the frame's submit and
  need the picture now: `mHost.flushRedraws(); mBackend.finishGuiTraces();` then `getCopy()`. A
  stop is where a drain belongs. `FrameContext` gains `ViewHost& mHost`.

### Interface changes

- `Rtx::GuiTraceOptions`: `+ bool mReadBack = false;`
- `Rtx::Shaders::VisibilityConstants`: `+ uint mRayMask`; `MeshInstance::mFirstPerson` → `mClass`;
  `SceneExtractor::setFirstPersonMask` → `setClassMask`; `OffscreenTrace` constructors take a
  `RayMask`.
- `Rtx::GuiSurface`: `+ bool takeGuiCopy(GuiSlot, std::span<std::uint8_t>);`
  `+ void finishGuiTraces();`
  `readGuiTexture` unchanged.
- `MWRender::OffscreenView::getCopy()` non-const.
- `MWRender::ViewHost`: `redraw(TracedView&)` replaces `deferRedraw`; `+ flushRedraws()`.
- `MWRender::FrameContext`: `+ ViewHost& mHost`.
- `Rtx::Timing::Views` row and its name in the report.
- `CountingRenderer` in the tests follows the interface.

### Tests (`apps/components_tests/rtx/guitextures.cpp`, `offscreentrace.cpp`)

- A traced picture is not readable before `finishGuiTraces` and is after it (`takeGuiCopy`).
- Two pictures traced into two slots in one frame each hold their own.
- A frame's hit count is the same with and without a picture traced before it.
- A scene with one sprite: the frame shows it, a picture of the world does not, a picture of a
  subject scene holding the same sprite does.
- A subject scene placed and traced on two consecutive `renderFrame`s without a finish between
  gives two correct pictures (the slot discipline).
- The existing trace tests call `finishGuiTraces()` before they read.

### Gates

`components-tests --gtest_filter='Rtx*'`; `openmw-rtxtool check`; `doll` and `map` verbs write
the same PNGs as before; `repeatable.sh --pairs=10` identical on the scene columns; `bench` at
`seyda-neen-ship` and one crossing route with the `views ms` row beside `frame ms`, before and
after, on a hot card, interleaved.

## 2. Placements evaluated by prefix

### What the code does

`CellRing::walkRings` calls `CellPlacer::place` for every held cell every frame, and `place`
evaluates every placement: `placement.mRadius² >= threshold²` and `isDisabled` (a binary search)
per reference. Tens of thousands per frame, for a set that changes by a few entries when the eye
moves and by none when it stands.

### Design

- `HeldCell::mPlacements` is sorted by `mRadius` descending once, at adopt (`CellRing::adopt`
  already builds the vector; sort in place, no allocation). `HeldCell` gains `std::size_t mShown`,
  the length of the placed prefix.
- `place`: `threshold²` as today; `wanted = std::partition_point(begin, end, radius² >= threshold²)`
  — one `log n` per cell — then only `[mShown, wanted)` is added and `[wanted, mShown)` dropped.
  The set is identical to today's; nothing is approximated.
- Disabled references leave the per-frame path. `Placement` gains `bool mDisabled`, set at adopt
  from `mDisabled` (the sorted vector stays for cells not yet held) and flipped by
  `CellRing::setReferenceEnabled`, which walks the held cells for the refnum and adds or drops
  that one slot at once. The prefix walk skips a disabled placement when adding; a disabled
  placement inside the prefix has no slot.
- The three "drop the ground slot" / "drop a placement slot" blocks in `dropGround`, `dropSlots` and
  `place` become `dropSlot(HeldGround&)` and `dropSlot(Placement&)`.

### Tests (`apps/components_tests/rtx/cellring.cpp`)

- A cell with radii 5, 4, 3, 2, 1 at a threshold that admits three: exactly the three largest are
  placed; move the eye so the threshold admits four: one add, no drops; back: one drop.
- Disable a reference inside the prefix: dropped that call; enable: placed that call; disable one
  outside the prefix: nothing changes; then move the eye so it enters the prefix: still not placed.
- Adopt after a disable: the placement arrives disabled.

## 3. Small items, each with its answer

- **`Sky::timescaleClouds()` per frame** (`renderingmanager.cpp:821`). Upstream read
  `Weather_Timescale_Clouds` once into `SkyManager::mTimescaleClouds`. The roll owns the policy:
  `explicit SkyRoll(bool timescaleClouds)`, `advance(seconds, cloudSpeed, timeScale)`; the static
  `after(…)` keeps its fourth argument and builds the roll with it. `RenderingManager` constructs
  `mWorld.mSkyRoll` with `Sky::timescaleClouds()` once.
- **`getInstancePtr()` per frame** (`rtxrenderer.cpp` `updateTraversal`, `drawGui`).
  `createGuiPlatform` makes the `MyGUIRtx::RenderManager` and can keep `mGui` before it moves the
  pointer into the platform. `Engine` destroys the window manager before the renderer and runs no
  frame between, so the null checks become an assert.
- **`CompositeQueue::mQueuedAt`** is a deque kept in step with `mPending`/`mDone` by position.
  `Request` gains `std::size_t mQueuedFrame`; `collect` reads `mDone.front().mRequest.mQueuedFrame`
  where it read `mQueuedAt[due]`. The frames are monotonic in the sequence, so the "due" count is
  the same loop over the done queue. The deque goes.
- **`GlRenderer::resetPreparationBudget`** allocates an `IncrementalCompileOperation` to read two
  defaults, and restores the defaults where upstream restored the previous values
  (`mOldIcoMin`/`mOldIcoMax`). `setPreparationBudget` saves the current pair into
  `std::optional<PreparationBudget> mRestingBudget` when it is empty; `reset` writes it back and
  clears it. No allocation, and upstream's semantics.
- **`FrameCapture::thumbnail`** builds RGBA at the thumbnail size and copies to RGB per pixel.
  `Rtx::frameImage` takes the channel count (3 or 4) and writes the format asked for. One pass.
- **`nodekind.hpp`** keys on `libraryName()`/`className()` pointer identity. It is a cache key and
  not a correctness assumption: two pointers that are equal name one class; a class that answers
  with a second literal misses, goes through `learn` (the `dynamic_cast` ladder), and takes a
  second row or a count in `getOverflow()`. One paragraph on the class says so; nothing else
  changes.
- **`SceneDesc` write facade.** Seventeen methods forward one call to one table (`addRig`,
  `addMorph`, `addMaterial`, `hold*`/`drop*` ×6, `addMask`, `addLayers`, `addTexture`,
  `addBakedTexture`, `fadeInstance`, `moveInstance`, `dropInstance`, `advancePlacement`).
  `SceneDesc` exposes `meshes()`, `materials()`, `textures()`, `placements()`, `deformers()` as
  writable references and keeps only what crosses tables or the scene's own vectors: `addMesh`,
  `addInstance` (their asserts), `poseRig`/`poseMorph`, `setMaterial`, `hasDroppedHolds`,
  `release`, `clearPlacement`, `clearArrivals`, `addLight`, `addEmitter`, `orderLights`. Callers
  spell `mScene.meshes().hold(x)`. Low value; last.
- **`rtxtool/main.cpp` `parseSize`** returns a `std::pair`. A `struct Size { std::uint32_t mWidth,
  mHeight; }` beside it.

## 4. Closed without a change

- **`nifloader.cpp` threads `Surface::Material&` through ~15 signatures.** Upstream threads
  `boundTextures` and `animflags` through the same signatures the same way, because the property
  handlers do not take `HandleNodeArgs`. The material rides exactly where `boundTextures` rides.
  The one consolidation — a `SurfaceSoFar { boundTextures, material }` bundle in the
  `boundTextures` parameter's place — touches every signature it would save a parameter on, and
  renames upstream's parameter in each. Diff-neutral; not done. Delete the item.

## 5. Order of work

Each phase ends with its own build, its filtered tests, and the format gate. Phase 1 ends with the
full gate list in §1.

1. **§1.1** slot discipline on `ViewScene`. `check`, `repeatable.sh --pairs=2`. Nothing visible
   changes; the drains are still there.
2. **§1.3, §1.4** no sprites for a world view, the view counter. `map` verb PNG: the rain is gone
   and nothing else moved. `check` hit fraction unchanged.
3. **§1.5** read-back in the batch, `takeGuiCopy`, `finishGuiTraces`; tests updated to
   `finishGuiTraces`. Still synchronous at this point (the drains stay), so each step is
   separable.
4. **§1.2, §1.6, §1.7** the batch defers; the game draws in the window; the harness flushes;
   `Timing::Views`. Full §1 gates. Then `--pairs=10`.
5. **§2** placements by prefix. `check`, `repeatable.sh --pairs=10` (the placed set must be
   identical on every frame), `bench` `walk ms` before and after.
6. **§3** in the order listed. `timescaleClouds` and `resetPreparationBudget` touch
   `renderingmanager.cpp` and `glrenderer.cpp`, which are already edited upstream files; both edits
   shrink the diff against upstream (a per-frame read back to a once-read, a restore back to
   upstream's restore).
7. Delete the addressed items from `.notes/review-upstream-diff.md`, and this file when it is
   empty.
