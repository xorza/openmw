# A redesign of the CPU side

A proposal and a plan, written against the measurements in `.notes/rtx/cpu-performance.md`. Those
were taken at `0024c46a07` on an RTX 4090 Laptop and an i9-13980HX. Every number quoted here as
"measured" comes from that document. Every number quoted as "expected" is an estimate, and the plan
says how each one is to be checked.

**This is early.** `AGENTS.md` says no frame times until the renderer draws everything the game
has. One reading justifies the exception and it is the one that stopped the work: Vivec asks
7.03 ms of the host against 5.34 ms of the device, so the host is already the limit at one place of
eighteen. The proposals below are ordered so that the ones which cannot change a picture come
first.

## The shape of the problem

The host does four kinds of work in a frame, and each has a different answer.

1. **Work the device should be doing.** The sprite shading is a grid rasterisation over thousands
   of discs, on the frame path, on one core. Proposal 1.
2. **Work that asks a question the last frame already answered.** The walk re-resolves every
   drawable, the light grid is rebuilt from nothing, and the paged chunks are walked again. Nothing
   about any of it changed. Proposals 2 to 5.
3. **Work that belongs to an arrival and lands on a frame.** The fold and the chunk build.
   Proposal 7.
4. **Work that is upstream's and is small.** Physics scheduling, the update traversal, the crosshair
   pick, MyGUI. Together under 0.5 ms. Left alone.

The second kind is the largest and the least obviously wasteful, because each individual step is
already written well. `MeshResolver::resolve` has a fast path, `PlacementTable` writes only what
moved, and `updateInstanceRecords` touches only the changed rows. What none of them has is a way to
say **"this is the same drawable I resolved last frame, in the same place"** without doing a hash
lookup to find out.

## Proposal 1 — the sprite shading goes to the device

**What it costs now.** `SpriteShade::layDown` is 18.22 per cent of the Vivec recording in self
time, and its chain `SceneBuffers::binSprites` is 2.40 ms a frame — 39 per cent of the main
thread's frame, and about two thirds of Vivec's unaccounted `rest`. It is 0.06 ms at Seyda Neen and
under 0.01 ms in most interiors, so this is one place's problem and it is the place that is
CPU-bound.

**Why it is large.** The work is `O(sprites x disc area)`, run once per emitter per light, twice a
frame. `sLargestInCells` bounds a disc at eight cells of radius, so a footprint reaches 289 cells of
the 32 by 32 grid. Vivec holds 6076 sprites in 62 emitters, so the frame lays down up to three and a
half million cell updates on one core.

**Why the device is the right place.** The precedent is in the tree. `spritebin.h` records that the
same argument was made for the tile binning, that the host cost was half a millisecond over Balmora
in the rain, and that the move made it a 0.05 ms device zone. The inputs are already on the device —
`tables.mSprites` and `tables.mEmitters` are written a few lines above the `shade` call — and the
outputs are two floats a sprite in a buffer the trace already reads.

**The shape it takes.** One workgroup per emitter per light. 1024 lanes, one per grid cell, and the
grid lives in shared memory at 4 KiB. The run is sorted by depth along the light, then walked in
order: for each sprite one lane reads the bilinear value at its point and writes the sprite's layer
count, then every lane adds its own cell's coverage of that sprite's disc. The read and the write
are separated by a barrier apiece.

**What makes it exact rather than an approximation.** Each cell is touched by exactly one lane, so
no atomic and no float summation order is in question. The sequence over sprites is the same
sequence the host walks. `readAt` and the coverage rule are copied over unchanged.

**The one hard part is the sort.** A bitonic sort in shared memory over the run, keyed on the depth
bits with the sprite index as the low half of a 64-bit key, which is the same total order
`shadeToward` states. An emitter longer than the workgroup's sort capacity falls back to the host
implementation for that emitter alone, and a counter says how often that happens.

**Expected.** Vivec's host frame falls from 7.03 ms to about 4.6 ms, which puts it under its 5.34 ms
of device time and removes the only CPU-bound row in the corpus. The device gains a zone the
`sprites` line already has room for — it is 0.09 ms at Vivec today.

**Risk.** The picture must not move. `apps/components_tests/rtx/spriteshade.cpp` already holds
seven tests with hand-computed expectations, including a coverage sweep against an analytic disc.
The host implementation stays as the reference and the device pass is cross-checked against it —
which is what the tests are for, and what the fallback path needs anyway.

## Proposal 2 — liveness moves from the map entry to the slot

**What it costs now.** Every drawable does three `std::unordered_map` lookups a frame:
`mMeshes.find`, `mMaterials.find` through `reuse`, and `mPlacements.find`. The hash-table lines
together are 4.36 per cent of the Seyda Neen recording — 0.152 ms a frame — against 0.276 ms for the
whole of `addDrawable`, `MeshResolver::resolve` and `MaterialResolver::reuse` in self time. **More
than half of what it costs to resolve a drawable is the three lookups.**

**Why they are there.** They are not there to find the slot. They are there so that the sweep can
tell what the walk met. `Kept` stamps each map entry with the epoch and `retire` drops the entries
that carry an older one.

**The redesign.** Split the two jobs the map is doing.

- **An arrival index**: `key -> slot`, consulted only when something is met for the first time, or
  when the trace of Proposal 3 misses. No epoch in it.
- **A slot life**: a dense `std::vector<std::uint32_t>` of epochs, one per slot of the table it
  belongs to, beside a count of how many slots this epoch has reached.

A walk then stamps by slot — `mLife[slot] = epoch` — which is a write into a dense array at an
address the walk usually already holds. A sweep is a linear pass over that array, which is what
`SlotRows::mark` and `SlotRows::sweep` already are. The `whole()` shortcut survives unchanged: the
count against the live count.

**Why this is the enabling change and not the win.** On its own it replaces a scattered node write
with a dense one, which is worth perhaps a third of the 0.152 ms. What it buys is that a slot index
is now a sufficient handle for everything the frame does to an entry, so Proposal 3 can hold slot
indices rather than map iterators — and iterators are what would otherwise tie the design to a
node-based map for ever.

**Expected.** 0.05 ms a frame at Seyda Neen on its own. It is a means.

**Risk.** Low, and it is bounded to `components/rtx/mirroridentity.hpp` and its four users. The
sweep order changes from bucket order to slot order, which is a change in *nothing* the picture
reads: `SlotRows::takeLowest` already makes the slot a function of what is free rather than of what
was freed last, which is the property the `one-cell-walk` reading was taken against.

## Proposal 3 — the walk keeps a trace, and a still world reads it

**The observation.** The walk visits the same nodes in the same order every frame, because
`descend` walks children in order, a `Switch` in branch order, and a residency in cell order. A cell
of 4916 placements has 138 deforming drawables in it. Nearly every drawable the walk meets is one it
met last frame, at the same position in the sequence, standing in the same place.

**The trace.** `MirrorTraversal` keeps one flat array, in walk order, of what each drawable resolved
to:

```
struct TracedPlacement
{
    std::size_t mIdentity;          // the fold of the path, which is the placement key
    osg::Matrixf mPlace;            // where it stood
    const osg::StateSet* mMaterialKey;
    Index mPlacement;
    Index mMesh;
    Index mMaterial;
    float mFade;
};
```

Ninety-six bytes a drawable, one contiguous buffer, cleared and refilled — 0.5 MiB at Seyda Neen and
5 MiB on a nine-by-nine exterior.

**What a frame does with it.** `addDrawable` takes a cursor. It compares the identity at the cursor
against the one the walk just folded. On a match — which is the overwhelming case — it has the three
slots without touching a map at all: it stamps the three slot lives, compares `mPlace` against the
transform it just composed, and only where those differ does it reach into `PlacementTable`. On a
mismatch it falls back to the maps, rewrites the row and carries on; the rest of the frame is
resolved through the maps and the trace is rebuilt as it goes.

**Why the identity is enough on its own.** It is the fold of every node address down the path,
including the drawable at the leaf, so a different drawable at the same walk position folds
differently. The material key is carried beside it because a state set can be replaced under an
unchanged path, which is exactly what `Shading::mAnimated` exists for — a recorded key that is
animated is re-read, as it is today.

**What it removes.** The three hash lookups, which are 0.152 ms of the recording, and the random
80-byte read of the instance row that `PlacementTable::move` makes to compare a transform, which
`osg::Matrixf::compare` puts at 0.033 ms. Both become one sequential stream.

**What it does not remove.** The traversal itself, and the state sets pushed at every *node* on the
way down. The trace has one entry per drawable, so a node's own `pushShading` is outside it. That
cost is Proposal 4's, where the node is inside a replayed subtree, and Proposal 6's, where it is not.

**Expected.** 0.18 ms of the 1.17 ms walk at Seyda Neen, and about 0.30 ms of Vivec's 2.12. It
scales with the drawable count, so it is worth most where the walk is worst.

**Risk.** Moderate, and it is the proposal that needs the most care. A trace that goes stale
silently would mirror the wrong geometry. Three guards: the identity compare is the primary one; a
generation counter on the extractor invalidates the whole trace whenever `retire` or `abandon`
erases anything; and a debug-only pass re-resolves through the maps and asserts that the trace
agreed, which `--validation` can carry.

## Proposal 4 — a paged chunk that did not change is not walked again

**What it costs now.** `TerrainResidency::collect` is 0.574 ms a frame at Seyda Neen — 49 per cent
of the walk — 0.870 ms at Vivec and 2.62 ms on the crossing. Almost all of it is the mirror
descending the chunk subtrees that `QuadTreeWorld::handOver` gives it. `osg::Group::traverse` is
8.24 per cent of the Seyda Neen recording in self time, and `Group.cpp:63` is the hottest single
line in the whole profile.

**The observation.** A paged chunk is immutable once built. `ObjectPaging` merges the statics of a
cell into one geometry and caches the node; nothing under it animates, carries a light, or holds a
particle system — `DistantLights` exists precisely because `LIGH` is not a paged type. So a chunk
handed over with the same `ChunkName` and the same node address as last frame contains, node for
node, exactly what it contained last frame.

**The replay.** `MirrorTraversal::takeChunk` already receives both halves of that question. Where
the pair matches what it recorded, it does not descend at all: it replays the run of trace entries
that chunk produced last frame, stamping the slot lives and touching nothing else. Where the pair
differs, it descends and records a fresh run.

**What makes it safe rather than assumed.** The run is recorded with a flag saying the subtree held
nothing but transforms, groups and plain drawables with no callback on any of them. A chunk that
fails that test is never replayed, whatever its name says. The flag is computed once, when the
chunk is first walked, and it costs the walk one boolean.

**Expected.** Most of `collect`'s walking cost on a frame that crosses no level-of-detail boundary,
which is nearly every frame of a standing or slowly moving camera. Call it 0.45 ms of Seyda Neen's
0.574 and 0.70 ms of Vivec's 0.870. It buys nothing on the crossing route, where every chunk is new.

**Risk.** Moderate. It rests on paged chunks being immutable, which the tree states in three places
but nowhere asserts. The plan below adds the assertion before it adds the replay.

## Proposal 5 — the light path is gated on a revision

**What it costs now.** Every frame, whatever the world did:

- `SceneDesc::orderLights` sorts every light. 0.024 ms at Seyda Neen with 341 of them.
- `LightGrid::rebuild` takes a bounds pass, a doubling search whose every iteration is a full pass
  over the lights, a count pass and a place pass. 0.081 ms.
- `SceneBuffers::place` converts every light, every sprite and every emitter into its GPU form, and
  writes three whole buffers.

Together about 0.15 ms a frame at Seyda Neen and about 0.20 at Vivec, spent to arrive at the same
light grid as the frame before.

**The redesign.** `SceneDesc` keeps the previous frame's light array instead of clearing it.
`clearPlacement` swaps the two rather than emptying one. `addLight` writes into the new array and
compares against the same position of the old one; a difference, or a differing count at the end,
raises `mLightsChanged`. The walk order is deterministic, so an unchanged world writes an identical
array and the flag stays down.

With the flag down, `orderLights` returns, `LightGrid::rebuild` returns, and the three buffer
writes are skipped for any frame slot whose written revision already matches — which is the
`SlotTable` pattern the instance table already uses.

**Expected.** 0.15 ms at Seyda Neen and 0.20 at Vivec, on any frame where no lamp moved. A torch in
a walking hand raises the flag, which is correct and is what the fallback is for.

**Risk.** Low. `orderLights` keeps its guarantee: it is skipped only when the array is identical to
one it already sorted. The one thing to get right is that the flag must also rise when the light
*count* is unchanged but the walk order changed, which the element-wise compare catches.

## Proposal 6 — `fadeThrough` stops asking a map for a uniform nothing has

**What it costs now.** `MirrorTraversal::pushShading` calls it at every node and every drawable the
walk enters. It is 0.113 ms a frame at Seyda Neen, 0.175 at Vivec — a tenth of the walk. The work is
two `osg::StateSet::getUniform` calls, and `getUniform` takes a `std::string` and searches a
`std::map<std::string, RefUniformPair>`: `stl_tree.h:2644` is 0.93 per cent of the Seyda Neen
recording and `memcmp-avx2-movbe.S:416` is 0.79.

**The fix is one line and it is not a cache.** `osg::StateSet::getUniformList()` is that map, and
`fadeThrough` wants an answer only where it is non-empty. Nearly no state set in Morrowind's content
carries a uniform at all — the two the function asks for are written by
`MWRender::TransparencyUpdater` onto the handful of actors the game is fading. An `empty()` test
first turns the common case from two red-black-tree searches into one load and one compare.

**Expected.** Most of the 0.113 ms at Seyda Neen and the 0.175 at Vivec.

**Risk.** None worth the word. The two calls are already guarded by a null test on the first
uniform, so an empty list already means the same answer.

## Proposal 7 — arrival work leaves the frame

**What it costs now.** On the crossing route the main thread's 4.72 ms holds
`ObjectPaging::createChunk` at 1.54 ms and `ShapeFold` at 1.32 ms. Both are work a chunk owes once,
paid on whichever frame first asks for it.

**Three changes, in order of confidence.**

**7a. Measure what the frame waits for the warming thread.** `TerrainResidency::collect` sets
`mYield` and then takes `mBuilding`, so a frame arriving while the thread is inside a chunk waits
for that chunk. `QuadTreeWorld::preload` tests its abort flag once per view entry, so the wait is
one chunk build — about a millisecond — and it is nothing at all at a standing camera, where `ask`
returns early and the thread is asleep.

**The lock is load-bearing and is not to be removed.** `Terrain::ChunkManager::getChunk` and
`Terrain::ObjectPaging::getChunk` each test their cache, build on a miss, and write back without
holding anything, so two threads missing one key both build it and the frame keeps whichever node
its own call returned — which for a mirror is a different drawable, a different mesh and a different
instance. `TerrainResidency`'s own doc records the five runs of `seyda-neen-shore` that measured it.

So this step is an instrument and not a change: time the `mBuilding` acquisition and report it
beside `walk`. If it is small, 7a is closed. If it is not, the answer is a finer yield inside the
thread's own loop, not a lock the frame may skip.

**7b. The warming thread folds what it builds.** The fold is a pure function of a geometry's
positions and indices, and the warming thread already holds the built chunk before any frame sees
it. A `FoldCache` — a mutex, and a map from `const osg::Geometry*` to a `FoldedShape` and the folded
index run — is filled by the thread and drained by `MeshResolver::resolve`. `WorldMirror` owns it
and hands it to both, so neither reaches into the other.

**This changes no picture at all**, which is the property that makes it the safest of the three:
`osg::TriangleIndexFunctor` and `ShapeFold::fold` are deterministic, so a fold computed on a thread
and a fold computed on the frame give the same indices and the same `FoldedShape`.

**7c. The warming lead is measured again.** `sLeadSteps` is thirty and the class doc records that
sixty made the median worse. That reading was taken before the thread had anything else to do. Take
it again after 6a and 6b, and take it at 12,000 units a second, which is the speed the lead is
failing at.

**Expected.** 7b removes up to 1.32 ms of the crossing route's 4.72 ms main-thread frame. 7c
decides how much of the 1.54 ms of `ObjectPaging::createChunk` follows it. Neither buys anything at
a standing camera.

**Risk.** 7b is low and changes no picture. 7a and 7c are measurements.

## Proposal 8 — the harness gets an unsettled leg

Carried over from `.notes/rtx/cpu-performance.md`, because Proposal 7 cannot be measured without
it. `RtxTool::runHosted` states a fixed step for every verb, which settles `CompositeQueue`, which
makes every hand-over wait for the bakes it queued. On the crossing route that is 9.4 ms a frame of
a main thread that is asleep, and it is the harness's own wait rather than the game's.

Give `bench` a switch that leaves the queue unsettled. Keep the settled run for `verify`, for `shot`
and for the hashes, which is what it was added for.

## Considered and not proposed

**A flat open-addressed identity map.** It was the obvious answer to the 0.152 ms and Proposal 3 is
a better one: a lookup avoided beats a lookup made faster. The map also stops being on the frame
path once the trace lands, so making it fast would be optimising an arrival.

**Merging the mirror's walk with OSG's update traversal.** Two whole-graph walks a frame, and the
update one costs 0.176 ms at Seyda Neen. They cannot be merged: `SceneUtil::RigGeometry`,
`MorphGeometry` and `MWRender::CameraRelativeTransform` each `static_cast` the visitor on the
strength of what it claims to be, and `PoseCull`'s own doc warns about exactly this.

**Shading the sprites every second frame.** It would halve Proposal 1's cost with none of its work.
It is a rota, and `AGENTS.md` rules out work batched behind a threshold for the reason that applies
here: what a picture holds would depend on how many frames came before it, and `verify` compares
stills.

**Slabbing the sprite grid so a disc becomes a difference image.** It makes `layDown` `O(1)` a
sprite at the cost of losing self-shadowing inside a slab. That is an approximation the current code
does not make, and image quality is the first priority.

**A smaller `MeshInstance`.** It is 80 bytes with a 64-byte matrix in it, and `mPrevious` is another
64 beside it. Neither is read per drawable once Proposal 3 lands, so the layout stops mattering.

## The plan

Each step is a commit. Each states what it is verified by. The determinism gate runs after every
step that touches anything a frame reads, which is all of them.

**The gate for every step**

```
cmake --build build-debug -j32 --target components-tests openmw-rtxtool
build-debug/components-tests --gtest_filter='Rtx*'
apps/rtxtool/repeatable.sh --pairs=10
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

and, for anything that could move a picture, a `bench --hashes` against the previous build's run.

### Stage 1 — the measurement can be trusted

1. **Proposal 8.** `bench --settled=false`, defaulting to true so nothing else changes. Re-take the
   `streaming` row under it and write both readings into `.notes/bench.txt`.
   *Verified by*: the two readings, and `repeatable.sh` still green on the settled path.

### Stage 2 — Vivec stops being CPU-bound

2. **`spriteshade.comp`, cross-checked.** A new pass beside `spritebin`, a `spriteshade.h` shared
   header, and the host `SpriteShade` kept as the reference. `binSprites` runs the device pass and,
   under `--validation`, runs the host one too and asserts the two agree to within a float's last
   places.
   *Verified by*: the seven existing tests in `apps/components_tests/rtx/spriteshade.cpp`, unchanged
   and still against the host implementation; a new test that the fallback path and the device path
   describe the same emitters; `shot --view=vivec` before and after, compared as `Rtx::Channel::Radiance`
   floats with `--exposure=1`.
   *Expected*: Vivec's host frame 7.03 ms to about 4.6.

### Stage 3 — the walk stops re-deriving what it knows

3. **Liveness by slot (Proposal 2).** `Kept` splits into an index and a slot life. No behaviour
   changes and no picture changes.
   *Verified by*: the extractor's existing tests; `scene --twice` still reports that a second walk
   adds nothing; `repeatable.sh`.

4. **The trace, resolving only (Proposal 3, first half).** Record the trace and use it for the three
   slot lookups. Keep `PlacementTable::move` as it is.
   *Verified by*: a new test that a second walk over an unchanged graph makes no map lookup at all,
   counted; the allocation guard, since the trace must be scratch and never a per-frame allocation;
   `repeatable.sh`.
   *Expected*: 0.12 ms of the Seyda Neen walk.

5. **The trace carries the transform (Proposal 3, second half).** Compare in the trace and reach
   `PlacementTable` only on a difference.
   *Verified by*: the same tests; a test that a moved placement still reaches `getMoved` exactly
   once.
   *Expected*: a further 0.06 ms.

6. **The chunk replay (Proposal 4).** First the assertion — a debug-only check that a chunk handed
   over twice under one name and one node address walks to an identical run of trace entries. Then
   the replay behind it.
   *Verified by*: the assertion, run over `island-crossing` and `seyda-neen-ship` under
   `build-debug`; `check` at every place of every suite; `repeatable.sh`.
   *Expected*: 0.45 ms at Seyda Neen, 0.70 at Vivec.

### Stage 4 — the light path

7. **The light revision (Proposal 5).** The swap in `clearPlacement`, the compare in `addLight`, and
   the gates in `orderLights`, `LightGrid::rebuild` and `SceneBuffers::place`.
   *Verified by*: a test that a lamp moved by one unit raises the flag and a lamp that did not does
   not; a test that the light list written under the gate equals the one written without it;
   `repeatable.sh`.
   *Expected*: 0.15 ms at Seyda Neen.

8. **The `fadeThrough` guard (Proposal 6).** One `getUniformList().empty()` test. It is last of this
   stage because Proposal 4 has by then taken the chunk subtrees out of its call count, so what
   remains is what it is really worth.
   *Verified by*: a test that a state set carrying `actorFade` still fades and one carrying no
   uniform still inherits; `repeatable.sh`.
   *Expected*: 0.05 ms at Seyda Neen, once Proposal 4 has landed.

### Stage 5 — the crossing

9. **`FoldCache` (7b).** The cache, the warming thread's fold, and `MeshResolver::resolve` draining
   it.
   *Verified by*: a test that a folded-on-a-thread geometry and a folded-on-the-frame geometry give
   identical indices and an identical `FoldedShape`; `bench --views=island-crossing --settled=false`
   before and after; `repeatable.sh`.

10. **The `mBuilding` wait, measured (7a).** Report the acquisition time beside `walk`. No change to
    the lock itself without a number, and none at all without answering `TerrainResidency`'s own doc
    on why it is there.
    *Verified by*: the number; and, should a change follow it, the instance and mesh counts over five
    runs of `bench --views=seyda-neen-shore`, which is the measurement the lock was added against
    and must stay exact.

11. **The lead, re-measured (7c).** A sweep of `sLeadSteps` at 15, 30, 60 and 120 on the unsettled
    crossing route, three legs each, interleaved.
    *Verified by*: the numbers, written into `.notes/bench.txt`.

### Stage 6 — read the result

12. Re-run every suite and re-take the profiles. Update `.notes/rtx/cpu-performance.md` with the
    new table and say which proposals paid and which did not.

## What this is expected to come to

Median milliseconds of host time a frame, `frame - wait`. The "after" column is the sum of the
estimates above and is not a measurement.

| view | now | expected | device time |
|---|---:|---:|---:|
| vivec | 7.03 | ~3.6 | 5.34 |
| balmora-fog-night | 3.08 | ~2.3 | 4.85 |
| seyda-neen-ship | 2.87 | ~2.1 | 5.29 |
| balmora-mages-guild | 1.13 | ~1.0 | 4.81 |

Overlaps are taken out rather than summed: `TerrainResidency::collect` is 49 per cent of the Seyda
Neen walk and 41 per cent of Vivec's, so Proposals 3 and 6 are counted only over the share of the
walk that Proposal 4 does not already replay.

**The crossing row is left out on purpose.** Its 14.10 ms mean is 9.4 ms of a main thread asleep in
`CompositeQueue::finish`, which is the harness settling itself and not the game. Proposal 8 is what
makes that row mean something, and until it lands there is no honest "after" to write in it. What
can be said is that of the 4.72 ms the crossing's main thread actually spends on a core, Proposal 7b
removes 1.32.

No place would then be CPU-bound. That is the point of the work: not a faster host, but a host that
is never the answer to "why was this frame slow".

## What the plan came to

Written 2026-09-09, after the steps below were built, tested and measured. The readings are in
`.notes/bench.txt`.

### Landed

| step | proposal | expected | measured |
|---|---|---:|---:|
| 1 | 8, `bench --settled=false` | — | 4.2 ms a frame of the streaming row was the harness's own wait |
| 2 | 1, the sprite shading on the device | 2.4 ms at Vivec | **3.3 ms** |
| 7 | 5, the light grid gated | 0.15 ms | 0.08 to 0.17 ms, in `place` |
| 8 | 6, `fadeThrough` guarded | 0.05 ms | 0.10 to 0.40 ms, in `walk` |

**Vivec's host frame is 3.89 ms against 5.76 ms of device time.** Every one of the eighteen views
came down, none went up, and no place in the corpus is CPU-bound any more. That was the whole point
of the work, and it is met.

### The host implementation is gone rather than kept

The proposal said the host `SpriteShade` would stay as the reference the device pass is
cross-checked against, and shade any emitter longer than one workgroup's shared memory could sort.
That is two implementations of one computation, which is two things to keep in step and a place for
them to disagree. It is now one.

**What made the cap unnecessary.** The cap existed because a bitonic network is defined on a power
of two and has to be padded to one, which needs room past the run. Two changes remove it: the depth
order is sorted into a device buffer instead of into shared memory, and the network is Batcher's
odd-even merge instead of a bitonic sort. Odd-even merge is the power-of-two network with every
comparator touching a wire past the run removed, which is exact because every comparator runs the
same way — a missing wire is the largest key there is, and the smaller of a key and the largest key
is the key. So a run of any length is sorted by the one network, with no padding and no cap.

The move made the shader smaller on every count: shared memory 24 KiB to 8, registers 44 to 36,
binary 7552 bytes to 4608. Vivec's host frame came to 3.71 ms and the device zone to 0.12.

**What stands in the reference's place.** The eleven tests were rewritten to drive the device pass,
and every hand-computed expectation in them is unchanged — the disc's analytic coverage at its rim,
the layers adding along the light, the tie broken on the index. Two were added for what the cap's
removal made possible: a run of every length from two to thirty-three coming out in depth order, and
several emitters in one table shaded apart. The cross-check test is gone with the thing it was
checking against.

### Two proposals were wrong, and the measurement is what said so

**Proposal 5 as written does not work.** It gated `orderLights`, the grid and the buffer writes on
a light array that had not changed. Morrowind's lamps flicker: `lightBrightness` moves
`Light::mIntensity` on nearly every frame of nearly every lit place, so the array changes almost
always and the gate would almost never fire.

What does work is narrower and exact. The grid is a function of each lamp's position and reach and
of nothing else — it never reads a colour — so `LightGrid::rebuild` keeps its own copy of that
sequence and returns where it matches. A lamp that only flickered keeps the grid it had, and one
that moved is binned again. The sort and the buffer writes stay, because those do read the colour.

**Proposal 1 was underestimated by a third.** The estimate counted `layDown`; what actually moved
was the depth sort and the projection with it, because a workgroup does all three.

**And its fallback was a mistake.** See above: a cap on the device side is what invented the second
implementation, and the cap was an artefact of where the sort's keys were put.

### Not landed, and why

**Proposals 2, 3 and 4 — the liveness split, the walk trace and the chunk replay.** These are the
largest remaining item on paper, 0.6 to 0.9 ms of an exterior walk. They are left because the
measurement moved out from under them: with no place CPU-bound and 1.5 to 2.9 ms of host headroom
at every view, a change to the middle of `SceneExtractor` — where a stale entry mirrors the wrong
geometry — buys a fraction of a millisecond nothing is waiting for. The design in Proposal 3 stands
and the staged plan under it stands; what has changed is that it is no longer urgent.

There is also a finding for whoever picks it up. Proposal 2 was written to make a slot index a
sufficient handle for the sweep, and six of the seven identity maps have a slot space — but
`MaterialResolver::mAnimated` has none, and the sweep has to walk each map regardless to drop the
key whose slot died. So the dense epoch saves the scattered write during the walk and not the map
walk at the sweep, which is a smaller prize than the proposal claimed.

**Proposal 7b — the fold on the warming thread.** Blocked on a seam rather than on the work.
`TerrainResidency` holds a `Terrain::View*`, and reaching the chunks that `Terrain::World::preload`
built into it needs a `Terrain::ViewData*` — a cast this fork would be making on the strength of
its world happening to be a `QuadTreeWorld`, which is exactly the shape `AGENTS.md` rules out.
`Terrain::World::collect` hands the chunks over through the interface instead, and would do it
without a cast, but it has no per-chunk abort where `preload` has one — so using it on the warming
thread gives back the yield that bounds what a frame waits for.

Neither answer is right as it stands. What is needed first is 7a's measurement: how long the frame
actually spends taking `mBuilding`. If that wait is small, the warming thread can afford the second
traversal `collect` costs and the seam problem goes away.

**Proposals 7a and 7c** are measurements and are untaken.
