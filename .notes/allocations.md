# Section 1 of the review, investigated

## Done

All six steps below are implemented. `openmw-rtxtool verify` renders all twenty views identically to
the tree before them, and `openmw-rtxtool scene --view=vivec` hands over the same digest,
`bc47d6c4a900a1a71763e604046c470e`. The full `components-tests` suite passes, 2022 tests.

What moved, measured at Vivec on the performance cores over the last 60% of a six-second run:

| symbol | before | after |
| --- | --- | --- |
| `std::stable_sort` buffer inside `SpriteShade::shadeToward` | 2.43% | gone |
| `std::sort` that replaced it | absent | 1.76% |
| `operator new` | 0.06% | below 0.01% |
| `malloc` | 0.09% | 0.07% |
| `cfree` | 0.26% | 0.21% |

**No frame time moved, and none was expected to.** Vivec's median frame sits at 9.19 to 9.74 ms
across four runs against a baseline of 9.10, and `place ms` at 1.52 against 1.47. The spread between
repeats is larger than the difference. The plan said this, and it holds.

What is enforced now rather than only written down: a steady extractor walk over an animated textured
material allocates nothing, a sprite run shaded twice allocates nothing, a weather lit at two hours
out of one record allocates nothing, and a texture arrival is bounded at six allocations.

Step 5 was built differently from the plan. See the note under its heading.


What follows revisits every item in "Structures that allocate" in `.notes/review.md`. Each one was
read again in the code and measured with `perf`. One item is withdrawn. The ranking in the review
was wrong, and the correction is the first section here.

## What the measurement says

Method. Release build (`build-release`, `-O3 -DNDEBUG`). `perf record -g --call-graph=dwarf -F 499`
over `openmw-rtxtool bench`. Reports are filtered to the performance cores, to the tool's own
process, and to the last 60% of the run, which drops the load and leaves the measured frames. Two
views: `vivec`, a still camera in a dense cell, and `island-crossing`, twenty cell boundaries in ten
seconds.

The whole heap cost, as a share of the main process's cycles in the measured frames:

| symbol | vivec | island-crossing |
| --- | --- | --- |
| `cfree` | 0.26% | 0.21% |
| `malloc` | 0.09% | 0.14% |
| `operator new` | below 0.01% | 0.07% |
| `SceneTextures::describe` | absent | 0.01% |

**Removing every allocation named in section 1 moves no median and no tail.** The heap costs about
four tenths of one percent. `VFS::Path::Normalized`, `normalizeFilename` and `SceneTextures` do not
appear in either profile above a hundredth of a percent.

So section 1 is not performance work. It is work against a stated rule: *"Allocation is a metric on
the frame path. Persistent scratch buffers refilled with `clear()`, results into an out-parameter, no
`std::string` or `std::function` per frame, logging that compiles out. A test enforces it."* That
rule buys tail behaviour a cycle profile cannot show, and it is worth keeping. The items below are
ranked by whether they break the rule on the game frame path, then by cost to fix.

### What is expensive, so that effort is not spent here by mistake

The same profiles say where the CPU goes. None of it is the heap.

`vivec`, measured frames, main process:

| symbol | share |
| --- | --- |
| `SpriteShade::layDown` | 24.99% |
| `osg::Group::traverse` | 8.00% |
| `SpriteShade` sort, both merge symbols | 2.43% |
| `SceneExtractor::addDrawable` | 3.04% |
| `SceneExtractor::resolveMesh` | 1.98% |
| `osg::Matrixf::mult` | 1.62% |
| `SpriteShade::readAt` | 1.60% |
| `LightGrid::rebuild` | 1.48% |
| `SceneExtractor::retire` | 1.41% |
| `osg::StateSet::getUniform` | 1.29% |
| `SpriteShade::shadeToward` | 0.85% |
| `__dynamic_cast` | 0.67% |

`SpriteShade` is 29.9% of the main process's cycles. That is nearly the whole of `place ms`, which
runs at a median of 1.47 ms in a 9.10 ms frame. **The disc rasteriser is the largest single host cost
in a still cell, and no part of it is an allocation.**

`island-crossing`, measured frames, main process:

| symbol | share |
| --- | --- |
| `TerrainComposite::TerrainComposite` | 18.83% |
| `paintedLight` | 6.22% |
| `ShapeFold::fold` | 4.62% |
| `ShapeFold::closes` | 4.03% |
| `sampleAt`, inside the bake | 3.57% |
| `toEncoded` | 2.29% |
| `osg::Group::traverse` | 2.25% |
| `TerrainComposite::buildChain` | 2.14% |
| `ShapeFold::canonical` | 1.24% |
| `osg::Image::getColor` | 0.43% |

About a third of the crossing's cycles are the bake, and the bake runs on the queue's own thread. The
walk's own share is `ShapeFold`, at 9.89%. The crossing's frame times are median 7.08 ms, p99 178.95
ms and worst 221.83 ms, so the tail is real. **The tail is the bake thread taking cores from the
frame and the walk folding sheets. It is not the heap.**

## Proposals

### P1. `SceneExtractor::takeTexture` builds a string per texture role per frame

`components/rtx/sceneextractor.cpp:1808-1815`.

```cpp
Index SceneExtractor::takeTexture(const osg::Image* image, ExtractionStats& stats)
{
    if (image == nullptr || image->getFileName().empty())
        return sNoIndex;

    countFormat(*image, stats);
    return mScene.addTexture(VFS::Path::Normalized(image->getFileName()));
}
```

`VFS::Path::Normalized(std::string_view)` copies the name and lowercases it. A texture path is longer
than a short string, so it reaches the heap every time. `SceneDesc::addTexture` takes a
`NormalizedView`, so the string dies at the end of the call.

`resolveMaterial` (`:1796`) reads an animated material again on every frame, and `readMaterial`
(`:1832-1839`) calls `takeTexture` up to four times. `Material::mTextureTransform` records 432
UV-controlled surfaces in Vivec. So the bound is about four heap strings per animated material per
frame.

**Proposal.** Key the answer on the image, not on its name.

```cpp
Identity<const osg::Image> mTextureOf;
```

`Identity` is the extractor's own alias for an `unordered_map` over an owning `osg::ref_ptr`, keyed by
address. The extractor already uses it for meshes, materials, rigs, morphs and emitter sprites. On a
miss, build the `Normalized` once, call `addTexture`, and call `mScene.holdTexture` on the slot. On a
hit, stamp the epoch and return the index. In `retire`, sweep the map and call `mScene.dropTexture`.

The hold is required and is not an optimisation. A slot whose last material stops naming it drops to
nought references and is freed and handed out again. A cache without a hold would answer with a slot
another texture had taken over. `mEmitterTextures` (`sceneextractor.hpp:627`) holds its two slots for
exactly this reason, and this follows it.

**What changes besides the allocation.** `countFormat` currently runs per call and would run per
arrival. `ExtractionStats::mTextureFormats` is what `openmw-rtxtool scene` prints, and a second walk
of the same graph would report nought formats where it now reports the whole table. Keep the count
where it is, outside the cache, so the reported numbers do not move.

Risk: low. The pattern is already in the file four times.

### P2. `SpriteShade::shadeToward` sorts with a heap buffer

`components/rtx/spriteshade.cpp:65-67`.

```cpp
std::stable_sort(mOrder.begin(), mOrder.end(),
    [this](std::uint32_t a, std::uint32_t b) { return mProjected[a].mDepth > mProjected[b].mDepth; });
```

`std::stable_sort` builds a `_Temporary_buffer` before it sorts, and that reaches `operator new` on
every call. The call runs twice per qualifying emitter per frame. The profile names both merge
symbols and puts them at 2.43% of the main process at Vivec, so this one is measurable as well as
against the rule.

**Proposal.** Make the order total and sort with `std::sort`.

```cpp
std::sort(mOrder.begin(), mOrder.end(), [this](std::uint32_t a, std::uint32_t b) {
    if (mProjected[a].mDepth != mProjected[b].mDepth)
        return mProjected[a].mDepth > mProjected[b].mDepth;

    return a < b;
});
```

Two sprites at one depth are ordered by index, which is what the comment already asks for. With a
total order `std::sort` has one answer, so the result is identical to the stable one and cannot
flicker between frames. Introsort uses stack, not heap.

Keep the comment. Change "Stable, so" to say the tie-break carries the same promise.

Risk: low. The output is provably the same sequence.

### P3. `SceneTextures` is the loader to persist

`components/rtx/sceneuploader.cpp:109-110` builds one and destroys it on every arrival frame.

```cpp
const SceneTextures textures = reset ? SceneTextures(scene, images, &mComposites)
                                     : SceneTextures(scene, images, scene.getArrivedTextures(), &mComposites);
```

The object owns five vectors (`texturebuilder.hpp:99-115`). `describe` builds two more locals, `kept`
and `lightOf` (`texturebuilder.cpp:159, 165`), the reset constructor builds a third, `everything`
(`:141`), and each sprite-light bake builds a `levels` vector (`:199`). The profile puts
`SceneTextures::describe` at 0.01% of the crossing, so this is the rule and not the clock.

**Proposal.** `SceneUploader` owns one `SceneTextures` for its life.

- Replace both constructors with `void describe(const SceneDesc&, Resource::ImageManager&,
  std::span<const Index> slots, const CompositeQueue*)` and a second overload that takes no slot list
  and walks the table.
- Every vector becomes `clear()` then refill. `mShading` already uses `resize`, which keeps its
  buffer.
- `kept`, `lightOf` and `everything` become members.
- `mUnreadable` resets at the top of `describe`.
- Delete the move constructor and the move assignment. Nothing moves it once the uploader owns it.

The span contract is unchanged. `mLevels` is still reserved exactly before any description points into
it, and `mSpriteLights` is still filled before any description reads it.

Risk: low. One caller, and `apps/components_tests/rtx/` holds the tests that cover it.

### P4. `CompositeQueue` builds a request per chunk

`components/rtx/compositequeue.cpp:90-113`. A `Request` owns four vectors
(`compositequeue.hpp:123-138`). `gather` builds one per chunk on the game thread. `collect` clears
`mTaken` (`:186`), which destroys them and frees all four.

**Proposal.** Cycle the requests instead of destroying them.

- Add `std::vector<Request> mSpare` beside `mTaken`.
- `gather` takes a `Request` off `mSpare` where one is there, and clears its four vectors. Otherwise
  it builds one.
- `collect` moves each spent `Request` into `mSpare` instead of letting `mTaken.clear()` free it. The
  images have to be released there, because a `ref_ptr` held in a spare buffer keeps an image alive
  for nothing. Clear `mImages` and keep the other three.
- `mPending` and `mDone` stay `std::deque`. Their node allocations are bounded by the queue depth and
  are not per chunk.

The bake's own buffers (`levels`, `stack` in `bake`, and everything the `TerrainComposite` constructor
builds) stay as they are. They run on the baker thread, off every frame, and the profile shows the
bake is a third of the crossing's cycles for reasons that have nothing to do with where its memory
came from.

Risk: medium. The spare list is touched by the game thread only, at both ends, so no lock changes.
Confirm that by reading `gather` and `collect` together before the change.

### P5. Texture and geometry arrival allocates on the arrival frame

Four places, all on the frame a cell arrives:

- `components/rtxvulkan/texture.cpp:301-302` and `:323-324` build `images` and `writes` per call.
- `texture.cpp:177` builds `regions` per texture.
- `texture.cpp:272` builds `"texture " + std::to_string(slot)`, and `texture.cpp:195` builds
  `std::string(name) + " shading"`. Both are Vulkan debug-object names. `Device::setName` does nothing
  unless `VK_EXT_debug_utils` is loaded, so the string is built for nothing in a release run.
- `components/rtxvulkan/sceneacceleration.cpp:291` builds `scratchOffsets` per build.

**Proposal.** Scratch members on the owners, and no name unless a name is wanted.

- `TextureArray` gains `std::vector<VkDescriptorImageInfo> mImageScratch` and
  `std::vector<VkWriteDescriptorSet> mWriteScratch`. `describe` clears and refills them.
  `describeApart` is `const`, so either make the two members `mutable` or make the function take the
  buffers as out-parameters. Prefer the out-parameters: the rule names them, and the one caller
  (`scenemicromaps.cpp:391`) already owns a scratch buffer beside it.
- `Texture`'s `regions` becomes a parameter passed in by `TextureArray::write`, which owns it.
- Give `Device` a `bool wantsNames()` and skip the name where it is false. The two concatenations
  then cost nothing in a release run and stay exactly as they are under validation.
- `SceneAcceleration::buildMeshes` keeps `scratchOffsets` as a member.

The device scratch buffer at `sceneacceleration.cpp:388` and the three buffers in
`SceneMicromaps::bake` are device allocations, not host ones. They are a larger change, they are not
what the rule is about, and the crossing's tail is elsewhere. Leave them.

Risk: low, except the `describeApart` signature, which is a two-caller change.

### P6. `RtxRenderer::eventTraversal` builds an event list per frame

`apps/openmw/mwrender/rtx/rtxrenderer.cpp:330`.

```cpp
osgGA::EventQueue::Events events;
mEvents->takeEvents(events);
```

`osgGA::EventQueue::Events` is a `std::list`, so every event drained costs a node.

**Proposal.** Keep the list as a member of `RtxRenderer` and let `takeEvents` refill it. The list is
cleared by `takeEvents` itself, so a member is drained and refilled with no change to the call.

This does not remove the node allocation, because a `std::list` node is not recycled by `clear()`. To
remove it the queue would have to hand out something else, and `osgGA::EventQueue` is upstream. **The
honest answer is that this one cannot be fixed inside the RTX places.** Record what it costs and leave
it. It is one node per function key pressed, and nothing presses one on a steady frame.

### P7. The harness relights with about a hundred strings per frame

`apps/rtxtool/lighting.cpp:31` and `:47` call `Rtx::makeDaylight`, which calls `readWeather`
(`components/rtx/lightbuilder.cpp:144`). `apps/rtxtool/view.cpp:192` calls `relight` on every frame
where the clock runs.

One `readWeather` builds, at least:

- `requireWeather` (`lightbuilder.cpp:380-411`): one prefix and 24 keys, each of them a chain of
  concatenations.
- four `ramp(...)` calls, each of which is `Sky::colourRamp` (`components/sky/timeofday.cpp:140-147`,
  one stem and four keys) plus a `std::string(quantity)` for `getValue`.
- `Sky::sunDiscAt`, one key. `Sky::landFogRamp`, one stem and two keys. `Weather::windSpeed`
  (`components/weather/downpour.cpp:32`), one key. `Rtx::glareView`, one key.

`makeDaylight(from, to, ...)` calls it twice.

**Built differently from what follows.** `Fallback::Map::init` merges rather than replaces, and the
test binary calls it more than once — so a table inside `components/rtx` would answer from whenever it
happened to be built first, and `makeDaylight` would depend on the order a suite ran in. What landed
instead is `Rtx::WeatherRamps` and `Rtx::readWeatherRamps`, which the caller holds. `RtxTool::HeldWeathers`
keeps the two a window is between, and `StagedWorld` owns it. The naming overloads of `makeDaylight`
still read a record every call, so nothing outside the harness changed.

**Proposal, two parts.**

First, `requireWeather` validates the fallback map, which does not change during a run. It belongs
once per weather, not once per reading. Add a `std::array<bool, 10>` beside the weather table in
`lightbuilder.cpp`, indexed by `weatherIndex`, and check it before validating. That removes about a
quarter of the strings and is four lines.

Second, `readWeather`'s result depends on the weather and the hour alone. Split it: read the ramps
per weather into a `WeatherRamps` struct held in a `std::array<WeatherRamps, 10>`, filled on first
use, and let `readWeather` evaluate those ramps at the hour with no string at all. `TimeOfDaySettings`
is already a shared singleton (`Sky::TimeOfDaySettings::shared()`), so the pattern matches.

This is harness-only. The game reaches `makeSkylight` directly
(`apps/openmw/mwrender/rtx/rtxrenderer.cpp:846`) and never calls `makeDaylight`. It is worth doing
because `view` is how a rendering change is judged, and a harness that allocates a hundred times per
frame is a harness whose own frame times cannot be read.

Risk: low. `apps/components_tests/rtx/` covers `makeDaylight`, and the values must not move.

## Withdrawn

### The `Log(Debug::Verbose)` in `SceneUploader::hand` does not allocate

Review item 1.6 was wrong. `Log::Log` (`components/debug/debuglog.cpp:31-45`) sets `mShouldLog` from
the level and returns at once when it is false, and every `operator<<` is guarded by that flag. The
arguments are still evaluated, and they are three `double` values. There is no allocation and no lock
unless the level is on.

## Not proposed

### `Weather::WrapAroundOperator::operateParticles`

`components/weather/precipitation.cpp:98` calls `ps->getWorldMatrices()`, which returns a fresh
`std::vector<osg::Matrix>` per particle system per frame. The file is lifted shared code and both
renderers run it. A change there is a change the rasterizer pays for, and *"the rasterizer's
behaviour is never changed"*. Caching the matrices needs a way to know the parent chain moved, and
`osg` offers none cheaply.

Name the file and the change, and wait for a go-ahead. Until then, one vector per system per frame,
and the profile does not see it.

## Implementation plan

Six steps, ordered so that each one is testable on its own and none depends on the next. No step
touches an upstream file.

### Step 1. P2, the sprite sort

Files: `components/rtx/spriteshade.cpp`.

Replace `std::stable_sort` with `std::sort` and the tie-break above. Update the comment to say the
tie-break is what makes the order total.

Test: extend the nearest existing test in `apps/components_tests/rtx/` that covers `SpriteShade`.
Assert that two sprites at one depth come out in index order, and that a run of known depths produces
a known sequence of layer counts. Hand-compute the expected counts from the grid arithmetic.

Verify: build `components_tests`, run with `--gtest_filter=*SpriteShade*`.

Expected: about 2.4% of the main process's cycles at Vivec, and one allocation per emitter per light
per frame.

### Step 2. P1, the texture cache

Files: `components/rtx/sceneextractor.hpp`, `components/rtx/sceneextractor.cpp`.

1. Add `Identity<const osg::Image> mTextureOf;` beside `mEmitterTextures`.
2. Rewrite `takeTexture` to look the image up, and to build the `Normalized`, call `addTexture` and
   call `holdTexture` only on a miss. Stamp `mEpoch` on both paths.
3. Keep `countFormat` outside the cache, so the reported format counts do not change.
4. In `retire`, add an `erase_if` over `mTextureOf` that calls `mScene.dropTexture` on the way out.
   Put it beside the `mEmitterTextures` sweep, which does the same thing.

Test: extend the extractor tests. Walk a graph twice and assert that the second walk adds no texture
and that `SceneDesc::getTextures().size()` does not move. Then drop the drawable, walk, retire, and
assert the slot is free.

Verify: `--gtest_filter=*Extractor*:*Scene*`.

### Step 3. P3, the persistent loader

Files: `components/rtx/texturebuilder.hpp`, `components/rtx/texturebuilder.cpp`,
`components/rtx/sceneuploader.hpp`, `components/rtx/sceneuploader.cpp`.

1. Turn both `SceneTextures` constructors into `describe` overloads. Keep the default constructor.
2. Move `kept`, `lightOf` and `everything` to members. Clear each at the top of `describe`.
3. Clear `mImages`, `mLevels`, `mDescriptions` and `mSpriteLights` at the top. `mShading` keeps
   `resize`.
4. Reset `mUnreadable`.
5. Delete the move operations.
6. Add `SceneTextures mTextures;` to `SceneUploader` and call `describe` where the two constructors
   were.

Test: extend the uploader tests. Hand two different scenes to one uploader in turn and assert the
descriptions of the second name only the second scene's slots. That is what a stale buffer would
break.

Verify: `--gtest_filter=*Uploader*:*Texture*`.

### Step 4. P5, the arrival scratch

Files: `components/rtxvulkan/texture.hpp`, `components/rtxvulkan/texture.cpp`,
`components/rtxvulkan/scenemicromaps.cpp`, `components/rtxvulkan/sceneacceleration.hpp`,
`components/rtxvulkan/sceneacceleration.cpp`, `components/rtxvulkan/device.hpp`,
`components/rtxvulkan/device.cpp`.

1. `Device::wantsNames()`, returning whether `mSetObjectName` is loaded. Guard the two concatenations
   in `texture.cpp` with it.
2. `TextureArray` gains the two descriptor scratch vectors. `describe` uses them.
3. `describeApart` takes the two buffers as out-parameters. Update `scenemicromaps.cpp:391`.
4. `Texture`'s `regions` becomes a parameter. `TextureArray::write` owns the buffer.
5. `SceneAcceleration` gains `mScratchOffsets`.

Test: this is what `apps/components_tests/rtx/visibility/framecost.cpp` is for, but that test does not
reach an arrival. Add a second test beside it that warms up, then adds a texture, and asserts the
arrival's allocation count is under a stated budget. State the budget as a constant with the reason
beside it, the way `budgetPerFrame` already is.

Verify: `--gtest_filter=*FrameCost*`. This test needs a device and skips without one.

### Step 5. P7, the harness weather

Files: `components/rtx/lightbuilder.cpp`.

1. Guard `requireWeather` with a per-weather flag.
2. Add a `WeatherRamps` struct and a `std::array<WeatherRamps, 10>` filled on first use. `readWeather`
   evaluates the held ramps at the hour.

Test: extend the existing `makeDaylight` tests. Assert that two calls at one hour give the same
`Daylight`, and that a call at another hour differs. Then assert the second call's allocation count is
nought, using `Rtx::Testing::getAllocationCount`.

Verify: `--gtest_filter=*Daylight*:*LightBuilder*`.

### Step 6. P4, the composite request pool

Files: `components/rtx/compositequeue.hpp`, `components/rtx/compositequeue.cpp`.

Last, because it is the only step that touches code two threads share, and because the profile says
its whole cost is a hundredth of a percent.

1. Add `std::vector<Request> mSpare`.
2. `gather` takes from it and clears the four vectors.
3. `collect` clears `mImages` and returns the request to `mSpare`.
4. Read `gather` and `collect` together and confirm both ends are the game thread only.

Test: extend the composite queue tests. Queue a chunk, collect it, queue a second, and assert the
second's layers and masks are the second chunk's. A spare buffer that was not cleared would carry the
first chunk's.

Verify: `--gtest_filter=*Composite*`.

## After every step

Build only the targets touched, then run the covering test binary with a filter, then format:

```
cmake --build build-debug --target components_tests openmw-rtxtool
./build-debug/components_tests --gtest_filter=<the filter for that step>
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

Then take the frame numbers again, warm and interleaved:

```
cd build-release && ./openmw-rtxtool bench --views=vivec,island-crossing --seconds=8
```

The baseline, taken on a warm card with nothing holding the clock back:

| | vivec | island-crossing |
| --- | --- | --- |
| frame ms, median | 9.10 | 7.08 |
| frame ms, p99 | 11.29 | 178.95 |
| frame ms, worst | 15.88 | 221.83 |
| walk ms, median | 2.79 | 3.36 |
| place ms, median | 1.47 | 1.18 |

**Only step 1 is expected to move any of these.** Every other step is against the rule, not the clock,
and a step that moves a number by less than the run-to-run spread has not been measured, it has been
guessed at. Repeat a leg rather than believing one.
