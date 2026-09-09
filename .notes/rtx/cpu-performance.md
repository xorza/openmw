# The CPU side of the RTX frame

What the host spends a frame on, what is wrong with it, and what to do. Taken at `7b1c6a7402` on
an RTX 4090 Laptop and an i9-13980HX, release build, `bench --seconds=20`. The crossing is measured
`--settled=false`, without which the row is the harness's own composite wait rather than the game's.

## Where it stands

Medians in milliseconds. `host` is `frame - wait`, `gpu` is the sum of the frame's device zones.

| view | host | gpu | walk | place |
|---|---:|---:|---:|---:|
| vivec | 3.89 | 5.76 | 1.76 | 0.82 |
| balmora-fog-night | 2.60 | 4.57 | 1.07 | 0.48 |
| seyda-neen-ship | 2.38 | 5.03 | 0.98 | 0.44 |
| seyda-neen-shore | 1.93 | 5.09 | 0.97 | 0.17 |
| balmora-mages-guild | 1.10 | 4.49 | 0.28 | 0.21 |
| arkngthand | 0.85 | 3.71 | 0.21 | 0.11 |

**No place that stands still is CPU-bound.** Every `host` is under its own `gpu`, with 1.7 to 2.9 ms
of headroom. Nothing in this document is about them.

**One place moves, and it is the whole problem.** `island-crossing` flies the Bitter Coast to the
Ashlands at 12,000 units a second:

| | median | mean | p95 | p99 | worst |
|---|---:|---:|---:|---:|---:|
| frame | 5.85 | 10.87 | 33.02 | 78.73 | 153.83 |
| walk | 1.14 | 2.98 | 12.63 | 32.40 | 84.57 |
| place | 0.18 | 2.63 | 13.41 | 25.09 | 61.71 |
| wait | 2.27 | 1.61 | 3.29 | 4.39 | 20.22 |

12.4 fps at the one per cent low, against 160 at the median. **The worst frame is a walk of 85 ms
and a place of 62 ms**, which is a whole cell ring arriving inside one frame.

## What a crossing frame is made of

Milliseconds a frame, averaged over the run, from `profile.sh --view=island-crossing
--settled=false`. Indentation is containment.

| | ms |
|---|---:|
| `OMW::Engine::frame` | **4.34** |
| — `WorldMirror::mirror` | 2.95 |
| — — `TerrainResidency::collect` | 2.50 |
| — — — `QuadTreeWorld::loadRenderingNode` | 1.77 |
| — — — — `ObjectPaging::createChunk` | 1.54 |
| — — `SceneExtractor::addDrawable` | 1.95 |
| — — — `MeshResolver::resolve` | 1.68 |
| — — — — `ShapeFold::fold` | 1.31 |
| — `WorldMirror::hand` | 0.30 |
| *on worker threads* | |
| `CompositeQueue::bake` | 4.39 |
| `QuadTreeWorld::preload` | 1.66 |
| `SceneUtil::WorkThread::run` | 1.52 |

**And the spike is work, not a wait.** `profile.sh --offcpu` puts every sleep of the main thread
over a millisecond at 405 ms across 1200 frames, and 291 ms of that is `Session::beginStop` — the
harness teleporting between stops, outside any measured frame. Outside it the longest single sleep
is 20 ms and about a dozen pass 2 ms. No 150 ms frame of this route is one long wait: not the ring
wait in `placeScene`, not a cell load, nothing.

**One thing is not accounted for.** `place` has a median of 0.18 ms against a mean of 2.63, so its
cost is a handful of arrival frames — and the on-CPU profile cannot attribute them, because they run
in the driver, which carries no frame pointer. What is visible of it is the driver's own allocate
and free at about 1.1 ms a frame: `Nv04VidHeapControl` into `nv_alloc_system_pages` at 0.66 and
`rmapiFree` at 0.41.

## The finding: the warming thread cannot work, by construction

`TerrainResidency` runs a thread that warms the quad tree ahead of the eye, so that the chunks the
next frames want are built before they are asked for. It does not do that, and it cannot.

**A chunk's identity depends on where it was resolved from.** `QuadTreeNode::traverseNodes` splits
on `isSufficientDetail(this, distance(viewPoint))`, so the *decomposition* — which nodes exist and
how wide each is — is a function of the view point. `ObjectPaging`'s cache is keyed on
`(centre, size, activeGrid)`. A thread that resolves from a point ahead of the eye produces a
different decomposition, and the chunks it builds have sizes the frame never asks for.

**The measurement says exactly this.** A sweep of `sLeadSteps` at 1, 15, 30, 60 and 120, three legs
interleaved, reads the same at every value: the spread across leads is inside the spread across legs
at any one of them. A lead of one warms essentially where the eye already is and reads the same as a
hundred and twenty. And against a build that never starts the thread at all, the thread is worth
0.2 ms of the walk's mean and nothing at the tail — which is the few chunks whose decomposition
happens to coincide.

So the thread burns a core building chunks nobody asks for — `preload` at 1.66 ms a frame beside the
frame's own 1.54 — and the frame builds the ring itself, in one frame.

## Proposal 1 — one thread owns the quad tree, and the frame reads what it published

**The shape.** The frame stops entering the quad tree at all. A worker resolves it, builds every
chunk, folds every geometry it finds, and publishes the set. The frame takes the newest published
set, hands it to the mirror, and posts its own view point for the worker's next round.

- **The worker resolves at the frame's own view point**, not at a lead. That is the whole of what
  makes it land: the decomposition it builds is the one the frame asked for, so nothing is built
  that nothing wants.
- **The frame's terrain lags by one round.** In steady state that is one frame. At a crossing it is
  as many frames as the ring takes to build — which is the stall, turned into a delay.
- **`mBuilding` goes.** Only one thread enters `QuadTreeWorld::loadRenderingNode`, so the lock that
  keeps the two out of its caches has nothing to guard, and `warm ms` becomes the wait for a
  snapshot rather than for a chunk.
- **The fold goes with it.** The worker holds each chunk's geometry before any frame sees it, so it
  runs the triangle collector and `ShapeFold` there and publishes the folded index runs beside the
  nodes. `MeshResolver` takes them instead of folding.

**What it removes from the crossing's frame.** `loadRenderingNode` at 1.77 ms and `ShapeFold` at
1.31, of a 4.34 ms main thread. It buys nothing at a standing camera, where nothing is arriving.

**What it costs the picture.** A chunk appears a frame or more after the frame that first wanted it.
At the reach's edge — 32,768 units at `distant-cells=4` — a chunk entering the view has two thirds
of a second before the eye is a cell closer, so a few frames of it is a handful of pixels arriving
late at the horizon. Against that, the stall it replaces is 150 ms.

**What it costs determinism.** Which frame a chunk lands on becomes a thread's answer rather than
the schedule's, which is the same problem `CompositeQueue::setSettled` already solves for the ground
bake and by the same means: a settled mode that waits for the round, used by `verify`, `shot` and
the hashes, and off for a run that is timing the streaming path.

**The seam.** `Terrain::World::collect(View*, viewPoint, ChunkTaker&)` is the whole interface the
worker needs — it resolves the view, builds every entry and hands each over with its `ChunkName`.
Nothing is cast and no upstream file is touched. `TerrainResidency` already owns two views; one of
them becomes the worker's and the other is not needed.

**Risk.** Moderate, and it is the largest change here. What it rests on is that a published chunk
node is immutable and safe to walk from another thread once built, which is the same claim
`ObjectPaging`'s cache already rests on.

## Proposal 2 — `place` is one row and needs to be four

`place` is 0.18 ms at the median and 62 ms at the worst, and nothing says which half of
`SceneUploader::hand` the 62 is. The profile cannot answer it: an arrival frame's time is in the
driver, and the driver has no frame pointers for perf to walk.

**Split the row the way `walk` is split.** `hand` runs a sequence with names already —
`composites->advance`, `mTextures.describe`, `extendScene`, `placeScene` — and timing each on the
host costs four `steady_clock` reads on a path that already takes two. The report gains a line and
the question becomes answerable.

**Expected.** Nothing. It is an instrument, and it is here because the second-largest term in the
crossing's frame is currently unattributable.

## Proposal 3 — the walk reads what it wrote last frame

**What it costs now.** At Seyda Neen the walk is 0.98 ms, of which `TerrainResidency::collect` is
0.499 — and with Proposal 1 that becomes walking the published chunks rather than building them, so
it stays. The three `std::unordered_map` lookups every drawable makes are 0.143 ms a frame, against
0.249 ms for the whole of `addDrawable`, `MeshResolver::resolve` and `MaterialResolver::reuse` in
self time. `Group.cpp:63`, the child loop, is 0.209 ms and the hottest single line in the profile.

**The shape.** The walk visits the same nodes in the same order every frame — `descend` walks
children in order, a `Switch` in branch order, a residency in cell order — and a cell of 4916
placements has 138 deforming drawables in it. So the walk keeps a trace, one entry per drawable in
walk order, holding the path's identity fold, where the drawable stood, its material key and its
three slots. `addDrawable` takes a cursor: where the identity at the cursor matches the one just
folded, it has the slots without touching a map, and it compares the transform in the trace rather
than reaching into `PlacementTable`. A mismatch falls back to the maps and rebuilds the trace from
there.

**And a subtree the walk can prove unchanged is not descended at all.** A paged chunk is immutable
once built and carries no light, no particle system and no callback, so a chunk published under the
same `ChunkName` and the same node replays its run of trace entries rather than being walked. The
run records whether the subtree held anything but transforms, groups and plain drawables, and one
that did is never replayed.

**Expected.** About 0.16 ms of the trace and 0.40 ms of the replay at Seyda Neen, 0.28 and 0.55 at
Vivec.

**Risk.** The highest here. A trace that goes stale silently mirrors the wrong geometry. Three
guards: the identity compare, a generation counter that invalidates the whole trace whenever a sweep
erases anything, and a debug-only pass that re-resolves through the maps and asserts the trace
agreed.

**And it is not urgent.** It is 0.6 ms on a host with 1.7 to 2.9 ms of headroom at every view that
stands still. It is here because it is the largest steady item, not because anything waits on it.

## Considered and not proposed

**Fixing the warming thread's aim.** No aim works: the decomposition follows the view point, so only
a lead of nought builds the frame's own chunks, and a lead of nought has no head start. Proposal 1
is that observation taken to its conclusion.

**A fold cache filled by the warming thread.** It moves one of the two arrival costs and depends on
the thread landing, which it does not. Proposal 1 subsumes it.

**Slicing the arrival over frames.** `AGENTS.md` rules out work batched behind a threshold, and for
the reason that applies here: what a picture holds would depend on how many frames came before it.
Off the frame path entirely is the alternative the same rule names.

**Shrinking the buffers between arrivals.** They already grow and never shrink — `growTo` returns
early where the buffer is big enough. Whatever the driver is allocating in an arrival frame, it is
not a table being resized down and up.

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

### Stage 1 — see the other half of the spike

1. **Split `place` (Proposal 2).** Four rows where there is one.
   *Verified by*: the rows summing to `place` within a tenth of a millisecond over a run; the
   numbers, written into `.notes/bench.txt`.

### Stage 2 — the crossing

2. **One fold, held by two.** Lift what `MeshResolver::resolve` does between reading a drawable and
   adding a mesh — the triangle collector and `ShapeFold` over it — into a type both the resolver and
   the terrain worker hold an instance of. No behaviour changes.
   *Verified by*: the extractor's tests, and `scene` reporting the same mesh, sheet and triangle
   counts at every view of the default suite; `repeatable.sh`.

3. **The worker resolves and publishes (Proposal 1), the frame still folding.** The snapshot, the
   view point posted back, `mBuilding` removed, and the frame reading what was published. Settled
   mode waits for the round.
   *Verified by*: `scene` reporting the same instance and mesh counts as before at every view of the
   default suite, which is what says the same world arrived; `check` at every place of every suite;
   `repeatable.sh --pairs=10` under the settled path; `bench --views=island-crossing
   --settled=false` before and after, three legs interleaved.
   *Expected*: `loadRenderingNode`'s 1.77 ms a frame off the crossing's main thread.

4. **The worker folds what it publishes.** The folded runs travel with the nodes.
   *Verified by*: a test that a geometry folded on the worker and folded on the frame give identical
   indices and an identical `FoldedShape`; the same bench legs.
   *Expected*: `ShapeFold`'s 1.31 ms a frame with it.

5. **Read what Stage 1 now shows of `place`**, and propose against it.

### Stage 3 — the walk, if it is still worth it

6. **The trace (Proposal 3, first half).** Record it and use it for the three slot lookups.
   *Verified by*: a test that a second walk over an unchanged graph makes no map lookup at all,
   counted; the allocation guard, since the trace is scratch and never a per-frame allocation;
   `repeatable.sh`.

7. **The trace carries the transform**, and `PlacementTable` is reached only on a difference.

8. **The chunk replay (Proposal 3, second half).** First the assertion — a debug-only check that a
   chunk published twice under one name and one node walks to an identical run — then the replay
   behind it.

### Stage 4

9. Re-run every suite, re-take the profiles, and update this file.

## How to repeat the measurements

```
cd build-release
for suite in default exteriors skies interiors; do
    ./openmw-rtxtool bench --validation=false --window=false --suite=$suite --seconds=20 --json=$suite.json
done
./openmw-rtxtool bench --validation=false --window=false --suite=streaming --seconds=20 --settled=false

apps/rtxtool/profile.sh --view=island-crossing --settled=false
apps/rtxtool/profile.sh --offcpu --view=island-crossing --settled=false
```

Warm the card with one thrown-away `bench` first; the clock and the temperature are printed beside
every result, and a leg whose clock differs from its neighbour's is the leg to repeat.

**Read the off-CPU run by thread, not by the summary.** Its totals are dominated by driver workers
parked for the length of the run. What answers a question is the main thread's own stacks, and those
resolve through this fork's frames:

```
perf script -i build-release/perf/blocked.data --no-inline > /tmp/blocked.txt
awk 'BEGIN{RS=""} /Engine::frame/ { ... }' /tmp/blocked.txt
```

**Its threshold is a millisecond and that is the floor.** `--off-cpu-thresh` counts milliseconds, so
a sleep shorter than one is invisible, and there is no way below it.
