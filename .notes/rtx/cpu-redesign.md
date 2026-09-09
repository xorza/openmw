# What is left of the CPU redesign

2026-09-09. What this file held before was a proposal and a twelve-step plan; four of the steps
landed and this is what remains. The readings are in `.notes/bench.txt`, the measurement that
started it is `.notes/rtx/cpu-performance.md`, and the profiles below were re-taken after the
landed work rather than carried over from it.

## Where the host stands now

Medians in milliseconds. `host` is `frame - wait`, `gpu` is the sum of the frame's device zones.

| view | host | gpu | walk | place |
|---|---:|---:|---:|---:|
| vivec | 3.89 | 5.76 | 1.76 | 0.82 |
| balmora-fog-night | 2.60 | 4.57 | 1.07 | 0.48 |
| seyda-neen-ship | 2.38 | 5.03 | 0.98 | 0.44 |
| seyda-neen-shore | 1.93 | 5.09 | 0.97 | 0.17 |
| balmora-mages-guild | 1.10 | 4.49 | 0.28 | 0.21 |
| arkngthand | 0.85 | 3.71 | 0.21 | 0.11 |

**No place in the corpus is CPU-bound.** Every `host` is under its own `gpu`, where Vivec's was
7.03 against 5.34. So nothing below is urgent, and the order they are written in is what buys the
most for the least risk rather than what is largest.

**The crossing is the exception and it is not in the table.** `island-crossing` measured
`--settled=false` reads 5.97 ms median, 77.67 at the p99 and 157.77 at the worst, with 12.9 fps at
the one per cent low. That row is what Proposal 4 is about.

## Landed already

Recorded in `.notes/bench.txt` with their readings, and named here only so a reader knows why the
biggest items are missing.

- **The sprite shading moved to the device.** 3.3 ms at Vivec, which is what took the one CPU-bound
  place off the list. The host implementation was deleted rather than kept as a reference.
- **The light grid is gated on where the lamps stand.** 0.08 to 0.17 ms of `place`, and see the
  short reading below.
- **`fadeThrough` asks the uniform list before the two names.** 0.10 to 0.40 ms of `walk`.
- **`bench --settled=false`.** Not a saving: it is what makes the streaming row mean anything.

## Proposal 1 — liveness moves from the map entry to the slot

**What it costs now.** Every drawable does three `std::unordered_map` lookups a frame:
`mMeshes.find`, `mMaterials.find` through `reuse`, and `mPlacements.find`. The hash-table lines
together are 0.143 ms a frame at Seyda Neen, against 0.249 ms for the whole of `addDrawable`,
`MeshResolver::resolve` and `MaterialResolver::reuse` in self time. **More than half of what it
costs to resolve a drawable is the three lookups.**

**Why they are there.** They are not there to find the slot. They are there so that the sweep can
tell what the walk met. `Kept` stamps each map entry with the epoch and `retire` drops the entries
that carry an older one.

**The redesign.** Split the two jobs the map is doing.

- **An arrival index**: `key -> slot`, consulted only when something is met for the first time, or
  when the trace of Proposal 2 misses. No epoch in it.
- **A slot life**: a dense `std::vector<std::uint32_t>` of epochs, one per slot of the table it
  belongs to, beside a count of how many slots this epoch has reached.

A walk then stamps by slot — `mLife[slot] = epoch` — which is a write into a dense array at an
address the walk usually already holds. The `whole()` shortcut survives unchanged: the count
against the live count.

**What it does not save, which the first writing of this got wrong.** Six of the seven identity
maps have a slot space and `MaterialResolver::mAnimated` has none, so that one keeps its own epoch.
And the sweep still has to walk each map to drop the key whose slot died — a slot index alone
cannot say which key named it. So this saves the scattered write during the walk and not the map
walk at the sweep.

**Expected.** 0.05 ms a frame at Seyda Neen on its own. It is a means, not the win: what it buys is
that a slot index becomes a sufficient handle, so Proposal 2 can hold slot indices rather than map
iterators — and iterators are what would otherwise tie the design to a node-based map for ever.

**Risk.** Low, and bounded to `components/rtx/mirroridentity.hpp` and its four users. The sweep
order changes from bucket order to slot order, which is a change in *nothing* the picture reads:
`SlotRows::takeLowest` already makes the slot a function of what is free rather than of what was
freed last.

## Proposal 2 — the walk keeps a trace, and a still world reads it

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

**What it removes.** The three hash lookups, 0.143 ms of the recording, and the random 80-byte read
of the instance row that `PlacementTable::move` makes to compare a transform. Both become one
sequential stream. It does not remove the traversal itself, nor the state sets pushed at every
*node* on the way down — the trace has one entry per drawable, and those are Proposal 3's.

**Expected.** 0.16 ms of the 1.04 ms walk at Seyda Neen, and about 0.28 ms of Vivec's 1.73.

**Risk.** Moderate, and the highest of anything left here. A trace that goes stale silently would
mirror the wrong geometry. Three guards: the identity compare is the primary one; a generation
counter on the extractor invalidates the whole trace whenever `retire` or `abandon` erases anything;
and a debug-only pass re-resolves through the maps and asserts that the trace agreed, which
`--validation` can carry.

## Proposal 3 — a paged chunk that did not change is not walked again

**What it costs now.** `TerrainResidency::collect` is 0.499 ms a frame at Seyda Neen — 48 per cent
of the walk — 0.676 ms at Vivec and 2.49 ms on the crossing. Almost all of it is the mirror
descending the chunk subtrees that `QuadTreeWorld::handOver` gives it. `Group.cpp:63`, the child
loop, is 0.209 ms at Seyda Neen and the hottest single line in the profile.

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
which is nearly every frame of a standing or slowly moving camera. Call it 0.40 ms of Seyda Neen's
0.499 and 0.55 ms of Vivec's 0.676. It buys nothing on the crossing route, where every chunk is new.

**Risk.** Moderate. It rests on paged chunks being immutable, which the tree states in three places
and nowhere asserts. The plan below adds the assertion before it adds the replay.

## Proposal 4 — arrival work leaves the frame

**What it costs now.** On the crossing route the main thread's 4.33 ms on a core holds
`ObjectPaging::createChunk` at 1.53 ms and `ShapeFold` at 1.32 ms. Both are work a chunk owes once,
paid on whichever frame first asks for it.

**4a. Measure what the frame waits for the warming thread.** `TerrainResidency::collect` sets
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
beside `walk`. If it is small, 4a is closed. If it is not, the answer is a finer yield inside the
thread's own loop, not a lock the frame may skip.

**4b. The warming thread folds what it builds.** The fold is a pure function of a geometry's
positions and indices, and the warming thread already holds the built chunk before any frame sees
it. A `FoldCache` — a mutex, and a map from `const osg::Geometry*` to a `FoldedShape` and the folded
index run — is filled by the thread and drained by `MeshResolver::resolve`. `WorldMirror` owns it
and hands it to both, so neither reaches into the other.

**This changes no picture at all**, which is the property that makes it the safest of the three:
`osg::TriangleIndexFunctor` and `ShapeFold::fold` are deterministic, so a fold computed on a thread
and a fold computed on the frame give the same indices and the same `FoldedShape`.

**And it is blocked on a seam rather than on the work.** `TerrainResidency` holds a
`Terrain::View*`, and reaching the chunks `preload` built into it needs a `Terrain::ViewData*` — a
cast this fork would be making on the strength of its world happening to be a `QuadTreeWorld`, which
is the shape `AGENTS.md` rules out. `Terrain::World::collect` hands the chunks over through the
interface instead and would need no cast, but it has no per-chunk abort where `preload` has one, so
using it on the warming thread gives back the yield that bounds what a frame waits for. **4a is what
decides between them**: if the wait is small, the thread can afford the second traversal `collect`
costs and the seam problem goes away.

**4c. The warming lead is measured again.** `sLeadSteps` is thirty and the class doc records that
sixty made the median worse. That reading was taken before the thread had anything else to do. Take
it again after 4a and 4b, and take it at 12,000 units a second, which is the speed the lead is
failing at.

**Expected.** 4b removes up to 1.32 ms of the crossing route's 4.33 ms of main-thread compute. 4c
decides how much of the 1.53 ms of `ObjectPaging::createChunk` follows it. Neither buys anything at
a standing camera.

**Risk.** 4b is low and changes no picture. 4a and 4c are measurements.

## Two smaller things, found while implementing the rest

**`binSprites` writes the whole sprite table every frame.** That write existed because the host had
just shaded into `mSpriteScratch`; nothing shades there now, so `place` could write it and
`binSprites` skip it — about 413 KB a frame at Vivec, to write-combining memory. It moves the sprite
table across the placement-against-frame synchronisation boundary, whose reasoning is written out at
the `renderFrame` call site, so it wants its own change and its own reading of that comment.

**Five passes push constants with no descriptors** — `micromappass`, `skinpass`, `spritebinpass`,
`spriteshadepass`, `wavepass` — and `spritebinpass` keeps a local `dispatch` that is
`dispatch.hpp`'s minus the descriptor push. One shared descriptor-free form would serve all five.
Not a performance item; it is here so it is not lost.

## One reading that came out short

**The light grid's gate does not fire at Seyda Neen.** `LightGrid::rebuild` costs 0.080 ms a frame
there, against 0.081 before the gate — so the sequence of positions and reaches differs on every
frame at that view, while at Vivec, Balmora, Sadrith Mora, Dagon Fel and the shore the gate fires
and takes 0.08 to 0.17 ms off `place`.

**What has not been established is why.** The likely cause is that `SceneDesc::orderLights` sorts on
position first, so one lamp that moves lands at a different index and carries every lamp after it to
a different index too — which the element-wise compare then reads as a whole world that moved. If
that is it, the question is whether the sort's key can drop what flickers without losing the total
order it exists to give. Measure before designing: count the frames on which the gate fires, at each
view of the default suite.

## The plan

Each step is a commit. Each states what it is verified by.

**The gate for every step**

```
cmake --build build-debug -j32 --target components-tests openmw-rtxtool
build-debug/components-tests --gtest_filter='Rtx*'
apps/rtxtool/repeatable.sh --pairs=10
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

and, for anything that could move a picture, a `bench --hashes` against the previous build's run.

### Stage 1 — the crossing

1. **The `mBuilding` wait, measured (4a).** Report the acquisition time beside `walk`. No change to
   the lock without a number, and none at all without answering `TerrainResidency`'s own doc on why
   it is there.
   *Verified by*: the number, written into `.notes/bench.txt`.

2. **`FoldCache` (4b)**, by whichever route step 1 leaves open.
   *Verified by*: a test that a folded-on-a-thread geometry and a folded-on-the-frame geometry give
   identical indices and an identical `FoldedShape`; `bench --views=island-crossing --settled=false`
   before and after; `repeatable.sh`.
   *Expected*: up to 1.32 ms of the crossing's main-thread compute.

3. **The lead, re-measured (4c).** A sweep of `sLeadSteps` at 15, 30, 60 and 120 on the unsettled
   crossing route, three legs each, interleaved.
   *Verified by*: the numbers, written into `.notes/bench.txt`.

### Stage 2 — the walk stops re-deriving what it knows

4. **Liveness by slot (Proposal 1).** `Kept` splits into an index and a slot life. No behaviour
   changes and no picture changes.
   *Verified by*: the extractor's existing tests; `scene --twice` still reports that a second walk
   adds nothing; `repeatable.sh`.

5. **The trace, resolving only (Proposal 2, first half).** Record the trace and use it for the three
   slot lookups. Keep `PlacementTable::move` as it is.
   *Verified by*: a new test that a second walk over an unchanged graph makes no map lookup at all,
   counted; the allocation guard, since the trace must be scratch and never a per-frame allocation;
   `repeatable.sh`.
   *Expected*: 0.11 ms of the Seyda Neen walk.

6. **The trace carries the transform (Proposal 2, second half).** Compare in the trace and reach
   `PlacementTable` only on a difference.
   *Verified by*: the same tests; a test that a moved placement still reaches `getMoved` exactly
   once.
   *Expected*: a further 0.05 ms.

7. **The chunk replay (Proposal 3).** First the assertion — a debug-only check that a chunk handed
   over twice under one name and one node address walks to an identical run of trace entries. Then
   the replay behind it.
   *Verified by*: the assertion, run over `island-crossing` and `seyda-neen-ship` under
   `build-debug`; `check` at every place of every suite; `repeatable.sh`.
   *Expected*: 0.40 ms at Seyda Neen, 0.55 at Vivec.

### Stage 3 — read the result

8. Re-run every suite and re-take the profiles. Update the table at the head of this file and say
   which proposals paid and which did not.

## What this is expected to come to

Median milliseconds of host time a frame. The "after" column is the sum of the estimates above and
is not a measurement. Overlaps are taken out rather than summed: `TerrainResidency::collect` is 48
per cent of the Seyda Neen walk and 39 per cent of Vivec's, so Proposal 2 is counted only over the
share of the walk that Proposal 3 does not already replay.

| view | now | expected | device time |
|---|---:|---:|---:|
| vivec | 3.89 | ~3.1 | 5.76 |
| seyda-neen-ship | 2.38 | ~1.9 | 5.03 |
| balmora-mages-guild | 1.10 | ~1.0 | 4.49 |

**The crossing is what the work is really for.** Nothing above moves it much at the median; what
moves it is Stage 1, and what that is worth is 1.3 ms of a 4.3 ms frame plus whatever the lead
turns out to be. The p99 of 77.67 ms is the number to watch, not the median.
