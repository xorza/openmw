# The CPU side of the RTX frame

What the host spends a frame on, what is wrong with it, and what to do. Taken at `5e8eb18563` on
an RTX 4090 Laptop and an i9-13980HX, release build, `bench --seconds=20`. The crossing is measured
`--settled=false`, without which the row is the harness's own composite wait rather than the game's.

**Read a profile by thread id and never by symbol.** The warming thread and the frame call the same
functions, and an earlier reading of this route summed a symbol over both and put the total under the
frame. Everything it then concluded about the terrain was wrong by about a factor of ten.

## Where it stands

Medians in milliseconds. `host` is `frame - wait`, `gpu` is the sum of the frame's device zones.

| view | host | gpu | walk | place |
|---|---:|---:|---:|---:|
| vivec | 4.75 | 6.09 | 2.23 | 0.85 |
| ald-ruhn | 3.01 | 4.90 | 1.26 | 0.46 |
| balmora | 2.86 | 4.98 | 1.38 | 0.40 |
| seyda-neen-ship | 2.83 | 5.32 | 1.26 | 0.45 |
| seyda-neen-shore | 2.40 | 5.40 | 1.32 | 0.18 |
| balmora-mages-guild | 1.23 | 4.88 | 0.30 | 0.21 |
| arkngthand | 1.08 | 3.95 | 0.28 | 0.12 |

**No place that stands still is CPU-bound.** Every `host` is under its own `gpu`, with 1.3 to 2.9 ms
of headroom — and vivec, the worst of them, has 1.3.

**One place moves, and it is the whole problem.** `island-crossing` flies the Bitter Coast to the
Ashlands at 12,000 units a second:

| | median | mean | p95 | p99 | worst |
|---|---:|---:|---:|---:|---:|
| frame | 6.45 | 11.71 | 35.66 | 81.69 | 164.52 |
| walk | 1.60 | 3.39 | 14.09 | 33.82 | 93.83 |
| place | 0.20 | 2.81 | 14.02 | 26.97 | 63.41 |
| wait | 0.96 | 1.53 | 3.64 | 5.17 | 21.28 |

12.2 fps at the one per cent low, against 155 at the median. **The worst frame is a walk of 94 ms
and a place of 63 ms**, which is a whole cell ring arriving inside one frame.

**And the device is not the limit.** Its zones sum to 5.76 ms a frame against 10.25 of host, so the
route is CPU-bound by nearly two to one. An arrival frame's device work is larger — a 5.97 ms
micromap bake on 178 frames of 1200 and a 2.08 ms structure build on 364 — and still nowhere near
the frames it lands in.

## What a crossing frame is made of

Milliseconds a frame, averaged over the run, from `profile.sh --view=island-crossing
--settled=false`. Indentation is containment.

| | ms |
|---|---:|
| **the main thread, on a core** | **8.57** |
| — attributed under `OMW::Engine::frame` | 4.92 |
| — — `MWRender::RtxRenderer::renderFrame` | 3.81 |
| — — — `WorldMirror::mirror` | 3.33 |
| — — — — `TerrainResidency::collect` | 2.79 |
| — — — — — `QuadTreeWorld::handOver` | 2.76 |
| — — — — — — `QuadTreeWorld::loadRenderingNode` | 0.16 |
| — — — — `SceneExtractor::addDrawable` | 2.14 |
| — — — — — `MeshResolver::resolve` | 1.80 |
| — — — — — — `GeometryFold::read` | 1.44 |
| — — — `SceneUploader::hand` | 0.34 |
| — — `MWWorld::World::update` | 0.63 |
| — inside `libnvidia-glcore` | 3.26 |
| *on worker threads* | |
| `CompositeQueue::bake` | 4.61 |
| `QuadTreeWorld::preload` | 1.80 |
| — `ObjectPaging::createChunk` | 1.53 |
| `SceneUtil::WorkThread::run` | 1.63 |

`collect` and `addDrawable` overlap: the collector walks each chunk it is handed, so `addDrawable` is
reached both through the graph and through `handOver`. **99% of the folding is chunk geometry** —
1.42 ms of 1.44 — and 92% of `addDrawable` is, 1.96 of 2.14. The game's own graph is 0.5 ms of the
walk and the paged chunks are the rest.

**And the spike is work, not a wait.** The main thread is on a core for 8.57 ms of an 11.80 ms
frame, and `wait ms` accounts for 1.55 of the remaining 3.23 — so 1.7 ms a frame is an uncounted
block and everything else is the CPU running. `profile.sh --offcpu` agrees: every sleep over a
millisecond comes to 405 ms across 1200 frames, and 291 of that is the harness teleporting between
stops, outside any measured frame.

**A third of the main thread is inside the driver and no unwinder reaches it.** 3.26 ms a frame is
in `libnvidia-glcore`, 1.41 of it down the kernel's ioctl path. Frame pointers stop at the driver's
first frame, and `--dwarf` does worse: it reaches `Engine::frame` on 4% of samples against 24%. What
names driver time is a `steady_clock` around the call that enters it.

**Where `place` went, timed from inside the backend.** Over the island route, by phase and summed
over the run: the report at the end of `extendScene` **1561.7 ms**, the acceleration structures the
arrivals created 826.3, the textures they wrote 539.9, the placement's own recording 245.9, the ring
wait in `placeScene` **0.1**, the textures the departures dropped 0.1.

So the spike was neither the device nor the recording. `readStats` called
`BottomLevelStore::getCompactableBytes`, which read the compacted-size queries with
`VK_QUERY_RESULT_WAIT_BIT` — standing the CPU still until the builds that frame had just recorded had
run. 3.5 ms an arrival, for a figure the report prints once. The compaction path in the same file
already forbids exactly this and reads the same pool without waiting, a placement later; the report
now reads what that read found. `place` fell from a mean of 2.7 to 1.4 and a p99 of 25.6 to 11.0,
and the one per cent low rose from 12.5 fps to 14.9. `.notes/bench.txt` holds the legs.

## The finding: the frame does not build chunks — it walks them

**The warming thread lands.** Split by thread id, `QuadTreeWorld::loadRenderingNode` is 0.16 ms a
frame on the frame and 1.75 on the thread; `ObjectPaging::createChunk` is 0.12 against 1.53. The
caches both go through are keyed on the chunk and not on the eye — `(centre, size, activeGrid)` for
the paging, `(centre, lod, lodFlags)` for the terrain — so what the thread builds a little ahead is
what the frame then finds already built.

**What the lead sweep measured was the wrong thing.** `sLeadSteps` at 1, 15, 30, 60 and 120 reads the
same at every value because the decomposition changes slowly: the route moves 200 units a frame
against a cell of 8192, so a lead of one and a lead of a hundred and twenty ask for nearly the same
squares. The sweep says the lead does not matter. It does not say the thread does not work.

**So what is left on the frame is the walk over the chunks, not their building.** `handOver` is
2.76 ms and `loadRenderingNode` is 0.16 of it; the other 2.6 is `takeChunk` handing each chunk to the
collector, which walks its geometry into the scene — a fold at 1.42 ms, a material resolve, an
instance compare, and the traversal itself. At a standing camera the same walk costs 1.77 ms with
nothing built, nothing folded and nothing new. **The frame's terrain cost is re-derivation, and it is
paid at every view rather than at a crossing alone.**

## Proposal 1 — a chunk the walk can prove unchanged is not walked — **landed**

**What it came to.** A third to two fifths of the walk at every view, and 11% of the crossing's
frame rate. `.notes/bench.txt` holds the legs. What follows is the shape it took.

**The shape.** `TerrainResidency` keeps, per chunk, the run of scene slots the last walk of it
produced, keyed on the `ChunkName` and the node. Where `takeChunk` is handed the same name and the
same `osg::Node*` as last frame, the residency replays the run — stamping the identities so the
sweep keeps them — instead of descending.

- **A paged chunk is immutable once built.** `ObjectPaging` builds it, caches it and hands the same
  node out until it expires; nothing runs a controller over it, no light hangs in it and no particle
  system sits under it. So the walk's answer for it cannot change while the node does not.
- **The node is the proof.** `loadRenderingNode` drops `entry.mRenderingNode` whenever the level of
  detail or its neighbours' change, and builds a new transform over freshly fetched chunks. A run is
  therefore valid exactly while the pointer is.
- **What it removes.** 2.6 ms a frame at the crossing and 0.7 at a standing exterior — the fold, the
  material resolve, the per-drawable map lookups and the traversal, for every chunk that did not
  change. It is the one item both a moving and a standing camera pay.
- **The run records what it saw.** A subtree holding anything but transforms, groups and plain
  drawables is never replayed, so the rule fails closed against content this does not know about.

**Risk.** A run that goes stale silently mirrors the wrong geometry. Three guards: the node compare,
a generation counter that voids every run whenever a sweep erases anything, and a debug-only pass
that walks the chunk anyway and asserts the run agreed.

**What it does not need.** No second thread, no published snapshot, no round of lag, and no settled
mode — the frame still asks for what it wants, when it wants it, and gets the same answer.

## Proposal 2 — the fold happens on the thread that built the chunk

**The shape.** `GeometryFold` moves behind a cache keyed on the drawable: the triangles a geometry
folds to, and its `FoldedShape`. The terrain worker fills it for every chunk it builds, in
`TerrainResidency::warm` and behind the same `mBuilding` lock. `MeshResolver` reads it and folds only
what it finds missing.

**What it removes.** 1.42 ms a frame at the crossing, which is 99% of the folding the frame does.
Nothing at a standing camera, where nothing is built — Proposal 1 is what covers that.

**Why this is not the thread's whole job.** Owning the quad tree outright — a worker that resolves,
builds and publishes a set the frame then reads — buys `loadRenderingNode`'s 0.16 ms on top of this,
and costs a round of lag, a snapshot, and a settled mode to keep the hashes repeatable. The thread
already builds the chunks; what it does not do is fold them, and that is the part worth moving.

**Risk.** Low. The cache is written by one thread while the frame is out of `loadRenderingNode`, and
a miss is the fold the frame does today. `GeometryFold` already documents itself as one instance a
thread.

## Proposal 3 — the walk reads what it wrote last frame

**What is left once Proposal 1 has taken the chunks.** The game's own graph: 0.5 ms of the
crossing's walk and about 1.1 of vivec's 1.77. The three `std::unordered_map` lookups every drawable
makes are 0.143 ms a frame at Seyda Neen, against 0.249 ms for the whole of `addDrawable`,
`MeshResolver::resolve` and `MaterialResolver::reuse` in self time. `Group.cpp:63`, the child loop,
is the hottest single line in the profile.

**The shape.** The walk visits the same nodes in the same order every frame — `descend` walks
children in order, a `Switch` in branch order, a residency in cell order — and a cell of 4916
placements has 138 deforming drawables in it. So the walk keeps a trace, one entry per drawable in
walk order, holding the path's identity fold, where the drawable stood, its material key and its
three slots. `addDrawable` takes a cursor: where the identity at the cursor matches the one just
folded, it has the slots without touching a map, and it compares the transform in the trace rather
than reaching into `PlacementTable`. A mismatch falls back to the maps and rebuilds the trace from
there.

**Proposal 1 is this rule applied to the one subtree it is easy on**, and this is the rest of the
graph, where a node may carry a light, a controller or a particle system and the proof has to be per
drawable rather than per subtree. So it comes after, and it is worth less: the chunks are the bulk.

**Expected.** About 0.16 ms of the trace and 0.40 ms of the replay at Seyda Neen.

**Risk.** The highest here, and the same failure as Proposal 1's — a trace that goes stale silently
mirrors the wrong geometry — over a graph the game writes rather than a chunk nothing touches.

**And it is not urgent.** It is 0.6 ms on a host with 1.3 to 2.9 ms of headroom at every view that
stands still. It is here because it is what is left, not because anything waits on it.

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

### Stage 1 — the terrain, which is most of the frame at every view

1. **The fold moves to the terrain worker (Proposal 2).** A cache keyed on the drawable, filled in
   `warm` behind `mBuilding` and read by `MeshResolver`.
   *Verified by*: a test that a geometry folded on the worker and folded on the frame give identical
   indices and an identical `FoldedShape`; the allocation guard; `repeatable.sh --pairs=10`.
   *Expected*: what is left of the crossing's 1.42 ms of folding now that the replay has taken the
   chunks that did not change — a first arrival still folds, and it would do so off the frame.

2. **Read what is then left of `place`.** Timed from inside the backend it is now the structures the
   arrivals create at 0.7 ms a frame and the textures they write at 0.45, and neither has been looked
   at. Propose against whichever is larger.

### Stage 2 — the game's own graph, if it is still worth it

3. **The trace (Proposal 3, first half).** Record it and use it for the three slot lookups.
   *Verified by*: a test that a second walk over an unchanged graph makes no map lookup at all,
   counted; the allocation guard, since the trace is scratch and never a per-frame allocation;
   `repeatable.sh`.

4. **The trace carries the transform**, and `PlacementTable` is reached only on a difference.

### Stage 3

5. Re-run every suite, re-take the profiles, and update this file.

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

**Read every run by thread id.** `perf script` prints `comm pid/tid` where the two differ and
`comm tid` where they do not, so the main thread is the record with one number. A symbol summed over
every thread is what put the terrain's building on the frame that does not do it. The off-CPU run is
worse still: its totals are dominated by driver workers parked for the length of the run.

```
perf script -i build-release/perf/blocked.data --no-inline > /tmp/blocked.txt
awk 'BEGIN{RS=""} /Engine::frame/ { ... }' /tmp/blocked.txt
```

**Its threshold is a millisecond and that is the floor.** `--off-cpu-thresh` counts milliseconds, so
a sleep shorter than one is invisible, and there is no way below it.

**And neither mode reaches inside the driver.** A third of the main thread is in `libnvidia-glcore`
at the crossing; frame pointers stop at its first frame, and `--dwarf` reaches `Engine::frame` on 4%
of samples where the frame-pointer walk reaches it on 24%. What names driver time is a
`steady_clock` around the call that enters it, added for the reading and taken out after — which is
how the compaction stall above was found.
