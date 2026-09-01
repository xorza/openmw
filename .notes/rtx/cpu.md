# The CPU side of a frame

**The goal is less CPU work, not a faster frame.** At this view the device is the wall, so most of
what is below buys headroom rather than milliseconds — for a heavier scene, a faster card, a
game frame that carries more than the harness does, and for the power a laptop spends. Judge an item
by whether the work is gone, not by whether the frame moved.

Open work only; an item applied or ruled out is deleted rather than marked.

## How to measure

`bench` defaults to `--seconds=20 --warmup=3`. Ten seconds left the CPU medians moving by more than
the changes being measured.

**The machine has to be idle.** With a browser and an editor running, identical builds measured
`walk` at 3.96 and 5.45 ms. Check with `top -bn2` — `ps` prints lifetime averages, not current load,
and reading those as current is a mistake that has already been made here once.

**Interleave and balance the order.** Alternating A,B,A,B is not enough: runs drift within a
sequence by more than the effect, which flatters whichever ran second. Use A,B,B,A,A,B,B,A with two
saved binaries (`cp` them into `build-release/`, no rebuild between runs). On an idle box the
within-build spread is about 0.06 ms, so 0.15 ms is readable; with anything else running it is 0.2
and nothing small can be judged at all.

**Hold the world still with `--people=0`**, which is what the option is for.

**`perf` shares are the better metric for this goal.** Wall time hides work that overlaps the
device; a function's profile share falling to nothing is unambiguous proof the work is gone.
Instruction counts are *not* usable — `perf stat` over a whole run varies ±3% here, which swamps
everything.

    apps/rtxtool/profile.sh --view=seyda-neen-shore --people=0 --seconds=10

**One caveat no warmup fixes.** A static camera still reports one cell crossing of ~0.5 s inside the
measured window — distant terrain paging in asynchronously — at warmups of 1, 5 and 10 seconds
alike. It offsets both sides of an A/B equally, but it is in every profile.

## A. What bounds a frame, so the ceiling is known

| config | frame | wait on GPU | CPU walk | bound by |
|---|---|---|---|---|
| `--people=0 --props=0` | 6.01 | 2.87 | 2.53 | GPU |
| `--people=0` | 5.52 | 1.99 | 2.73 | GPU |
| default | 6.67 | **0.00** | 5.20 | CPU |

**A static scene is GPU-bound and the walk is hidden.** Proved by gutting it: a build that does no
walk at all past the first 300 frames takes `walk` from 2.54 ms to 0.00 and leaves the frame at 5.77
against 5.84 — `wait` rises from 2.86 to 5.36 and absorbs it exactly.

**Only animated residents push it over**, by about 2.5 ms. So the frame-time ceiling for all CPU
work here is ~0.35 ms, and past that the device is the wall. That is the number to weigh a
*frame-time* claim against; it is not a reason to leave work in place.

## B. Where the remaining work is

Profile shares with residents held still, after the three changes already applied:

| | self |
|---|---|
| `osg::Group::traverse` | 12.8% |
| `SceneExtractor::addDrawable` | 9.4% |
| `SceneExtractor::resolveMesh` | 7.5% |
| `SpriteShade::layDown` | 4.1% |
| `osg::Matrixf::compare` | 2.8% |
| `osg::StateSet::getUniform(std::string)` | 2.7% |
| `SceneUtil::RigGeometry::cull` | 2.6% |
| `osg::Matrixf::mult` | 2.4% |
| hash node iterator over `StateSet` keys | 2.4% |
| `SceneExtractor::retire` | 1.9% |

**B1. `addDrawable` and `resolveMesh` are now mostly one hash lookup apiece** against maps of tens
of thousands of entries — a cache miss, which is why the share is large where the arithmetic in them
is small. Narrowing it means a cheaper identity than a hash map, not a cheaper hash. The obvious
shape is a slot index cached on the drawable itself, which is a design change rather than a tweak.

**B2. `getUniform(std::string)` at 2.7%**, from `fadeThrough`, once per state set per drawable per
frame. The `std::string` is already static; OSG still walks a `std::map` keyed by string. A state
set's fade cannot change unless the state set does, so it is cacheable on the state-set pointer —
trading a string-map walk for a hash lookup, which wants measuring rather than assuming.

**B3. `SpriteShade::layDown` at 4.1%** is the largest item never looked at.

**B4. What is left of `retire` is the meshes and materials sweep**, 1.9%, which unlike the
placements cannot simply be skipped: it fills the live lists `SceneDesc::release` consumes. Those
lists are unchanged whenever nothing was added or dropped, so the same counter trick works with a
size comparison beside it.

**B5. `Matrixf::compare` 2.8% and `mult` 2.4%** are `moveInstance` asking whether a placement moved
and the walk accumulating `mHere`. Both are real work on a scene that did not move.

## C. The residents, which are the other 2.5 ms

`RigGeometry::cull` and the deforming path through `resolveMesh`, which re-reads the vertex arrays
for every deforming drawable every frame. That is correct — a pose really did change — and it is the
only per-frame vertex traffic in the renderer. Whether it can be narrowed to actors actually in
view, or settled more cheaply with the skeleton, is unmeasured.

## D. Ruled out

**Skipping unchanged subtrees buys no frame time**, which is what it was proposed for: the ceiling
test in section A removes the walk entirely and the frame does not move. It remains the largest
*CPU* prize by far, so it is not ruled out for this file's goal — but it needs the extractor to know
a subtree is untouched, OSG offers no dirty flag, and a missed invalidation is a stale scene rather
than a crash. Do B1–B5 first; they are safe, and they will say how much is left for it.

**A profile share is not a frame-time cost when the work is off the critical path.** That is what
the ceiling test exists to catch, and it has caught two items here already.
