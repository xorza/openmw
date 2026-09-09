# The CPU side of the RTX frame

2026-09-09, at `0024c46a07` with no working-tree changes. Release build from
`apps/rtxtool/release.sh`, which carries `-g1 -fno-omit-frame-pointer`. RTX 4090 Laptop,
i9-13980HX, NVIDIA 610.57.04.

Every bench ran `--validation=false --window=false --seconds=20`, which is 1200 measured frames
after 180 warm-up frames at each place. The profiles are `apps/rtxtool/profile.sh`, which records
`task-clock` at 5999 Hz over the measured frames only.

## What a row means

`bench` reports four times a frame. `frame` is the whole loop, one trace to the next, so the
game's update is in it. `wait` is the CPU standing still for the device inside `finishFrame`.
`walk` is `WorldMirror::mirror`. `place` is `WorldMirror::hand`.

This document adds two derived columns. `cpu` is `frame - wait`, which is what the frame costs
the host. `rest` is `frame - wait - walk - place`, which is everything the two named rows do not
cover: the game's own update, the sprite shading, the interface, and the present.

The percentages from `perf` are converted to milliseconds a frame with the recording's own event
count over 1200 frames. A profile averages over every frame, so it is read against a `bench`
mean and not against a median.

**The two instruments agree on the walk.** `WorldMirror::mirror` from the profile against the
`walk` mean from `bench`, in milliseconds: 1.17 against 1.37 at Seyda Neen, 2.12 against 2.26 at
Vivec, 3.10 against 3.16 on the crossing. The mages guild reads 0.48 against 0.31, which is a
14,000-sample recording of a 0.3 ms row.

**Where they disagree, the difference is the main thread blocked.** `perf` counts a thread on a
core and `bench` counts a wall clock, so `OMW::Engine::frame` from the profile against
`frame - wait` from `bench` is what the main thread spends waiting outside `finishFrame`:

| view | on-CPU | `frame - wait` | blocked |
|---|---:|---:|---:|
| balmora-mages-guild | 1.15 | 1.13 | -0.02 |
| seyda-neen-ship | 2.27 | 2.87 | 0.60 |
| vivec | 6.12 | 7.03 | 0.91 |
| island-crossing, median | 4.72 | 4.52 | -0.20 |
| island-crossing, mean | 4.72 | 14.10 | 9.38 |

A small negative is the sampling error of the profile against the wall clock. The last row is
not: it is 9.4 ms a frame of a main thread that is asleep, and it is Finding 1.

## Every place, by host cost

Median milliseconds. `gpu` is the sum of the frame's device zones.

| view | inst | frame | p99 | wait | walk | place | rest | **cpu** | gpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| vivec | 4877 | 8.58 | 11.51 | 1.54 | 2.14 | 0.99 | 3.90 | **7.03** | 5.34 |
| island-crossing | 2568 | 5.95 | 169.32 | 1.43 | 1.38 | 0.25 | 2.89 | **4.52** | 5.55 |
| balmora-fog-night | 5600 | 5.45 | 8.43 | 2.37 | 1.47 | 0.50 | 1.12 | **3.08** | 4.85 |
| balmora-storm-night | 5502 | 5.66 | 8.71 | 2.66 | 1.47 | 0.51 | 1.03 | **3.00** | 5.01 |
| ald-ruhn | 3840 | 5.40 | 8.14 | 2.52 | 1.17 | 0.49 | 1.22 | **2.88** | 4.74 |
| seyda-neen-ship | 4880 | 5.89 | 8.68 | 3.02 | 1.27 | 0.45 | 1.15 | **2.87** | 5.29 |
| sadrith-mora | 3413 | 5.29 | 8.18 | 2.48 | 1.10 | 0.56 | 1.15 | **2.81** | 4.56 |
| balmora | 5538 | 5.46 | 8.22 | 2.80 | 1.20 | 0.47 | 0.98 | **2.66** | 4.80 |
| seyda-neen-ship-dawn | 4851 | 6.20 | 8.94 | 3.67 | 1.12 | 0.41 | 1.00 | **2.53** | 5.70 |
| seyda-neen-ship-overcast | 4880 | 5.79 | 8.62 | 3.29 | 1.02 | 0.44 | 1.04 | **2.50** | 5.15 |
| seyda-neen-shore | 5774 | 5.64 | 8.57 | 3.28 | 1.20 | 0.29 | 0.87 | **2.36** | 5.18 |
| vivec-canalworks | 543 | 5.16 | 8.41 | 3.19 | 0.31 | 0.14 | 1.52 | **1.97** | 4.31 |
| dagon-fel | 1881 | 5.25 | 8.15 | 3.74 | 0.56 | 0.22 | 0.74 | **1.51** | 4.89 |
| seyda-neen-customs | 850 | 5.11 | 8.72 | 3.65 | 0.32 | 0.15 | 1.00 | **1.46** | 4.86 |
| addamasartus | 635 | 5.19 | 7.62 | 3.89 | 0.27 | 0.19 | 0.84 | **1.30** | 4.88 |
| andrano-tomb | 1000 | 4.48 | 7.00 | 3.26 | 0.32 | 0.05 | 0.86 | **1.22** | 3.96 |
| balmora-mages-guild | 1221 | 5.11 | 8.01 | 3.97 | 0.29 | 0.20 | 0.64 | **1.13** | 4.81 |
| arkngthand | 990 | 4.09 | 6.38 | 3.12 | 0.23 | 0.11 | 0.63 | **0.97** | 3.77 |

`seyda-neen-ship` and `balmora-mages-guild` each stand in two suites. The table holds one row of
each, and the two readings of each agree to within 0.15 ms of frame time.

**One place of eighteen is CPU-bound today.** Vivec asks 7.03 ms of the host against 5.34 ms of
the device, and it is the only row where `cpu` passes `gpu`. Every other place waits on the
device. Leaving the two rows above out, the host has between 1.8 and 3.7 ms of headroom in the
frame.

**The streaming row is a different question.** `island-crossing` has a median of 5.95 ms and a p99
of 169.32 ms. Its one per cent low is 5.9 fps.

## Where the main thread's frame goes

Milliseconds a frame, from the profile. Indentation is containment, and a child is included in
its parent.

| | seyda-neen-ship | vivec |
|---|---:|---:|
| `OMW::Engine::frame` | **2.27** | **6.12** |
| — `RenderingManager::renderFrame` | 1.58 | 5.08 |
| — — `WorldMirror::mirror` | 1.17 | 2.12 |
| — — — `TerrainResidency::collect` | 0.57 | 0.87 |
| — — `RtxRenderer::traceWorld` | 0.31 | 2.84 |
| — — — `VulkanRenderer::renderFrame` | 0.07 | 2.47 |
| — — — — `SceneBuffers::binSprites` | 0.06 | 2.40 |
| — — — `WorldMirror::hand` | 0.23 | 0.36 |
| — — `MyGUIRtx::RenderManager::collectDrawCalls` | 0.08 | 0.11 |
| — `RtxRenderer::updateTraversal` | 0.18 | 0.30 |
| — `MechanicsManager::update` | 0.16 | 0.17 |
| — `World::updateFocusObject` | 0.16 | 0.25 |
| — `World::updatePhysics` | 0.07 | 0.08 |
| — `ScriptManager::run` | 0.04 | 0.07 |

The two columns hold the whole argument. Seyda Neen's frame is the walk; Vivec's frame is the
walk and the sprite shading, and the second is larger.

## Finding 1 — the crossing spike is a wait, and the harness is what makes it one

`island-crossing` reports `place` at a median of 0.25 ms, a mean of 7.21 ms, a p99 of 130.93 ms
and a worst of 391.22 ms. The profile says `WorldMirror::hand` costs 0.367 ms a frame of CPU. So
about 95 per cent of `place` is a thread that is blocked and not a thread that is working.

`CompositeQueue::advance` is where it blocks:

```
gather(scene.getTables(), images);
if (mSettled)
    finish();                       // waits until every queued bake is done
return collect(scene, sCompositesPerFrame);
```

`RtxRenderer` settles the queue whenever the frame clock states a step, and
`RtxTool::runHosted` states one for every verb the harness has. So **every number this harness
reports carries the composite wait**, and a played game does not.

Three A/B runs over the same route say the wait tracks the distant ground and not the objects on
it:

| leg | inst | frame med | frame mean | frame p99 | place mean | place worst |
|---|---:|---:|---:|---:|---:|---:|
| as measured | 2568 | 5.95 | 15.66 | 169.32 | 7.21 | 391.22 |
| `--distant-statics=false` | 1800 | 5.16 | 11.42 | 121.50 | 5.40 | 309.78 |
| `--distant-cells=2` | 1144 | 4.73 | 11.03 | 85.45 | 4.35 | 193.23 |
| `--distant-cells=0` | 930 | 4.69 | 8.61 | 68.62 | 2.64 | 98.39 |

Removing the paged buildings, trees and rocks takes 30 per cent off the instances and leaves the
worst frame at 310 ms. Halving the ground's reach halves the spike. Handing the reach back to
`viewing distance`, which barely leaves the active grid and so hardly reaches the flatten path,
cuts the spike by four.

The baker itself agrees. `CompositeQueue::bake` is 22.28 per cent of the recording, which is
4.26 ms a frame of a worker thread, or 5.1 s over an 18.6 s run.

**What this costs the work.** The gate cannot report the streaming path's real frame time while
it settles, and the settling is what makes the run repeatable. A run that measures streaming and
a run that compares pictures are two runs, and the tree has one.

**What to do.** Give `bench` a switch that leaves the queue unsettled, and take the streaming
number under it. Keep the settled run for `verify`, for `shot` and for the hashes. State in
`benches.cfg` that the `streaming` suite is measured unsettled.

## Finding 2 — sprite shading on the host is the whole of Vivec's excess

`SpriteShade::layDown` is 18.22 per cent of the Vivec recording, self time. Its whole chain —
`SceneBuffers::binSprites` into `SpriteShade::shade` — is 22.20 per cent, which is 2.396 ms a
frame on the main thread. Vivec's `rest` column is 3.90 ms. The sprite shading is about
two thirds of it, and `binSprites` is 39 per cent of the main thread's on-CPU frame.

The cost tracks the sprite count of the emitters `shade` does not skip:

| view | emitters | sprites | rest ms | layDown ms/frame |
|---|---:|---:|---:|---:|
| vivec | 62 | 6076 | 3.90 | 1.966 |
| vivec-canalworks | 40 | 1957 | 1.52 | — |
| seyda-neen-customs | 55 | 1020 | 1.00 | — |
| balmora-mages-guild | 19 | 399 | 0.64 | — |
| seyda-neen-ship | 21 | 357 | 1.15 | 0.045 |

`balmora-storm-night` holds 2624 sprites and pays no sprite shading at all, because rain is a
streak and `shade` refuses an emitter with a width. That is what says the count that matters is
the round, non-additive one.

The work is a grid rasterisation, once per emitter per light — toward the sun and toward the sky.
It is `O(sprites x disc area)`, it runs on the frame path, and it runs on the host. Over both
lights `layDown` costs 0.32 microseconds a sprite at Vivec against 0.13 at Seyda Neen, because a
canton's plumes are wider in cells.

**What to do.** Move it to the device, the way the sprite bin already moved. The input is the
sprite run and two directions. The output is two floats a sprite. `mGrid` is 32 by 32 floats,
which is 4 KiB and fits in shared memory, and the sort is over a few hundred keys per emitter.
`.notes/bench.txt` records what the bin's move was worth: 12.6 per cent of the harness's CPU
became a 0.05 ms device zone.

## Finding 3 — the walk is the largest steady cost, and half of it is the paged terrain

`WorldMirror::mirror` costs 1.17 ms a frame at Seyda Neen and 2.12 ms at Vivec. Inside it,
`TerrainResidency::collect` costs 0.574 ms and 0.870 ms — 49 per cent and 41 per cent of the
walk. At `island-crossing` it is 2.62 ms of a 4.72 ms main-thread frame.

`collect` hands each chunk to `MirrorTraversal`, which then walks the chunk's whole subtree. The
subtree is one merged geometry per paged static kind, so the walk is shallow and wide, and
`osg::Group::traverse` is 8.24 per cent of the Seyda Neen recording, self time. `Group.cpp:63` —
the child loop — is the hottest single line in the whole profile at 7.83 per cent.

What the rest of the walk spends. Milliseconds a frame, and children unless the row says
self:

| symbol | seyda-neen-ship | vivec | island-crossing |
|---|---:|---:|---:|
| `osg::Group::traverse` (self) | 0.288 | 0.472 | 0.270 |
| `SceneExtractor::addDrawable` | 0.352 | 0.557 | 2.019 |
| `MeshResolver::resolve` | 0.121 | 0.179 | 1.700 |
| `MaterialResolver::animate` | 0.084 | 0.197 | 0.266 |
| `MaterialResolver::reuse` (self) | 0.071 | 0.108 | — |
| `fadeThrough` | 0.113 | 0.175 | 0.142 |
| `LightGrid::rebuild` (self) | 0.081 | 0.113 | — |
| `__dynamic_cast` | 0.070 | 0.104 | — |

Three observations follow.

**The identity maps are a measurable share.** The `std::unordered_map` lines together —
`hashtable_policy.h:1464`, `hashtable.h:2263`, `:2266` and `:2269` — are 4.36 per cent of the
Seyda Neen recording, which is 0.152 ms a frame. Every drawable does three lookups a frame:
`mMeshes.find`, `mMaterials.find` through `reuse`, and `mPlacements.find`. Each is a node
allocation away from the last, so each is a cache miss. A flat open-addressed table with the same
`ByAddress` key would keep the semantics and remove the chase.

**A world that stands still is walked whole every frame.** The static fast path in
`MeshResolver::resolve` is good — a crate met again costs a lookup and a stamp — but the walk
still descends every node, composes every matrix, and re-resolves every drawable. There is no
gate that says "this subtree did not move". Nothing here is wrong, and it is where the remaining
millisecond of an exterior lives.

**`fadeThrough` asks a question of the state set that the state set already answered.**
`MirrorTraversal::pushShading` calls it at every node and every drawable the walk enters, and it
costs 0.113 ms a frame at Seyda Neen, which is a tenth of the walk. What it does is two
`osg::StateSet::getUniform` calls, and `getUniform` is a `std::map<std::string, ...>` lookup:
`stl_tree.h:2644` is 0.93 per cent of the recording and `memcmp-avx2-movbe.S:416` is 0.79 per
cent. Nearly no state set in the world carries `actorFade`, and whether one does is a fact about
the state set rather than about the frame. The answer belongs beside the material the state set
already keys, asked once when the walk first meets it.

## Finding 4 — `ShapeFold` is 28 per cent of the streaming frame

`ShapeFold::fold` and what it calls cost 1.319 ms a frame at `island-crossing`, against a
main-thread frame of 4.721 ms. `fold` is 0.600 ms of that in self time and `closes` is 0.597 ms.

The class doc already records one round of this work, and says the fold "was measured at an
eighth of a streaming run's whole CPU time". It is now 6.9 per cent of the whole recording and
28 per cent of the frame, which is the same finding read against a different denominator.

It runs on the frame that a mesh arrives on. A paged chunk is one merged geometry of every static
in it, so a ring brings tens of thousands of triangles at once.

**What to do.** This is arrival work with no frame of its own. Either move it to the loading
threads beside the mesh read, or give it to the same worker the composite baker runs on. Neither
needs the fold to get faster.

## Finding 5 — chunk building still lands on the frame at speed

`QuadTreeWorld::loadRenderingNode` is 9.23 per cent of the `island-crossing` recording, and
`ObjectPaging::createChunk` under it is 8.06 per cent. `SceneUtil::Optimizer::optimize` inside
that is 5.90 per cent. That is a chunk being built inside `collect`, on the asking frame.

`TerrainResidency` already runs a warming thread that aims 30 steps ahead, and it is doing work:
`TerrainResidency::warm` is 2.08 per cent of the recording. At 12,000 units a second the lead is
not enough, and the frame builds what the thread has not reached.

**What to do.** Measure the lead again at this speed before changing it. The class doc records
that doubling it made the median worse, so the answer is probably not a larger number. A warming
thread that follows the route rather than extrapolating the last several frames would aim better,
and `bench` is the one caller that knows the route.

## What is not a problem

**Physics.** `PhysicsTaskScheduler::doSimulation` is 10.42 per cent at Seyda Neen and 8.23 per
cent at the mages guild, and it runs on its own threads. What the frame itself pays is
`World::updatePhysics` at 0.07 ms, which is the scheduling and the wait for the workers.

**Recast.** `AsyncNavMeshUpdater::process` is 29.98 per cent of the Vivec recording and 22.21 per
cent of the streaming one. It is the initial navmesh build on a worker thread. This box has 32
threads, so it takes nothing from the frame; it does mean a Vivec recording's percentages have a
denominator half of which is not the frame.

**The interface.** `MyGUI::RenderItem::renderToTarget` is 0.068 to 0.090 ms a frame.

**`World::updateFocusObject`.** 0.118 to 0.251 ms a frame. It is upstream's crosshair pick, and
it is a Bullet ray cast per frame.

## Ranked

**Superseded.** Items 1 and 3 have since landed, and half of 4 — the `fadeThrough` half.
`.notes/rtx/cpu-redesign.md` holds 2, 5 and the other half of 4, against numbers re-taken after the
three. What follows is left as the reading of the day it was taken.

1. **Move `SpriteShade` to the device.** 2.4 ms a frame at Vivec, which is the only place the
   host is the limit. It removes the one CPU-bound row in the corpus.
2. **Take `ShapeFold` and the chunk build off the frame path.** Together they are 2.9 ms of the
   4.7 ms streaming frame, and both are arrival work with a thread already beside them.
3. **Give `bench` an unsettled leg.** Until then no streaming number describes the game, and the
   p99 the gate reports is the harness's own wait.
4. **Cache what `fadeThrough` asks, and flatten the identity maps.** 0.113 ms and 0.152 ms a
   frame at Seyda Neen. Both scale with the drawable count, so both are worth most where the walk
   is already worst.
5. **Gate the walk on motion.** The largest remaining item and the least defined. Nothing is
   wrong with walking the world whole. It is simply what an exterior's walk is, once the paged
   terrain and the fold come out of it.

## How to repeat this

```
cd build-release
for suite in default exteriors skies interiors streaming; do
    ./openmw-rtxtool bench --validation=false --window=false --suite=$suite --seconds=20 --json=$suite.json
done

apps/rtxtool/profile.sh --view=vivec
apps/rtxtool/profile.sh --view=island-crossing
```

Warm the card with one thrown-away `bench` first. The clock and the temperature are printed
beside every result, and a leg whose clock differs from its neighbour's is the leg to repeat.

**The off-CPU profile was not taken.** `profile.sh --offcpu` needs root for BPF, and this session
had no password. It is the instrument that would name the blocking call in Finding 1 outright,
rather than by the difference between `place` and the on-CPU cost of `hand`.
