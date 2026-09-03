# Section 4 of the review, investigated

Every item read again in the code. **Three of the nine directions are wrong or overstate what is
there**, and those corrections are first, because two of them would have made the tree worse.

## What this section can and cannot buy

**There is nothing to measure here.** Section 2 was cycles and section 4 is not: every item below is
an argument that travels, a reference held twice, or a fact told more often than it changes. Where a
cost was claimed, the investigation checked it and found none — 4.8's seven per-frame setters are
inline assignments and `DistantLights::follow` already returns early when nothing changed.

So the measure is what a reader has to hold in their head: signatures shortened, ownership questions
closed, and one type stated once instead of twice. Nothing here will move a frame time, and an item
that claims it would is wrong.

## Corrections

**4.3's premise is false. The four do not always travel together.** `VkCommandBuffer`, the slot,
`GpuTimer*` and `Graveyard&` fall into three groups:

| group | signatures | what they carry |
| --- | --- | --- |
| pass records | `SkinPass::record`, `VisibilityPass::record`, `SpriteBinPass::record`, `SceneAcceleration::recordRefit` and `recordTopLevel` | commands and timer — no slot, no graveyard |
| placements | `VulkanRenderer::recordPlacement`, `SceneAcceleration::place`, `SceneBuffers::binSprites` | all four |
| batched builds | `SceneMicromaps::bake`, `SceneAcceleration::buildArrived` | `Batch&`, timer and graveyard — no commands, no slot |

A single `FrameContext` fits three of ten sites and would hand the other seven two fields they never
read. The third group already has its context and it is called `Batch`: it supplies the commands, so
a struct carrying a command buffer beside it would be two answers to one question.

**4.6's direction costs 79 edits to save twelve bytes.** `Traversals` is one `unsigned int`. There
are 81 places that construct a `SceneExtractor` and **79 of them pass no traversals** — 78 extractor
tests and `RtxTool::StagedWorld`. Only `OffscreenTrace` and `WorldMirror` pass one. Making the caller
always own one means a `Traversals traversals;` line in each of the 79, to remove one member and one
branch from two classes. Refused below; what is
real in the item is smaller and is proposed instead.

**4.7 is half already done.** `createOffscreenView` returns a `std::unique_ptr<OffscreenView>` and
`MWRender::CharacterPreview` and `LocalMap` hold it — the caller already owns the view, and
`~TracedView` already reports to the owner through `forgetView`. The renderer's `mDeferred` and
`mDrawing` are observer lists and were never owners. What is left of the item is one reference.

## Proposals

### P1. `addLight` stops taking a path it never reads

`SceneExtractor::addLight` declares `const osg::NodePath& path` and never touches it. The caller
passes `getNodePath()`, which is a reference to the visitor's own vector — so **this costs nothing at
runtime**, and the whole of the finding is that the signature makes a claim the body does not keep.

One parameter and one argument. Risk: none.

### P2. The walk's per-pass state is one thing, borrowed once

`ExtractionStats&` is threaded through eleven signatures across four headers — `sceneextractor`,
`meshresolver`, `materialresolver`, `emitterresolver — and `MirrorTraversal` keeps an `mStats` of its
own beside them.

The resolvers already borrow `const std::uint64_t& mEpoch` from the mirror, for the reason each one
states: a copy per resolver is one fact stated four times and free to fall behind. **The stats are
the other half of the same fact** — what this pass is, as against what the last one was.

```cpp
/// What one walk is: which sweep stamps it, and where its counts go.
///
/// **Borrowed by every resolver and by the traversal**, so a pass has one state rather than five
/// copies that can disagree. `mStats` is null between walks, which is what says a resolver was
/// reached outside one.
struct MirrorPass
{
    std::uint64_t mEpoch = 0;
    ExtractionStats* mStats = nullptr;
};
```

`SceneExtractor` holds it; `walk` sets `mStats` and clears it on the way out; each resolver and the
traversal take `MirrorPass&` where they took `const std::uint64_t&`. Eleven parameters go, and
`MirrorTraversal::begin` loses one too.

**A walk is already not re-entrant** — `mAnchor` is a member set per walk — so one pass at a time is
the contract this rests on, and a `debug_assert` on `mStats` states it.

Risk: low. The mistake it invites is a resolver reached outside a walk, which the assert catches on
the first frame.

### P3. A frame context for the placements, and only for them

The three signatures in the placement row above carry the same four and read all four:

```cpp
/// Where a placement records, which copy it writes, what times it and what it may bury.
struct Placing
{
    VkCommandBuffer mCommands = VK_NULL_HANDLE;
    std::uint32_t mSlot = 0;
    GpuTimer* mTimer = nullptr;
    Graveyard& mGraveyard;
};
```

`recordPlacement` goes from seven arguments to four, `SceneAcceleration::place` from eight to five,
`SceneBuffers::binSprites` from eight to five. The other seven signatures are left alone, for the
reason in the correction above.

**And one free fix beside it.** `binSprites` takes `Graveyard&` then `GpuTimer*`; every neighbour
takes them the other way round. The types differ so the compiler catches a crossed call, but a
reader has to check.

Risk: low.

### P4. A sky moment, which four signatures already carry by hand

`loadRegion` takes ten arguments. They are not ten things:

- `world, centre, root, loaded, liveProps` — **exactly `readRegion`'s five**, which it calls;
- `scene, extractor` — the mirror to load into;
- `weather, day, hour` — when the sky stands.

The third group is not `loadRegion`'s alone. `relight` has two overloads — one weather, or two and a
blend — and `StagedWorld::setSky` has the matching pair. That is **four functions carrying the same
triple by hand, and two of them exist only because a transition names two weathers.**

```cpp
/// When a sky stands: the weather, what it is turning into, and the date and hour.
///
/// **One type rather than an overload for the transition.** A weather that is not turning names
/// itself on both sides at a blend of nothing, which is what the deck already does.
struct SkyMoment
{
    std::string_view mWeather;
    std::string_view mNextWeather;
    float mBlend = 0.0f;
    int mDay = 0;
    float mHour = 12.0f;
};
```

`relight` and `setSky` become one function each instead of two. `loadRegion` becomes
`loadRegion(const RegionRequest&, Rtx::SceneDesc&, Rtx::SceneExtractor&, const SkyMoment&)` — four
arguments, where `RegionRequest` is `readRegion`'s five named once.

`measurePlace`'s seven with three out-parameters is a separate item and the review's reading of it
stands: `BenchRun` already exists and can carry `samples` and `pixelScratch`.

Risk: low, and the transition collapse is the part to test — a blend of nothing has to give what the
single-weather overload gave.

### P5. `WorldState` lifts into `components/`, and the two hosts stop describing it twice

This is the one item with a real seam behind it, and the investigation says it is **buildable with no
upstream edit at all** — which the review doubted.

`MWRender::readWorld` now proves the translation is one function. It cannot be shared because
`WorldState` lives in `apps/openmw/mwrender/sceneframe.hpp`, so the harness reimplements it as
`CellLighting` plus `applyLighting`.

What the lift needs:

- `WorldState` is 197 lines of plain scalars, `osg` maths, `Sky::SkyRoll` and `Weather::Precipitation*`
  — both already in `components/` — and three fork-local satellites: `Location` and `FogBand` from
  the same header, and `MoonState` from `weatherresult.hpp`.
- **Every one of those files was added by this fork.** `sceneframe.hpp`, `renderer.hpp` and
  `weatherresult.hpp` are not in the merge base.
- `WorldState` is *filled* by `RenderingManager::describeWorld`, which **is** an upstream file, and
  read by `gl/postprocessor.cpp`, which is the rasterizer's.

So the lift is: move the four types into `components/`, and leave `using WorldState = ...;` in
`sceneframe.hpp`, which is the fork's own seam header. **`renderingmanager.cpp` and
`postprocessor.cpp` then compile unchanged** — the rasterizer reads exactly what it read before,
which is the condition the fork's rules put on a lift.

Then `readWorld` moves to `components/` beside it, and `CellLighting` holds a `WorldReading` plus the
three fields that are the harness's alone: the sea's seconds, the water level and the rain.

Risk: **the highest here**, and it is the only item that touches what the rasterizer reads. The alias
is what keeps that to nothing, and `verify` is what proves it.

### P6. `OffscreenTrace` stops heap-allocating four bytes

`SceneExtractor` holds `Traversals mOwnTraversals` by value; `OffscreenTrace` holds
`std::unique_ptr<Traversals>` for the same four-byte counter. Make the second match the first.

One line, no call site moves. This is what is left of 4.6 once its direction is refused.

### P7. `TracedView` asks its owner for the renderer

`mRenderer` is used exactly once — `mRenderer.readGuiTexture(mSlot, mPixels)`. Two references to
things whose lifetimes are one thing is two ways for the pair to disagree; an accessor on the owner
makes it one.

Risk: low.

### P8. The residency is told what changed, when it changes

`WorldMirror::mirror` calls five setters on `DistantLights`, two on `TerrainResidency` and
`SceneExtractor::follow` every frame, for values that change per cell.

**Measured at nothing.** All seven are inline assignments and `DistantLights::follow` compares two
members and returns. So this is shape and not cost: what a reader sees is seven per-frame calls
that look like per-frame facts.

Do it when something else touches the residency, and not before. The review's `Viewpoint { eye, grid,
outdoors }` is the right shape when that happens.

### 4.9 stands as it is

`RendererOptions::mWindow` as a raw `SDL_Window*` is documented as deliberate. Leave.

## Implementation plan

Six steps, smallest first, each verified before the next. P8 is not in it, for the reason above.

### Step 1. P1 and P6

Two one-line removals in two files. Nothing else moves.

Test: the extractor tests already place lights (`apps/components_tests/rtx/extractor/lights.cpp`);
they pass unchanged or the parameter mattered.

### Step 2. P3, the placement context

Files: `vulkanrenderer.{hpp,cpp}`, `sceneacceleration.{hpp,cpp}`, `scenebuffers.{hpp,cpp}`.

Three signatures and their call sites. Fix the `binSprites` argument order in the same pass.

Test: `--gtest_filter=*Scene*:*Vulkan*`, and `verify`, which is what says a placement still places.

### Step 3. P7

Files: `tracedview.{hpp,cpp}`, `rtxrenderer.hpp`.

`RtxRenderer` gains an accessor for the renderer it owns; `TracedView` drops its second reference.

**Nothing in the tree tests this**, so it is checked by reading — the same footing as the rest of
`apps/openmw/mwrender/rtx/`.

### Step 4. P2, the mirror pass

Files: `sceneextractor.{hpp,cpp}`, `meshresolver.{hpp,cpp}`, `materialresolver.{hpp,cpp}`,
`emitterresolver.{hpp,cpp}`.

`MirrorPass` in its own header beside `mirroridentity.hpp`. Each resolver's constructor takes it
where it took the epoch. `walk` sets and clears the stats around the traversal.

Test: every extractor test reads the stats a walk returns, so a pass that lost them fails at once.
Add one that asserts two walks in a row report their own counts and not the sum.

### Step 5. P4, the sky moment and the region request

Files: `apps/rtxtool/cellscene.{hpp,cpp}`, `lighting.{hpp,cpp}`, `stagedworld.{hpp,cpp}`, `bench.cpp`.

`SkyMoment` and `RegionRequest` in `cellscene.hpp` and `lighting.hpp`. The two `relight` overloads
become one, and the two `setSky` overloads become one.

Test: `--gtest_filter=*RtxTool*`. The transition collapse needs its own case — a moment naming one
weather on both sides at a blend of nought has to give what the single-weather call gave, exactly.

### Step 6. P5, the world state lift

Files: a new `components/` home for `Location`, `FogBand`, `MoonState` and `WorldState`;
`sceneframe.hpp` and `weatherresult.hpp` keep aliases; `readworld.{hpp,cpp}` moves beside them;
`apps/rtxtool/lighting.hpp` holds a `WorldReading`.

**No upstream file is edited**, and that is the claim the step has to keep. `git diff --stat` naming
`renderingmanager.cpp` or anything under `mwrender/gl/` means the alias did not do its job.

Test: `verify`, which covers the harness path, plus the whole suite. The rasterizer's own reading is
covered by nothing in the tree, so the check there is that its files are untouched.

## After every step

```
cmake --build build-debug
./build-debug/components-tests
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
cd build-release && ./openmw-rtxtool verify --against=<a run from before the step>
```

**20 of 20 views identical is the bar**, and it covers `components/` and the harness. It does not
cover `apps/openmw/mwrender/rtx/`, which steps 3 and 6 touch: nothing in the tree tests
`MWRender::RtxRenderer`, so those are checked by reading the moved statements against the originals.
That is how the section 3 work caught a dropped sweep.
