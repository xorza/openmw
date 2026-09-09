# The CPU side of the RTX frame

What the host spends a frame on, what limits it, and what is left to do. Taken at `8fb5cb803e` on
an RTX 4090 Laptop and an i9-13980HX, NVIDIA 610.57.04. The build is `apps/rtxtool/release.sh`,
which carries `-g1 -fno-omit-frame-pointer`.

**Finding 3 has since been named and fixed**, and the section that names it says what moved. Every
figure outside that section is from before the fix. The three places that stand still were measured
again after it and did not move; the island route did, and its row carries the newer reading.

Every bench ran `--validation=false --window=false --seconds=20`. That is 1200 measured frames after
180 warm-up frames at each place. The card was warm before the first suite, and no run waited for it
to cool. The profiles are `apps/rtxtool/profile.sh`, which records `task-clock` at 5999 Hz over the
measured frames only. `island-crossing` ran `--settled=false`, because a settled run puts the
composite queue's whole bake on the frame that queued it.

## How to read a row

`bench` reports the frame and the shares of it. `frame` is the whole loop, so the game's update is
in it. `wait` is the CPU standing still for the device inside `finishFrame`. `walk` is
`WorldMirror::mirror`. `place` is `WorldMirror::hand`.

**Four rows were added while this was written, and together they close the frame.** `finish` is the
whole of collecting the frame behind, of which `wait` is the largest share. `trace` is the record
and its submit. `present` is the picture reaching the surface with the interface over it. `update`
is the game's own loop, between one call into the renderer and the next. `frame` less all of them is
under 0.05 ms at every place that stands still — before them, 2.9 ms of the island route's frame was in no
row at all.

This document adds two columns. `host` is `frame - wait`, which is what the frame costs the CPU.
`rest` is `host - walk - place`, which the four rows above now split: the game's own update, the
trace record, the present, and the ring's work after the fence. `gpu` is the sum of the frame's
device zones.

**Read a profile by thread id and never by symbol.** The terrain warms on one thread, the composites
bake on another, and Recast builds the navigation mesh on a third. All three call functions the frame
also calls. Each figure below says which thread it is on. A percentage becomes milliseconds a frame
through the recording's own event count over 1200 frames.

## Where it stands

Medians in milliseconds, over 18 places.

| view | frame | p99 | wait | **host** | walk | place | rest | gpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| vivec | 6.86 | 9.98 | 3.40 | **3.46** | 1.26 | 0.81 | 1.39 | 5.86 |
| balmora-storm-night | 5.65 | 8.63 | 3.26 | **2.39** | 0.93 | 0.43 | 1.03 | 5.01 |
| ald-ruhn | 5.44 | 8.43 | 3.05 | **2.39** | 0.79 | 0.45 | 1.15 | 4.80 |
| balmora-fog-night | 5.41 | 8.44 | 3.05 | **2.36** | 0.80 | 0.48 | 1.08 | 4.74 |
| sadrith-mora | 5.28 | 8.22 | 3.12 | **2.16** | 0.67 | 0.42 | 1.07 | 4.67 |
| seyda-neen-ship | 5.76 | 8.69 | 3.61 | **2.15** | 0.71 | 0.44 | 1.00 | 5.16 |
| balmora | 5.44 | 8.43 | 3.29 | **2.15** | 0.79 | 0.38 | 0.98 | 4.86 |
| seyda-neen-ship-overcast | 5.86 | 8.91 | 3.74 | **2.12** | 0.70 | 0.44 | 0.98 | 5.27 |
| seyda-neen-ship-dawn | 6.19 | 9.22 | 4.08 | **2.11** | 0.70 | 0.42 | 0.99 | 5.62 |
| seyda-neen-shore | 5.60 | 8.71 | 3.95 | **1.65** | 0.67 | 0.17 | 0.81 | 5.28 |
| dagon-fel | 5.25 | 8.18 | 4.08 | **1.17** | 0.34 | 0.14 | 0.69 | 4.94 |
| addamasartus | 5.14 | 8.37 | 4.00 | **1.14** | 0.23 | 0.18 | 0.73 | 4.80 |
| balmora-mages-guild | 5.10 | 8.16 | 3.97 | **1.13** | 0.28 | 0.20 | 0.65 | 4.75 |
| andrano-tomb | 4.42 | 7.51 | 3.36 | **1.06** | 0.32 | 0.04 | 0.70 | 4.08 |
| seyda-neen-customs | 4.62 | 7.82 | 3.61 | **1.01** | 0.27 | 0.13 | 0.61 | 4.31 |
| vivec-canalworks | 4.51 | 7.51 | 3.55 | **0.96** | 0.25 | 0.12 | 0.59 | 4.22 |
| arkngthand | 4.13 | 7.14 | 3.23 | **0.90** | 0.23 | 0.11 | 0.56 | 3.82 |
| island-crossing | 5.71 | 63.33 | 3.06 | **2.65** | 1.10 | 0.21 | 1.34 | 5.77 |

`seyda-neen-ship` and `balmora-mages-guild` each stand in two suites. The table holds the reading
from the `default` suite. The two readings of each agree to within 0.10 ms of frame time.

**No place that stands still is CPU-bound.** Every `host` is below its own `gpu`. The smallest
margin is 2.38 ms, at `balmora-fog-night`, and the largest is 3.77 ms at `dagon-fel`.

**One place moves, and it is the whole problem.** `island-crossing` flies the Bitter Coast to the
Ashlands at 12,000 units a second. Its median frame is 5.71 ms and its p99 is 63.33 ms. That is
15.8 fps at the one per cent low against 175 at the median. The rest of this document is mostly
about that row. Its figures here are from after Finding 3's fix, which is why they differ from the
profile tables below.

## The two instruments agree

Four views were profiled. `total` is every thread of the process. `main` is the thread that runs the
frame, on a core. `host` and `walk` are the same run's own `bench` means.

| view | total | main | host | mirror | walk |
|---|---:|---:|---:|---:|---:|
| balmora-mages-guild | 1.56 | 1.29 | 1.31 | 0.34 | 0.35 |
| seyda-neen-ship | 3.02 | 2.35 | 2.23 | 0.80 | 0.75 |
| vivec | 6.57 | 3.59 | 3.66 | 1.32 | 1.34 |
| island-crossing | 17.36 | 6.10 | 8.99 | 2.74 | 2.70 |

`WorldMirror::mirror` from the profile and `walk` from `bench` agree to within 0.06 ms at every view.
At the three places that stand still, the main thread's on-CPU time and `frame - wait` agree to
within 0.12 ms. **So a standing frame's host cost is work and not a hidden block.**

The crossing is the exception, and Finding 3 is about the 2.89 ms between its two columns.

## Where the main thread's frame goes

Milliseconds a frame, from the profile, main thread only. Indentation is containment, and a child is
inside its parent.

| | guild | ship | vivec | crossing |
|---|---:|---:|---:|---:|
| **`OMW::Engine::frame`** | **0.840** | **1.819** | **2.617** | **4.184** |
| — `RenderingManager::renderFrame` | 0.467 | 1.131 | 1.710 | 3.307 |
| — — `WorldMirror::mirror` | 0.344 | 0.797 | 1.324 | 2.741 |
| — — — `TerrainResidency::collect` | 0.000 | 0.186 | 0.173 | 2.245 |
| — — — — `QuadTreeWorld::handOver` | 0.000 | 0.176 | 0.164 | 2.220 |
| — — — — — `MirrorTraversal::takeChunk` | 0.000 | 0.164 | 0.149 | 2.163 |
| — — — — — `QuadTreeWorld::loadRenderingNode` | 0.000 | 0.000 | 0.000 | 0.153 |
| — — — `SceneExtractor::replayChunk` | 0.000 | 0.077 | 0.105 | 0.089 |
| — — — `MirrorTraversal::apply(osg::Drawable&)` | 0.115 | 0.238 | 0.305 | 1.908 |
| — — — — `SceneExtractor::mirrorDrawable` | 0.101 | 0.210 | 0.282 | 1.871 |
| — — — — — `MeshResolver::resolve` | 0.041 | 0.073 | 0.088 | 1.682 |
| — — — — — — `GeometryFold::read` | 0.000 | 0.000 | 0.000 | 1.424 |
| — — — — — — — `ShapeFold::fold` | 0.000 | 0.000 | 0.000 | 1.373 |
| — — — — — `MaterialResolver::animate` | 0.036 | 0.071 | 0.153 | 0.234 |
| — — `RtxRenderer::traceWorld` | 0.051 | 0.251 | 0.282 | 0.333 |
| — — `WorldMirror::hand` | 0.034 | 0.229 | 0.191 | 0.312 |
| — — — `VulkanRenderer::placeScene` | 0.027 | 0.204 | 0.164 | 0.075 |
| — — `MyGUIRtx::RenderManager::collectDrawCalls` | 0.060 | 0.071 | 0.093 | 0.097 |
| — `RtxRenderer::updateTraversal` | 0.106 | 0.173 | 0.257 | 0.156 |
| — `World::updateFocusObject` | 0.099 | 0.158 | 0.230 | 0.196 |
| — `MechanicsManager::update` | 0.078 | 0.153 | 0.143 | 0.101 |
| — `World::updatePhysics` | 0.013 | 0.066 | 0.055 | 0.064 |
| — `MWWorld::World::update` | 0.016 | 0.023 | 0.079 | 0.248 |
| — `ScriptManager::run` | 0.013 | 0.040 | 0.061 | 0.026 |
| *the driver, beside the rows above and not inside them* | 0.310 | 0.374 | 0.775 | 1.696 |

**The chunk replay landed and it holds.** `TerrainResidency::collect` costs 0.19 ms at Seyda Neen
and 0.17 ms at Vivec, and `SceneExtractor::replayChunk` is 0.08 ms and 0.11 ms of that. The walk
around it fell by a third to two fifths at every view when the replay landed, and `.notes/bench.txt`
holds those legs.

**A standing frame folds nothing.** `GeometryFold::read` is zero at all three standing views,
because nothing arrives. It is 1.42 ms at the crossing, and that is Finding 2.

**A third of the interior's frame is the driver.** The guild's main thread spends 0.31 ms a frame
inside `libnvidia-glcore`, `libnvidia-rtcore`, `libcuda` and NGX, against a whole host frame of
1.31 ms. Frame pointers stop at the driver's first frame, so the caller is an address. What names
this time is a `steady_clock` around the call that enters it, and no profiler here can do it.

## What the other threads do

Milliseconds a frame, per thread, from the same recordings.

| thread | guild | ship | vivec | crossing |
|---|---:|---:|---:|---:|
| the frame | 1.29 | 2.35 | 3.59 | 6.10 |
| `CompositeQueue::bake` | — | — | — | 4.59 |
| `AsyncNavMeshUpdater::process` | — | — | 2.59 | 4.16 |
| `SceneUtil::WorkThread::run` | — | — | — | 1.53 |
| `PhysicsTaskScheduler::doSimulation` | 0.15 | 0.37 | 0.17 | — |
| `MWLua::Worker::run` | 0.06 | 0.23 | 0.15 | — |
| `TerrainResidency::warm` | — | — | — | 0.41 |

This box has 32 hardware threads and the whole process asks for 17.36 ms of CPU a frame at its
worst, which is 1.61 cores. **No worker takes anything from the frame.** The navigation mesh is the
largest of them and it is an initial build, not a per-frame cost.

## Finding 1 — the crossing's tail is the paged statics, and they are two thirds of it

`--distant-statics=false` stands the same distant ground with nothing on it. Two legs of each,
interleaved, `--settled=false`.

| leg | median | mean | p95 | p99 | worst | 1% low | instances |
|---|---:|---:|---:|---:|---:|---:|---:|
| as measured | 5.80 | 10.69 | 30.43 | 65.99 | 135.17 | 15.2 | 2568 |
| as measured | 5.97 | 10.71 | 30.17 | 65.61 | 136.91 | 15.2 | 2568 |
| no distant statics | 6.04 | 7.33 | 13.89 | 23.12 | 44.32 | 43.3 | 1800 |
| no distant statics | 6.13 | 7.41 | 14.31 | 23.79 | 45.06 | 42.0 | 1800 |

The walk moves with it: a mean of 2.71 ms becomes 1.42, and a worst frame of 84.45 ms becomes 22.12.
The whole crossing report falls from 1.0 s of reading to 0.3 s.

**Thirty per cent of the instances carry two thirds of the tail.** The paged buildings, trees and
rocks are one merged geometry per chunk and per kind, so a ring of them arrives as a few very large
drawables rather than as many small ones. The frame that first meets one folds it, resolves it and
walks it.

**The median does not improve, and the card is why.** Every leg without the statics ran at a lower
core clock (1740–1995 MHz against 1755–2250), because the shorter run gives the card less to boost
for. The median difference is 0.2 ms and it is inside that spread.

## Finding 2 — the fold is still on the frame, and it is the largest single item on the crossing

`GeometryFold::read` costs 1.424 ms a frame of the main thread at the crossing, out of a
`MeshResolver::resolve` of 1.682 and a walk of 2.741. `ShapeFold::fold` is 1.373 of it, and
`ShapeFold::closes` is 0.62 in self time. The hottest line in the whole crossing profile is
`shapefold.cpp:35`, the position hash, at 0.274 ms a frame.

The fold matches every triangle against its reversed twin and asks whether the shape is closed.
Morrowind doubles every leaf, fern and grass card, so the work is proportional to the triangles that
arrive. A paged chunk merges every static of one kind into one geometry, which is why an arrival
frame folds tens of thousands of triangles at once.

**The frame does not build chunks. It folds and walks them.** `ObjectPaging::createChunk` costs
1.564 ms a frame across the process and 0.116 of that is on the frame. `QuadTreeWorld::preload` on
the work thread builds what the frame then finds already built. So 93 per cent of chunk building is
already off the frame, and the fold is not.

**One attempt to move it was measured and withdrawn.** `.notes/bench.txt` records it: the fold ran on
`TerrainResidency::warm`, and at the shipped lead of thirty steps the cache hit rate was 1.6 per
cent. That thread aims ahead of the view point, so it builds a decomposition the frame does not ask
for, and `ObjectPaging` keys a chunk's merged geometry on the decomposition.

**What the numbers point at now is a different thread.** The game's own preloader already builds
93 per cent of the chunks the frame uses, on `SceneUtil::WorkThread`. A fold beside
`ObjectPaging::createChunk`, keyed on the geometry the paging caches, would be read by the frame
that later asks for that same chunk. The hit rate is the measurement to take first, and it is the
one the withdrawn attempt failed on. Take it before writing the fold cache.

## Finding 3 — the crossing's block was a drain the frame did not need — **fixed**

**What it was.** 2.89 ms a frame of the island route was a main thread asleep in a place no counter
named and no profiler could reach. It is now named, and it was one call.

`RtxRenderer::fitToWindow` hands the window's extent to the backend on every settled frame. It does
that on purpose: a swapchain can go stale without the window moving, and only the backend can say
so. `VulkanRenderer::resize` then drained the interface's staged batches — `GuiTextures::finish`, a
submit and a wait — **before** asking whether anything had changed. The drain guards a command pool
reset that a swapchain rebuild causes, and a rebuild almost never happens.

`Presenter::wantsResize` is now the question, and the drain waits for a yes.

**What it cost.** Measured on the island route by bracketing the call: a median of 0.00 ms and a
mean of 2.70, with a p95 of 12.14 and a worst frame of 27.57. The shape is why nothing had found
it — an ordinary frame has nothing staged, so the drain returns at once, and the cost lands only on
the frames a ring arrives on.

**What it came to.** Two legs of each, interleaved, `--settled=false`:

| leg | median | mean | p95 | p99 | worst | fps | 1% low | `wait` mean |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| before | 7.25 | 11.29 | 31.13 | 66.00 | 134.27 | 137.9 | 15.2 | 1.83 |
| after | 5.75 | 9.90 | 26.59 | 65.96 | 125.50 | 173.8 | 15.2 | 3.41 |
| before | 6.05 | 11.02 | 30.77 | 67.14 | 144.32 | 165.2 | 14.9 | 1.68 |
| after | 5.71 | 9.93 | 27.18 | 63.33 | 126.56 | 175.3 | 15.8 | 3.38 |

**1.1 ms of the mean and 0.5 of the median, and the wait doubles.** What the host stops spending on
its own drain it spends standing still for the device instead, which is the right place for it. The
one per cent low does not move: the tail is the frames a ring arrives on, and those are the walk and
the placement.

**A place that stands still is unchanged**, because nothing is staged there for the drain to find.
That is why the cost had never appeared outside a route.

**How it was found, and what to do with the next one like it.** The block was bisected with
`steady_clock`, one stretch at a time, until every part of the loop had a row. Four of those rows
earned their place and stayed. The profiler could not have found this: the whole cost is a thread
asleep inside the driver, and every stack the off-CPU profile collected for the main thread ends at
the driver's first frame with no caller — 1822 sleeps a run, 5.13 ms a frame, and not one of them
names a function of ours.

## What the new rows say now the block is gone

Milliseconds a frame at the island route, after the fix, `--settled=false`. Means, because a route's
median describes the frames between the arrivals rather than the run.

| row | mean | p99 | worst |
|---|---:|---:|---:|
| `frame` | 9.93 | 63.33 | 126.56 |
| — `finish` | 3.92 | 17.48 | 32.79 |
| — — `wait` | 3.38 | 15.72 | 24.53 |
| — `walk` | 2.81 | 32.25 | 86.59 |
| — `place` | 1.38 | 10.72 | 35.83 |
| — `update` | 1.25 | 23.13 | 48.73 |
| — `present` | 0.20 | 0.57 | 1.78 |
| — `trace` | 0.18 | 0.35 | 0.50 |

Two things follow that were not visible before.

**The ring's after-fence work is 0.55 ms a frame at the crossing and 0.01 at a standing place.**
`finish` less `wait` is reading the device's counters and its timestamps and destroying what that
frame was the last to read. It costs nothing where nothing is destroyed, which is why only a route
shows it.

**The game's own loop is 1.25 ms a frame with a p99 of 23.** `update` is upstream's — the cells
arriving and the preloader waited on — and this fork cannot change it. It is the second largest
share of an arrival frame after the walk.

## Finding 4 — the identity maps are a fifth of a standing frame's walk

The `std::unordered_map` lines together — `hashtable_policy.h:1464`, `hashtable.h:2263`, `:2266`,
`:2269` and `hashtable_policy.h:585` — cost 0.159 ms a frame at Seyda Neen and 0.232 at Vivec. Every
drawable the walk meets does three lookups: the mesh, the material and the placement. Each is a node
allocation away from the last, so each is a cache miss.

`Group.cpp:63`, the child loop of `osg::Group::traverse`, is the hottest single line at every
standing view: 0.122 ms at Seyda Neen, 0.175 at Vivec, 0.071 at the guild.

Both belong to the same proposal: the walk visits the same nodes in the same order every frame, and
it could read what it wrote last frame instead of asking a map. The chunk replay is that rule applied
to the one subtree where the proof is easy. The rest of the graph needs a per-drawable proof, because
a node there may carry a light, a controller or a particle system.

**And it is not urgent.** It is about 0.2 ms on a host with 2.4 ms of headroom at every view that
stands still.

## Finding 5 — the fold is three quarters of the walk at the tail

The mean said the fold was 1.42 ms a frame. That is not the number the one per cent low is made of,
so the walk now reports a `fold` row of its own. Milliseconds, `island-crossing`,
`--settled=false`.

| row | median | mean | p95 | p99 | worst |
|---|---:|---:|---:|---:|---:|
| `frame` | 5.23 | 9.50 | 24.63 | 60.02 | 122.56 |
| `walk` | 0.80 | 2.52 | 12.17 | 31.07 | 84.65 |
| — `fold` | 0.00 | 1.36 | 9.55 | 23.14 | 69.09 |
| `place` | 0.24 | 1.32 | 4.36 | 10.64 | 33.72 |
| `update` | 0.59 | 1.17 | 1.26 | 22.16 | 46.20 |

**The fold is 74% of the walk at the p99 and 82% of it at the worst frame**, against 54% of its
mean. Read against the whole frame it is 39% at the p99 and 56% at the worst. A place that stands
still folds nothing, so the row is nought everywhere else. The mean understated it because the fold
is all arrival and the mean divides by every frame.

**And 92% of it could have been done before the frame asked.** By thread, from the same run's
profile: `ObjectPaging::createChunk` is 1.528 ms a frame across the process and 0.119 of that on the
frame. `GeometryFold::read` is 1.352 ms a frame, every sample of it on the frame. So the geometry
the frame folds was, nearly always, built by the game's own preloader some frames earlier and left
sitting unfolded.

**What stands between the two numbers is which thread.** No thread this fork owns builds those
chunks. `TerrainResidency::warm` resolves a decomposition of its own, which is why folding there
reached 1.6% at the shipped lead and 31% at a lead of one. `.notes/bench.txt` records that attempt
and names its cap: the warming pass was cut short by the frame, rather than aimed at the wrong
squares.

**So the work to do is a warming pass that folds what the preloader has already built**, and the
thing to fix inside it is why the pass does not finish. The lag is not the problem — at 12,000 units
a second a frame moves the view point about 120 units against a cell of 8192, so a pass one frame
behind asks for nearly the same squares.

**The risk is the one every cache here carries**: a fold that goes stale mirrors the wrong geometry.
`ShapeFold` already documents itself as one instance a thread, and the fold is keyed on the
drawable, so the guard is the same one `ChunkRuns` uses — the pointer is the proof, and a debug-only
pass that folds anyway and compares is what says so.

## What is not a problem

**The workers.** Recast, Bullet and Lua all run off the frame, and the frame pays only for scheduling
them. `World::updatePhysics` is 0.066 ms at Seyda Neen.

**Chunk building.** 93 per cent of it is on the preloader. The frame's own share is 0.153 ms at the
crossing and nothing anywhere else.

**The sprites.** The bin and the shading both moved to the device. `EmitterResolver::placeSprites` is
0.065 ms a frame at Vivec, the largest of the four views profiled, and the device's `sprites`
zone is 0.13 ms there.

**The interface.** `MyGUIRtx::RenderManager::collectDrawCalls` is 0.060 ms a frame at the guild,
and `MyGUI::RenderItem::renderToTarget` is 0.048 of it. That is the view with the most interface in
it.

**The composite queue.** With `--settled=false` the bake never touches the frame. It costs 4.59 ms a
frame of one worker thread over the crossing, and `CompositeQueue::advance` on the frame is 0.003 ms.

## What to do next, in order

1. ~~Name the crossing's 2.89 ms of block.~~ **Done.** Finding 3 says what it was and what it came
   to. The rows that found it are now part of every report.
2. ~~Measure the fold cache hit rate.~~ **Done, and both halves say build it.** The `fold` row
   below says what it costs at the tail, and the thread split says 92% of it could have been done
   before the frame asked.
3. **Fold what the preloader has already built, on a thread of this fork's own.** The design and
   the risk are below.
4. **Then read what is left of the walk at the crossing.** `takeChunk` is 2.16 ms and the fold is
   1.42 of it. The other 0.74 is the walk of chunks whose runs did not replay.
5. **The trace over the game's own graph is last.** It is 0.2 ms at a standing view and the host has
   ten times that in headroom.

**No frame time here is a budget failure.** The target is 60 fps at 1920×1080 internal. Every place
in the corpus runs at 145 fps or better at the median, and the one per cent low is above 100 fps
everywhere except the crossing.

## How to repeat these measurements

```
cd build-release
for suite in default exteriors skies interiors; do
    ./openmw-rtxtool bench --validation=false --window=false --suite=$suite --seconds=20 --json=$suite.json
done
./openmw-rtxtool bench --validation=false --window=false --suite=streaming --seconds=20 --settled=false

apps/rtxtool/profile.sh --view=balmora-mages-guild
apps/rtxtool/profile.sh --view=seyda-neen-ship
apps/rtxtool/profile.sh --view=vivec
apps/rtxtool/profile.sh --view=island-crossing --settled=false
```

Warm the card with one thrown-away `bench` first. The clock and the temperature are printed beside
every result, and a leg whose clock differs from its neighbour's is the leg to repeat.

**Read every recording by thread id.** `perf report -i <data> --sort pid` lists the threads by share,
and the main thread is the one whose tid equals the pid. Then filter:

```
perf report -i build-release/perf/cpu.data --stdio --no-inline --tid=<main> --children --sort symbol -g none
```

Percentages stay relative to the whole recording under a filter, so a figure times the recording's
event count over 1200 frames is milliseconds a frame on that thread. The same report splits one
symbol into two rows when a stack has two roots. Sum the rows that carry one name.
