# Loading: what an arrival frame pays, and a plan to stop paying it on the frame

Written at `8918103993` plus the working tree, against `.notes/bench.txt` and the two notes
`8918103993` removed (`cpu-performance.md`, `optimisation-proposal.md`, both readable from git).
Every number below is from this box unless it says otherwise: RTX 4090 Laptop, i9-13980HX,
`apps/rtxtool/release.sh`, `bench --views=island-crossing --seconds=20 --settled=false
--window=false --validation=false`, four legs interleaved, one thrown-away warm-up first.

## Where it stands today

**Nothing that stands still is short of anything.** Every standing place in the corpus waits on
the device with 2.4 to 3.8 ms of host headroom. **The frames a cell ring arrives on are the whole
of what is left**, and the island route is the instrument for them.

Two legs each, interleaved. `on` is the tree as it is. `off` is `--distant-statics=false`, which
turns the game's object paging off entirely: the active grid's statics are stood one by one and
nothing stands in the distance at all.

| leg | median | mean | p95 | p99 | worst | walk mean / worst | fold mean / worst | place mean / worst | update p99 / worst | 1% low | instances |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| on | 5.67 | 9.45 | 21.97 | 63.63 | 121.86 | 2.65 / 81.75 | 1.34 / 67.57 | 1.48 / 35.96 | 19.89 / 50.54 | 15.7 | 2484 |
| off | 7.28 | 7.90 | 13.83 | 24.54 | 45.71 | 1.51 / 22.71 | 0.32 / 14.87 | 1.21 / 11.24 | 12.90 / 28.51 | 40.7 | 1747 |
| on | 6.58 | 10.42 | 24.33 | 64.17 | 139.22 | 2.74 / 96.37 | 1.37 / 79.55 | 1.56 / 38.23 | 21.14 / 51.77 | 15.6 | 2484 |
| off | 7.14 | 7.30 | 11.64 | 20.73 | 42.48 | 1.34 / 22.68 | 0.31 / 15.32 | 1.05 / 10.73 | 11.42 / 25.22 | 48.2 | 1747 |

The `off` legs ran at a lower clock (1785–1935 MHz against 1770–2265), so their medians are not
comparable and the tails are. Structures: 72.3 MiB against 14.3. `blas` zone: 0.75 ms a frame
(2.04 on 363 frames of 1200) against 0.12. Textures: 355 against 228.

Three things the table says:

1. **Two thirds of the tail is the paged statics, again.** p99 64 to 22, worst 130 to 44, one per
   cent low 15.7 to 44 fps. This is the reading `cpu-performance.md` Finding 1 took, re-taken on
   this tree, and it has not moved.
2. **The game's own loop is in it too.** `update` — upstream's `Scene::changeCellGrid`, which
   loads cells synchronously — has a p99 of 20 ms with the paging on and 12 without. The likely
   difference is the active grid's object chunks: where the preloader has not kept up, the grid
   change ends in a *synchronous* terrain preload (`Scene::preloadTerrain(pos, worldspace, true)`
   → `CellPreloader::syncTerrainLoad`), and `QuadTreeWorld::preload` builds every chunk manager's
   chunk, the merged statics included. A profile of the `update` row by thread is what confirms
   it, and it is measurement 3 below.
3. **The `off` legs still fold**: 0.31 ms a frame, 15 ms at the worst. That is the active grid's
   own templates, each folded on the frame it is first met. Nothing is paged there; it is simply
   that the fold happens where the template is first seen, and the template is first seen on the
   frame.

**What the `off` legs are not**: a picture. A world with nothing on its distant ground is not the
world this fork draws, so the `off` row is the *floor* the frame can reach and not a target to ship.
The question is how to put the distant statics back at that price.

## What the frame does on an arrival, and who does it

Four sources hand geometry to the mirror, and they arrive by four routes.

**The active grid's objects** are nodes on the graph. `Scene::loadCell` clones a template per
reference (`SceneManager::getInstance`), and `SceneUtil::CopyOp` shares every plain
`osg::Geometry` between the clones — only rigs, morphs and particle systems are deep copies. So
the drawable the walk meets under a hundred crates is the template's one drawable, and
`MeshResolver`, keyed on the drawable, folds and uploads it once. The first sight is on the
frame; every later sight is a map lookup.

**The paged statics** are `Terrain::ObjectPaging` chunks, and they are the problem. The quad tree
decides a decomposition from the eye — centre, size, level — and the paging merges every static
of one kind inside a chunk into one geometry, keyed on that decomposition. `object paging active
grid` is on, so this covers the cells the player stands in as well as the distance:
`Scene::addObject` skips every reference `getPagedRefnums` names, and the game never stands those
statics itself. What reaches the mirror is a few very large drawables per chunk, each unique to
its chunk, built on the game's work thread (92% of the build is already off the frame) and then
**folded, resolved and walked on the frame that first meets it**. `ShapeFold` matches every
triangle against its reversed twin; a merged chunk is tens of thousands of them. Two attempts to
move that fold off the frame were built, measured and withdrawn: the warming thread aims at a
decomposition the frame does not ask for (a 1.6% hit rate), and walking a chunk's subtree from
any thread but the frame's crashes in a race nothing in `components/rtx` can name.

**The ground** is the same quad tree's terrain chunks, collected through `TerrainResidency`,
warmed a little ahead by a thread of its own, replayed where the chunk name has not changed
(98.5% of them on the route), and its composites are baked on the `CompositeQueue` thread.

**The distant lights** are read straight out of the content files by `DistantLights`, through
`Terrain::ObjectStorage::collect`, once per cell for the life of the scene — on the frame, blocks
off the disk included.

Then the hand-over. `SceneUploader::hand` describes the arrived textures (the file's own levels, a
`MipChain` for what the file did not carry, a shading estimate over every texel), and
`VulkanRenderer::extendScene` stages the geometry into the blocked buffers, writes the images,
records the bottom-level builds onto the placement's submit and rebuilds the top level. `place`
is 1.5 ms mean and 36 worst on the route, and the profile puts about 1.1 ms a frame of that in
the driver's own allocate and free.

And beside all of it, `update`: upstream's grid change, cell insertion, physics and the
synchronous terrain wait. Not this fork's file and not this fork's thread.

## The idea as asked: a background thread loads more cells than the renderer renders

Read against the code, this is two ideas and both are right, with one condition.

**"More cells than the renderer renders."** This renderer renders everything it holds — there is
no frustum — so *rendered* here means *placed in the top level*, and *loaded* means *resident on
the device*: meshes in the blocked buffers, structures built, textures uploaded. The idea is a
ring of cells prepared one wider than the ring that is placed, so that when a cell crosses into
the reach its geometry is already there and only its placements move. That is exactly the shape
that turns an arrival frame into a few hundred `addInstance` calls.

**"Prepare a data format for fast renderer consumption."** The scene description already is that
format: `SceneDesc` is flat, slot-addressed, and appended to rather than rebuilt, and
`extendScene` uploads only what arrived. What is *not* in that format when a cell arrives is the
work between a template and a row — the fold, the vertex read, the material read, the texture
describe — and all of it runs on the frame today. Doing it on a worker and handing the frame rows
to copy is the second half of the idea.

**The condition.** Neither half touches the paged statics, because a paged chunk cannot be
prepared ahead: its identity is the eye's decomposition, so a worker preparing "the next cell"
builds chunks the frame will not ask for, and the chunk it does ask for cannot be walked off the
frame. The plan that withdrew the fold cache measured both. So the idea works **only if the
ray tracer stops consuming paged chunks** — and once it does, the idea is the whole of what is
needed.

## The better idea: the ray tracer stands its distant statics itself, from the content files

**Instancing is what a ray tracer has that a rasterizer does not.** The paging exists to turn
a thousand draw calls into a few; a top-level acceleration structure takes a thousand instances of
one bottom level as one entry apiece and traces them at the same price as a merged copy. Merging
buys this renderer nothing and costs it the fold of every merged chunk, on the frame, at every
decomposition change.

**And the seam is already there.** `Terrain::ObjectStorage` is documented as "what the paging
and the ray tracer ask of the content files": which references stand in a square of cells, reduced
by reference number the way content files stack, with `pagedType` deciding what a distant
hillside is made of. `DistantLights` already reads it. The statics are the same call with
`RefKind::Paged` — the forty lines of `ObjectPaging::createChunk` that read records, without the
thousand that merge scene graphs.

So: **`Rtx::StaticRing`**, in `components/rtx/`, a `Residency` beside `DistantLights` and
`TerrainResidency`.

- **A thread of its own**, as `TerrainResidency` and `CompositeQueue` have. Not the game's
  `SceneUtil::WorkQueue`: that has one thread and the cell preloader lives on it, and a ring that
  queued a hundred cells there would push the game's own preload behind them.
- **Per cell**, on the thread: `ObjectStorage::collect(Paged, 1, cell)`, then the rules
  `createChunk` applies and no others — the hidden marker, an empty model, `correctMeshPath`,
  the `_dist`/`_far`/`_lod` name from `getEsmVersion` — then `SceneManager::getTemplate(model,
  false)`, which is thread safe and is what `CellPreloader` and `ObjectPaging` call from the same
  kind of thread. The template is walked for its drawables and state sets **on the thread**.
  This is what upstream's own `AnalyzeVisitor` does inside `createChunk` on the work thread, so it
  is the walk that is known to be safe; the one that crashed walked a *chunk* built by another
  thread and cached by a third.
- **The reading, not the insertion, moves.** `MeshResolver::resolve` today reads vertices, folds
  and calls `SceneDesc::addMesh` in one go, and `MaterialResolver::resolve` reads a state set and
  calls `addMaterial`. Each is split into a *reading* — a pure function of the drawable or the
  state set into a value the caller owns (positions, normals, texture coordinates, the folded
  indices, `FoldedShape`; a `Material` and the texture paths it names) — and an *adopt* that
  inserts a reading under the same identity `resolve` would have used. The worker makes readings
  with a `GeometryFold` of its own; the frame adopts them. One answer about what a shape is,
  because both paths call one reading. The withdrawn `FoldCache` was this seam beside the
  resolver; this puts it inside.
- **What a prepared cell is**: the templates it names (held by `ref_ptr`), one reading per
  drawable and per state set the ring has not adopted yet, and one placement per reference —
  template, local transform, radius, `RefNum`. A few kilobytes of rows and the folded geometry of
  whatever models were new.
- **The frame's commit, once a frame, one cell at most.** Adopt the new meshes and materials (a
  copy into the blocked tables, no fold), record the placements the ring owns, hand the textures
  the worker described to the uploader. A cell that arrives while another is being adopted waits a
  frame. Nothing is batched behind a threshold and no frame absorbs a second cell.
- **Two rings.** *Prepared* is the reach plus one cell — at the default of four cells that is
  11×11 against 9×9, so a cell is prepared while it is still a cell away from being seen. *Placed*
  is the reach less the active grid. A prepared cell's meshes exist on the device — the bottom
  levels are built by the `extendScene` of the frame that adopted it, out of sight — and its
  placements are added on the frame it crosses into the reach. At the route's 12,000 units a
  second that lead is 0.7 s; at a walk it is four.
- **The size rule is per object and per frame**, which is where this is stricter than the game
  rather than looser. `createChunk` drops a reference whose scaled radius is under `object paging
  min size` times the chunk's reach, and decides it once for the chunk's life. The ring keeps every
  reference with its radius and places the ones whose radius clears `minSize × distance(eye,
  cell)` this frame: a compare per prepared placement — a few thousand, no allocation — and an
  `addInstance` or `dropInstance` where one crosses. The threshold is the game's own number, told
  to the ring by the host the way the reach is, because `components/rtx` reads no settings.
- **The sweep keeps what the ring holds** because the ring's meshes and materials sit in the
  resolvers' identity maps under the same keys the graph walk uses — the template's drawable, the
  template's state set — and the ring stamps them every frame the way a chunk replay does. The
  placements are the ring's own slots and never the extractor's: they are dropped when a cell
  leaves the prepared ring, and the sweep never sees them.
- **The distant-to-active transition is free.** When a cell enters the active grid the game
  stands its objects as clones of the same templates, the walk meets the same drawables, and
  `MeshResolver` finds the mesh the ring adopted. The ring drops its placements for that cell on
  the same frame; the meshes, materials and structures stay. In the other direction the same
  thing backwards. The `fold` row of the `off` legs — 0.31 mean, 15 worst — goes with it, because
  the template was read on the ring's thread before the cell was active.
- **Object paging goes off for this renderer**, decided at the seam the way `wantsPagedTerrain`
  is: a `Renderer::wantsObjectPaging()` that `RenderingManager` reads where it reads the setting
  today. With no `ObjectPaging` the quad tree builds terrain and nothing else, the game stands
  every active-grid static itself, and `update`'s synchronous terrain preload no longer merges
  chunks — which is where the 20 to 12 ms of the table most likely comes from. The harness's
  `--distant-statics` becomes the ring's switch, and `false` is the same A/B it is today.

**What the frame is left with on a crossing.** Walking the new active cells' subtrees (individual
nodes now, every mesh already known), adopting one prepared cell's rows, and `update`. Of those,
only the walk is this fork's, and the `off` legs already show what it costs: 1.3 to 1.5 ms mean.

## What it costs, and what has to be measured before it is built

**Instances.** A merged chunk is one instance; its statics are many. The `on` legs hold 2484
instances with the merges in them. A cell has one to four hundred paged references and the size
rule removes most of the small ones at distance — a radius of 25 to 82 units survives at one cell
out and 98 to 328 at four, depending on how much a merge would have bought — so the ring's placed
count is a few thousand, likely under ten. The top level
rebuilds every frame at 0.21 ms for 2484; it is linear, and the number that decides the design is
**measurement 1** below. Above about twenty thousand the answer changes: the outer band would
place at a coarser rule, which is the game's own LOD by another name.

**Memory.** The `on` legs hold 72 MiB of structures, most of it merged chunks that are unique per
decomposition. Per-template structures are shared by every instance; the `off` legs hold 14 MiB
for the active grid's templates, and a ring of the same models is not much more. The prepared
ring adds one band of cells: 40 more of 121, with their textures. Bounded, and printed by every
bench beside the frame.

**The picture changes, once.** A distant static stops being a merged copy with the paging's own
LOD cuts and becomes the template, with the `_dist` model where the content ships one. `verify`
will move at every exterior view, and that is read once as the new reference. Per-object pop at
the size threshold replaces per-chunk pop at a rebuild — a smaller event, more often. Nothing else
about shading changes: the same drawables, the same state sets, the same materials.

**Determinism.** Which frame a prepared cell is adopted on is the ring thread's answer, as a
composite's is today. `--settled` already names the rule: a settled run waits for what it queued
before it takes any, and the ring follows `CompositeQueue::setSettled` — a settled stop adopts
nothing until every cell of the prepared ring is prepared, so the placed set is a pure function of
the eye and `repeatable.sh` holds. An unsettled run streams, which is what the crossing is measured
under.

**Runtime changes to references.** `World::enable`/`disable` and the four `pagingBlacklistObject`
sites in `worldimp.cpp` reach `RenderingManager::pagingEnableObject`, which today reaches
`ObjectPaging` and with it off reaches nothing. The blacklist is active-grid only and the ring
never places inside it, so it needs nothing. `enable`/`disable` on a static in a distant cell is a
fact the ring must be told, or it draws a disabled ghost fence. **This is the one upstream touch
the plan names and waits on**: `RenderingManager::pagingEnableObject` forwarding to the renderer
seam beside the paging, two lines in `apps/openmw/mwrender/renderingmanager.cpp` and a virtual on
`Renderer`. Until it lands the ring starts from the content files, which is also where the
rasterizer's paging starts after a save is loaded.

**Threads.** `getTemplate` on two threads at once (the game's preloader and the ring) is what the
scene manager documents as safe. The ring's walk over a template is read-only over an object the
cache holds by `ref_ptr` and nobody animates. The shading estimate and the mip chain are pure
functions of an `osg::Image`. What must not happen on the thread is anything into `SceneDesc` or
the identity maps, and the split in stage 1 is what makes that a type rather than a rule.

### Measurements, in order, each before the stage that depends on it

1. **How many instances the ring places**, per view of the `exteriors` suite, at the game's size
   rule: a count off `ObjectStorage::collect` over the reach, with the radius rule applied from
   each template's bound. Twenty lines in `openmw-rtxtool scene`, and it decides the outer band.
2. **What preparing a cell costs on the thread** — collect, templates, readings, describes — from
   cold, for the reach around three views. Decides whether a teleport's loading screen has to wait
   for the ring (it should, under `--settled`; the game need not).
3. **The `update` row by thread**, `profile.sh --view=island-crossing --settled=false` with the
   paging on and off, to confirm that the 8 ms between the two p99s is the synchronous chunk
   build. The `off` legs themselves are the floor and are already taken, above; the ring's gate is
   those figures with the distant statics standing.

## The plan

Each stage is a commit and states its gate. The gate for every stage:

```
cmake --build build-debug --target components-tests openmw-rtxtool
build-debug/components-tests --gtest_filter='Rtx*'
apps/rtxtool/repeatable.sh --pairs=10
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

and `scene --twice` at three views against the previous build's digest wherever a stage claims the
scene is unchanged.

### Stage 0 — the readings and the seam

Measurement 1 and 2. Name the `pagingEnableObject` forwarding and `wantsObjectPaging` on the
renderer seam, with the two-line change to `renderingmanager.cpp`, and wait for the go-ahead. Both
are the seam's own kind of change — a question asked of whatever holds the answer — and neither
changes what the rasterizer does.

### Stage 1 — the resolvers split reading from insertion

`MeshResolver`: a `MeshReading` (positions, normals, texture coordinates, folded indices,
`FoldedShape`, what deforms) made by a function that takes a drawable and a `GeometryFold`, and an
`adopt(drawable, reading, material)` that inserts it under the drawable's identity. `resolve`
becomes the two in sequence. `MaterialResolver` the same with `MaterialReading`. **No behaviour
change**: `scene --twice` reports the same digest at three views, and the extractor's tests pass
unchanged. The tests that exist for the fold keep covering the reading; a new one proves
`adopt` of a reading and `resolve` of the drawable land on identical rows.

### Stage 2 — `Rtx::StaticRing`, placed ring only

The residency, its thread, the per-cell preparation, the frame's commit of one cell, the stamps,
the placements with the per-object size rule, and the host wiring in `WorldMirror` beside
`DistantLights` (`follow(storage, sceneManager, worldspace)`, `setReach`, `setActiveGrid`,
`setViewPoint`, `setMinSize`, `setSettled`). `wantsObjectPaging()` false on `RtxRenderer`;
`--distant-statics` drives the ring. The prepared ring equals the placed ring at this stage.

Gate: two interleaved pairs of the crossing against the `on` and `off` rows above. The claim is
the `off` row's tail with instances and structures near the `on` row's; `fold` at nought on every
frame; `warm` gone. `verify --exposure=1` is re-baselined once, with the reading written into
`.notes/bench.txt`. `check` gains a claim: the ring's placed count equals the count measurement 1
predicts at each view.

### Stage 3 — the prepared ring, one cell wider

The outer band, adoption ahead of placement, and the textures described on the ring's thread
(`SceneTextures` for the ring's arrivals, owned by the ring, handed to `extendScene` as
descriptions). The `place` row is the gate: its p99 and worst on the route, against stage 2.

### Stage 4 — the bottom-level builds off the placement's submit

The second queue `optimisation-proposal.md` item 1 designed, unchanged in shape: a compute family
at device creation, a timeline semaphore, the builds recorded into that family. It earns more here
than it did there, because a prepared cell's builds now land on frames where the cell is out of
sight and the trace need not wait for them at all. Gate: the `blas` zone and the frame's p95 on the
route.

### Stage 5 — the ground the same way

Later, and its own document. The same ring standing each cell's ground from `Terrain::Storage` —
the heights as one mesh per cell, the blend maps as one composite per cell baked on the queue that
already bakes them — would take `TerrainResidency`, its warming thread, the chunk replay and the
quad tree's level-of-detail seams out of this renderer, and put the `update` row's synchronous
terrain wait back into upstream's own hands. It is last because its share of the tail is the
smallest of the four and because its picture question — full-detail ground everywhere, at 8k
triangles a cell and 121 cells — deserves a reading of its own.

## What is deliberately not proposed

- **A cache of folded meshes on disk.** After stage 2 the fold is on the ring's thread, and a
  cold ring is a loading screen. A cache would save the thread work nobody is waiting for.
- **Loading more active cells.** `Scene::changeCellGrid` and `Constants::CellGridRadius` are
  upstream's, `update` is their cost, and a wider active grid simulates more actors and scripts
  for a picture the ring already gives.
- **A fold cache under the warming thread.** Withdrawn twice; the chunk it would cache goes away.
- **Keeping the paging on and skipping its chunks.** `QuadTreeWorld::preload` and
  `loadRenderingNode` build every chunk manager's chunk whether or not a taker wants it, so a ring
  beside a live `ObjectPaging` would pay for merges nobody traces, on the thread and in `update`.
