# Section 2 of the review, investigated

Every item in "Per-frame computations that can be precomputed" read again in the code and measured
with `perf`. Two of the review's directions were wrong and are corrected here. The order below is the
measurement's, not the review's.

## What the measurement says

Method. `build-release`, `perf record -g --call-graph=dwarf -F 499` over `openmw-rtxtool bench`,
filtered to the performance cores, to the tool's own process, and to the measured frames.

`vivec`, a still camera in a dense cell:

| item | symbol | share |
| --- | --- | --- |
| 2.3 | `SceneExtractor::resolveMesh` | 1.93% |
| 2.2 | `osg::Matrixf::mult` | 1.67% |
| 2.5 | `LightGrid::rebuild` | 1.62% |
| 2.7 | `SceneBuffers::place` | 1.57% |
| 2.1 | `SceneExtractor::animate` | 0.83% |
| 2.1, 2.3 | `__dynamic_cast` | 0.61% |
| 2.6 | `SceneDesc::orderLights` and its sort | 0.12% |
| 2.9 | `DistantLights::collect` | 0.01% |
| 2.4 | `SceneDesc::notePosed` | does not appear |

`island-crossing`, twenty cell boundaries in ten seconds:

| item | symbol | share |
| --- | --- | --- |
| 2.10 | `osg::Image::getColor` | 0.44% |

**Section 2 is about 8% of the main process at Vivec, and half of that is work that has to happen.**
Nothing here is a frame time on its own.

### Where the time actually is

The same profiles, so that effort does not go to the wrong place.

`vivec`:

| symbol | share |
| --- | --- |
| `SpriteShade::layDown` | 24.12% |
| `osg::Group::traverse` | 9.08% |
| `SpriteShade`, the rest of it | 5.33% |
| `SceneExtractor::addDrawable` | 3.20% |

`island-crossing`:

| symbol | share |
| --- | --- |
| `TerrainComposite::TerrainComposite` | 17.60% |
| `ShapeFold`, all three parts | 9.91% |
| `paintedLight` | 6.09% |
| `TerrainComposite::buildChain` | 2.28% |

**The sprite disc rasteriser is a third of a still frame and the terrain bake is a quarter of a
crossing.** Both are outside section 2. The bake runs on the queue's own thread, so it takes cores
from the frame rather than time in it; `layDown` is on the frame path outright.

## Corrections to the review

**2.3's direction is wrong.** It says `Known` should keep the vertex count and the deformer index.
The whole purpose of reading them is to notice that the drawable changed — `resolveMesh`
(`components/rtx/sceneextractor.cpp:1467-1476`) says a rig re-pointed at a longer mesh is the same
rig, and posing it into the old slot writes past a run inside a shared vertex buffer. A cached count
would answer about the mesh the slot was cut for and never about the one in front of it. What can be
saved is named under P2 below.

**2.8 overstates `landReach`.** It reads `Settings::rtx().mDistantLandCells` and
`Settings::camera().mViewingDistance`, which are members of a static category struct rather than a
settings lookup. A few loads and two multiplies. Nothing to hoist.

## Proposals

### P1. `LightGrid::rebuild` computes each light's box three times

`components/rtx/lightgrid.cpp:105, 115, 122`. `boxAround` runs in the sizing loop, the counting pass
and the putting pass, for every light every frame. Vivec stands 614 lights.

**Proposal.** One pass into a `std::vector<CellBox>` kept on the grid, cleared and refilled. The
count and the put read the scratch. `CellBox` is six integers, so the scratch is small and settles at
the busiest cell.

Measured share: 1.62%. Expect about two thirds of it, since the box arithmetic is most of the loop
and the two later passes still walk their cells.

Risk: low. The result is the same boxes in the same order.

### P2. `resolveMesh` looks each deformer up twice

`components/rtx/sceneextractor.cpp:1487, 1492, 1509, 1515`. The rig or morph is found once to work
out `deformer`, then found again to stamp the epoch — two lookups in an `unordered_map` per posed
part per frame. Vivec poses 332.

**Proposal.** Keep the iterator from the first lookup and stamp through it. Four lines.

Beside it, `vertexCountOf` (`:1034`) reaches the count through `dynamic_cast<const osg::Vec3Array*>`.
`osg::Array::getType()` answers the same question with a load and a compare, and `static_cast`
follows. The same two casts sit in `readVertices` (`:1002, 1008`), which runs on the arrival path.

Measured share: part of `resolveMesh`'s 1.93% and of `__dynamic_cast`'s 0.61%. The validation itself
stays, for the reason above.

Risk: low. `getType` is what `osg::Array` carries the enum for.

### P3. `animate` finds the updater with `dynamic_cast` every frame

`components/rtx/sceneextractor.cpp:880-887`. `findUpdater` walks both callback chains and casts every
callback, per animated node per frame. `Animated` (`sceneextractor.hpp:647`) keeps the state set and
the epoch and not the updater.

**Proposal.** `Animated` keeps the updater, and beside it the two chain heads it was found through:

```cpp
struct Animated
{
    osg::ref_ptr<osg::StateSet> mStateSet;
    SceneUtil::StateSetUpdater* mUpdater = nullptr;

    /// The two callbacks the updater was found through, so that a chain which has changed is walked
    /// again rather than answered from a pointer that may have gone.
    const osg::Callback* mCull = nullptr;
    const osg::Callback* mUpdate = nullptr;

    std::uint64_t mEpoch = 0;
};
```

The entry answers from `mUpdater` where both heads still match, and walks again where either does
not. **Two pointer compares rather than an assumption**: a cached pointer alone would be right only
while nothing adds or removes a controller, which is a promise the content makes and not one this
code can check.

Measured share: `animate` is 0.83% and part of `__dynamic_cast`'s 0.61%.

Risk: low, and the guard is what makes it so.

### P4. `readMask` reads one texel at a time

`components/rtx/sceneextractor.cpp:238`. `image.getColor(column, row).a()` per texel of a terrain
blend map, on the frame a chunk arrives.

**Proposal.** Read the alpha through a row pointer for the formats the masks come in, keeping
`getColor` for anything else. `Rtx::readFormat` already says which format an image is, and
`components/rtx/texelreader.cpp` already reads texels by hand elsewhere in this component.

Measured share: 0.44% of the crossing, on the arrival frame — which is the frame with the least room.

Risk: medium. A format read by hand is a second answer to `getColor`'s question, so the fallback has
to stay and a test has to compare the two over every format a mask arrives in.

### P5. `MirrorTraversal::placed()` multiplies per drawable and per light

`components/rtx/sceneextractor.cpp:318`, called at `:444` and `:665`. `osg::Matrixf(mHere) * mRoot` —
one 4x4 multiply per placement per frame. `osg::Matrixf::mult` is 1.67% at Vivec.

**Measure before building.** `mHere` changes only in `apply(osg::Transform&)` (`:627`), so the product
could be computed there and kept beside it. That trades one multiply per drawable for one per
transform, and `NifOsg` builds a transform per node — so whether it wins at all depends on how many
transforms a cell holds against how many drawables. **Count them first**, with a walk that tallies
`apply(osg::Transform&)` against `apply(osg::Drawable&)` over a Vivec frame. Build it only if
transforms are the fewer.

**And the cheaper idea is refused.** Seeding `mHere` with `mRoot` at the start of the walk would
remove the multiply outright, and it is equivalent for every relative transform. It is not equivalent
for one that declares an absolute reference frame: `computeLocalToWorldMatrix` sets the matrix there
rather than pre-multiplying, so the seeded walk would drop `mRoot` where today's multiplies it back
on. A sky node is exactly such a transform, so this would be a wrong placement nobody would see.

### P6. `SceneDesc::notePosed` does a linear search per pose

`components/rtx/scenedesc.cpp:340`. `std::find` over `mDeformed` per posed mesh, so a frame with N
posed meshes does N²/2 comparisons. Vivec poses 332, which is 55,000 compares of an index.

**It does not appear in the profile.** This is work against the stated rule that a frame owes every
other frame the same cost, not work that moves a number: the count is quadratic in something a
crowded cell decides.

**Proposal.** A `std::vector<char>` beside `mKeptMeshes`, set when a mesh is posed and cleared with
the placement. `release` (`:799`) then clears the flag rather than erasing from the vector.

Risk: low.

### P7. `SceneDesc::orderLights` sorts on a nine-field tuple

`components/rtx/scenedesc.cpp:700-708`, called once a frame from `sceneuploader.cpp:50`.

**Measured at 0.12%, and the shape is the finding rather than the cost.** Nine `std::make_tuple`
comparisons decide an order that a single key could. A `std::uint64_t` folded from the position and
the intensity, computed as the light is added, would sort in one compare — and it would make the
order a fact about a light rather than a rule spread across a comparator.

Do it when `Light` is next touched. On its own it buys a tenth of a percent.

### P8. `DistantLights::collect`, `RtxRenderer::traceWorld`, `SceneBuffers::place`

Three the measurement takes off the table.

- `DistantLights::collect` (`components/rtx/distantlights.cpp:94-104`) does (2r+1)² `std::map`
  lookups a frame and measures 0.01%. A flat grid is the better structure and belongs with 5.7 in the
  review, which is about the container rather than the cost.
- `RtxRenderer::traceWorld` (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:818-935`) rebuilds the sky
  every frame and does not appear in the profile at all: it is a few dozen operations once a frame.
  Holding the previous `WorldState` to skip it would add a comparison of a large struct to avoid work
  that costs nothing.
- `SceneBuffers::place` (`components/rtxvulkan/scenebuffers.cpp:413-438`) converts every light,
  sprite and emitter each frame, at 1.57%. Making it incremental needs a per-frame identity for a
  light, which the walk does not carry. That is a change to what a light *is*, and the review's
  judgement stands: not before the renderer draws everything.

## Implementation plan

Four steps, in measured order. Each is testable alone and none depends on the next.

### Step 1. P1, the light grid's boxes

Files: `components/rtx/lightgrid.hpp`, `components/rtx/lightgrid.cpp`.

Add `std::vector<CellBox> mBoxes` to the grid. `rebuild` fills it once, then the counting and putting
passes index it. Clear and refill, never free.

Test: extend `apps/components_tests/rtx/lightgrid.cpp`. Assert the same lights give the same runs as
before over a grid with lights that span one cell, several cells and the whole grid. Add an
allocation assertion over a second `rebuild` of the same lights, since the scratch is the point.

Verify: `--gtest_filter=*LightGrid*`.

### Step 2. P2, the deformer lookups and the array casts

Files: `components/rtx/sceneextractor.cpp`.

Keep the iterator from each deformer lookup. Replace the three `dynamic_cast<const osg::Vec3Array*>`
with `getType()` and `static_cast`.

Test: the extractor tests already walk rigged and morphed graphs twice
(`apps/components_tests/rtx/extractor/skinning.cpp`). Add an assertion that a second walk over a posed
body allocates nothing and poses the same vertices, which pins both halves.

Verify: `--gtest_filter=*Extractor*`.

### Step 3. P3, the updater pointer

Files: `components/rtx/sceneextractor.hpp`, `components/rtx/sceneextractor.cpp`.

`Animated` gains the updater and the two chain heads. `animate` answers from the entry where both
heads match.

Test: extend `apps/components_tests/rtx/extractor/materials.cpp`. Walk a controlled node twice and
assert the material is read again both times. Then swap the node's callback for another controller
and assert the walk follows it rather than the pointer it held — that is what the two heads are for,
and without it the test would pass on a stale cache.

Verify: `--gtest_filter=*Extractor*`.

### Step 4. P4, the mask rows

Files: `components/rtx/sceneextractor.cpp`.

Read the alpha through a row pointer for the formats masks arrive in, and keep `getColor` for the
rest.

Test: a new case beside the terrain tests comparing the two readings texel for texel, over every
format `Rtx::readFormat` names. A format the fast path does not handle must take the fallback and
still agree.

Verify: `--gtest_filter=*Extractor*:*PagedTerrain*`.

### P5 first needs a count, not a change

Add a temporary tally of `apply(osg::Transform&)` against `apply(osg::Drawable&)` over a Vivec frame,
read it, and throw it away. Build the cache only if transforms are the fewer.

## After every step

```
cmake --build build-debug --target components-tests openmw-rtxtool
./build-debug/components-tests --gtest_filter=<the filter for that step>
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

Then the picture, which is what says a walk still mirrors the same world:

```
cd build-release && ./openmw-rtxtool verify --against=<a reference from before the step>
```

And the numbers, warm and interleaved:

```
cd build-release && ./openmw-rtxtool bench --views=vivec,island-crossing --seconds=8
```

The baseline, taken on a warm card with nothing holding the clock back:

| | vivec |
| --- | --- |
| frame ms, median | 9.52 |
| walk ms, median | 3.20 |
| place ms, median | 1.51 |

**Steps 1 to 3 together are worth about 3% of the main process, which is under a tenth of a
millisecond of a 9.5 ms frame.** None of them will show above the run-to-run spread. Take the profile
rather than the frame time to say whether a step did what it meant to.
