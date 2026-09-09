# What to optimise next, and how

Written at `4f2b88db9d` against `.notes/rtx/cpu-performance.md`, `.notes/rtx/gpu-performance.md` and
`.notes/rtx/gpu-techniques.md`. Every figure below was measured on this box, and where one was not,
the item says so.

## Where the renderer stands

**The stated target is met.** At 1920×1080 internal to 3840×2160, the three places measured read 74.8
to 90.6 fps at the median and 56.7 to 65.4 at the one per cent low, against the 60 the target names.

**Nothing that stands still is short of anything.** At 1920×1080 out, every place in the corpus waits
on the device, with 2.4 to 3.8 ms of device headroom and a host frame of 0.90 to 3.46 ms. Work on a
standing frame buys nothing today, whichever side it is on.

**One thing is short, and it is the frames a cell ring arrives on.** The island route reads a median
of 6.0 ms against a p99 of 64 and a worst of 128, which is 15.5 fps at the one per cent low. That is
what a player feels crossing the map, and it is the whole of what is left.

**So the list below is ranked by what it does to an arrival frame**, and the last two items are on it
only so that nobody has to rediscover why they are last.

## 1 — The acceleration structures move to a second queue

**What it is worth.** Over the island route the device spends **0.81 ms a frame baking micromaps and
0.69 building bottom levels**, and both land on the frames a ring arrives on — the frames whose p99 is
49 ms. Today they are serial with the trace, because there is one queue.

**Why now.** Every source says the same thing, and this is the one structural recommendation the tree
does not follow. NVIDIA: "Move AS management (build/update) to an async compute queue, which pairs
well with graphics workloads and in many cases hides the cost almost completely."

**The shape.**

- `Device` takes a second queue at creation, from a family that offers compute without graphics.
  This card has one: family 2, compute and transfer, eight queues. A device that offers no such
  family keeps the single queue it has, which is the same code path as today.
- The micromap bake and the bottom-level builds are recorded into a command buffer of that queue's
  own family and submitted there.
- A timeline semaphore orders them: the placement's submit signals what the builds wait on, and the
  builds signal what the frame's trace waits on. Nothing waits on the host.
- `mBuilding` is untouched. That mutex holds two *host* threads out of `loadRenderingNode`, and this
  is the device side.

**The risk.** The sources warn that on hardware older than Ampere, overlapping compute on the
graphics queue with a dedicated async queue can leave gaps in the async queue. The floor is Turing,
so this needs a reading on the floor and not only on this card — and if it costs there, the second
queue becomes a decision made once at device creation, which is the shape `Rtx::Reorder` already has.

**How it is verified.** `bench --views=island-crossing --seconds=20 --settled=false`, two legs of each
interleaved, reading the `micromap` and `blas` zones, the frame's p99 and the one per cent low.
`scene --twice` at three views for the digest, and `repeatable.sh --pairs=10`.

## 2 — The fold moves off the frame, through a seam upstream has to open

**What it is worth, measured.** Built, measured and withdrawn once already. Two legs of each,
interleaved, on the island route:

| leg | median | mean | p99 | worst | `walk` mean | `fold` mean | 1% low |
|---|---:|---:|---:|---:|---:|---:|---:|
| before | 6.06 | 10.20 | 65.04 | 126.01 | 2.85 | 1.41 | 15.4 |
| after | 6.25 | 9.57 | 48.28 | 82.52 | 1.88 | 0.45 | 20.7 |
| before | 6.01 | 10.25 | 63.99 | 128.73 | 2.82 | 1.43 | 15.6 |
| after | 6.08 | 9.49 | 47.76 | 82.11 | 1.87 | 0.42 | 20.9 |

**A third of the one per cent low**, a quarter of the frame's p99 and a third of the worst frame. The
scene was identical at three views, digest included.

**Why it is not done.** The filling side walked a paged chunk's subtree from the warming thread, and
that crashes: two runs of six, then six of twelve after the one repair it earned. Bisected — the same
`collect` with a taker that does nothing is safe, and the one that walks each chunk is not. Nothing
in `components/rtx` can say what it races with.

**What would make it possible.** The geometry has to be handed over by the thread that built it
rather than walked by one that did not. That thread is the game's own preloader, and the merge
happens in `components/terrain/objectpaging.cpp` — an upstream file.

**The change to name and wait on.** `ObjectPaging::getChunk` already knows the moment a chunk's
merged geometry becomes complete, because it is the line that puts it in the cache. A hook there —
an observer this fork registers, told the node that was just built — is the whole of it: no new
thread, no lock, and the fold then happens on the thread that already holds the geometry.

**This item is a proposal to the tree's owner and not a plan.** `AGENTS.md` says an upstream change
is named and waited on, so it is named here.

## 3 — Two exact early exits in `ShapeFold::closes`

**What it is worth.** Unmeasured, and honestly so. `closes` is 0.62 ms of the fold's 1.36 ms mean on
the island route — about half — and its answer for a merged terrain chunk is almost always no. What
the exits are worth is what fraction of that half they cut, and only a run says.

**Why it is worth trying anyway.** It needs no thread, no lag and no upstream change, and both exits
are arithmetic rather than a heuristic.

**The shape.** A closed shape has every edge on exactly two triangles, so it has exactly `3T/2`
distinct edges. Two things follow, and neither is a guess:

- `T` odd cannot close, because `3T/2` is not an integer. That exit fires before a single edge is
  hashed.
- More than `3T/2` distinct edges cannot close. That exit fires inside the insertion loop, as soon as
  the count passes the limit — which for a merge full of grass cards, each contributing five distinct
  edges for two triangles, is well before the end.

**The one way to get it wrong.** A shape that closes reaches the limit exactly, so the refusal is
for a push that would *exceed* it and never for one that reaches it. The tetrahedron in
`apps/components_tests/rtx/shapefold.cpp` is four triangles and six edges — exactly `3T/2` — and it
must still read closed. That test is the guard, and it is already written.

**How it is verified.** The `fold` row on the island route, two legs of each. A test that a closed
shape still closes and that an odd triangle count is refused before any work.

## 4 — Settle whether a refitted structure may be compacted

**What it is worth.** Memory rather than frame time. Indiana Jones compacts its dynamic bottom levels
and took its vegetation from 1027 MB to 606 — 41 per cent, with individual structures halving.

**Why it is open.** `BottomLevelStore` deliberately withholds `ALLOW_COMPACTION` from what deforms:
"A mesh that refits is left out: a refit writes back into the slack." The source does the opposite,
under one constraint — a compacted structure must be refitted and never rebuilt afterwards.

**One of the two is wrong, and the specification says which.** This item is a reading, not a change:
if the specification allows an update of a compacted structure, the flags change and the memory
follows. If it does not, the comment gains the reference that settles it for good.

## 5 — The walk reads what it wrote, over the game's own graph

**What it is worth.** About 0.2 ms of a standing frame — the three `std::unordered_map` lookups every
drawable makes, at 0.159 ms at Seyda Neen and 0.232 at Vivec, plus what `osg::Group::traverse`'s
child loop costs above them.

**Why it is last.** Every standing place has 2.4 to 3.8 ms of device headroom and waits on the card.
Two tenths of a millisecond of host time at a place that is already device-bound buys nothing at all,
and the design carries the highest staleness risk in the tree — a trace that goes wrong mirrors the
wrong geometry over a graph the game writes.

**It is here so that it is not proposed again as though it were new.** `.notes/rtx/cpu-performance.md`
holds the design.

## What is deliberately not proposed

- **The upscaler.** It is the largest cost in every frame — 2.27 to 2.57 ms at 1080p and 5.26 to 5.98
  at the target — and the only knob on it is which extent to trace. The target already names one that
  fits, and the picture is what the fork is for.
- **The top level.** 0.24 ms a frame, rebuilt every frame. The guidance is to rebuild it every frame
  with `PREFER_FAST_TRACE` whatever moved, which is what this tree does.
- **Reordering, opacity micromaps, the ray flags and the build flags.** Every one was measured here
  and every reading agrees with what the sources say about a frame shaped like this one.
- **The sea's spectrum.** 0.22 ms a frame wherever a cell holds water, whatever the camera can see.
  Skipping it needs a frame-late answer about whether any water was hit, which makes what a picture
  holds depend on how many frames came before it — the objection the tree already raised against a
  budget for the distant lights.
- **The game's own loop.** `update` is 1.25 ms a frame on the route with a p99 of 23 and a worst of
  50. It is upstream's cell loading, and it caps what any of the above can buy on an arrival frame.

## The plan

Each stage is a commit, and each states what it is verified by.

**The gate for every stage**

```
cmake --build build-debug -j32 --target components-tests openmw-rtxtool
build-debug/components-tests --gtest_filter='Rtx*'
apps/rtxtool/repeatable.sh --pairs=10
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

and, for anything that could move a picture, `scene --twice` at three views against the previous
build's digest.

### Stage 1 — the cheap one first

1. **The two exits in `ShapeFold::closes`.** A test that a closed shape still closes, and the `fold`
   row on the island route, two legs of each. Keep it if it moves the row, delete it if it does not —
   it is fifteen lines either way.

### Stage 2 — the structural one

2. **A second queue at device creation**, chosen once, with the single-queue path kept for a device
   that offers no compute-only family. Nothing uses it yet. Verified by `info` reporting both
   families and by every existing gate still passing.
3. **The micromap bake and the bottom-level builds submitted there**, ordered by a timeline
   semaphore. Verified by the island route's `micromap` and `blas` zones, its p99 and its one per
   cent low, and by the scene digest.

### Stage 3 — the readings

4. **Read the specification on compacting a structure that is later refitted**, and either change the
   flags and measure the memory, or write the reference into the comment that says why not.
5. **Take a reading on Turing**, if one can be reached, for the second queue's pre-Ampere caveat.

### Stage 4 — only if somebody opens the seam

6. **The fold off the frame**, once `ObjectPaging` can say what it built. The measurement above is
   what it is worth, and it was taken on a working implementation of everything except the hand-over.
