# The ground: stood by the cell ring from the land records

Written at `6d1147cc17` plus the working tree, after stages 0 to 3 of `loading-plan.md` landed. That
plan's stage 5 is this document. Every number is from this box unless it says otherwise.

## Where it landed

**Stage 5a is built.** `.notes/bench.txt` at `07:00` holds the legs, three order-balanced pairs of
the crossing against the binary of the batch before it.

| leg | median | p95 | p99 | worst | fold mean / worst | walk p99 / worst | upload p99 | wait p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| before | 6.0 to 7.8 | 12.8 to 13.6 | 20.8 to 22.0 | 37 to 40 | 0.29 / 14.7 | 8.9 / 21 | 3.9 to 4.3 | 6.3 to 7.2 |
| after | 6.2 to 7.8 | 15.9 to 17.9 | 20.8 to 23.0 | 35 to 39 | 0.01 / 0.5 | 3.3 to 3.7 / 5 to 6 | 5.2 to 9.3 | 9.1 to 10.3 |

**The fold is gone and the walk's tail with it.** The crossing frames' own worst fell from 31–32 ms
to 16–22. **The frame's p99 and worst do not move**: what is left at the tail is `update`, which
stage 5b takes away, and the adopt frames. **The p95 rises by three milliseconds**, and it is the
adopt frames: a cell's ground is 8192 triangles copied, built and uploaded on the frame that adopts
it, and the frame waits on the device for it — `wait` p99 6.3–7.2 to 9.1–10.3. The `blas` zone
reads 0.41 ms on 404 build frames before and about 0.5 on 337 after, which does not account for
the wait on its own; an `nsys` timeline of an adopt frame is the next reading, and the loading
plan's stage 4 is the design that answers it.

**Standing, the trace does not see the detail.** At the shore and the ship, two order-balanced
pairs, the `trace` zone reads 2.07 and 2.55 before against 2.10 and 1.82 after, and 2.04 and 2.17
against 2.03 and 1.92 — sixty-four times the ground's triangles at one cell out, a thousand at
four, for nothing the zone can read. That is the picture question answered: full detail costs the
frame nothing where it stands still.

**What it holds.** Structures 29.4 to 42.6 MiB, textures 70 to 169 MiB — one 512² composite per
cell outside the active grid, 112 of them at the reach of four — and 11,163 instances against
11,211, 81 of them ground where some hundred and thirty chunks stood.

**The picture moved once, as said.** `verify --exposure=1` against the binary before: thirteen
of fourteen views, the exteriors by the ground itself (`ald-ruhn` worst 86 of 255 on a quarter of
the pixels, `sadrith-mora` 52 on two fifths) and the interiors by a few levels of noise following
the slot layout. Read once as the new reference. `check` holds `ground-stands` at every exterior:
81 cells against 81 in the reach.

**A lending race in the ring, older than this stage, came out under `check`** and is fixed at the
root: a hold on a model is a cell's, counted by the reader, and never the frame's to say.
`loading-plan.md` holds the account. **Vertex colours** are in `.notes/ISSUES.md`.
`repeatable.sh --pairs=10` is identical over every pair on both builds; 601 `Rtx*` tests pass, and
the exterior suite passes under AddressSanitizer.

## What the quad tree hands this renderer, and what that costs

**The ground arrives as `Terrain::QuadTreeWorld` chunks**, resolved against the eye by
`TerrainResidency::collect` on every frame. A chunk is named by its centre, its width and its
neighbours' levels of detail, so the set changes whenever the eye crosses a level-of-detail
boundary — and every chunk the walk has not met is folded on the frame that meets it. That is the
`fold` row the ring did not move: 0.30 ms mean and 15 worst on both legs of the 05:40 batch, and now
the largest item left on an arrival frame. `ChunkRuns` replays 98.5% of the chunks a frame is handed;
the fold is the other 1.5%.

**The distant ground is nine vertices across, whatever the chunk's width.** `DefaultLodCallback`
uses a chunk of width `size` at level `log2(size / minSize)`, `getVertexLod` keeps every
`2^level`-th vertex, and `minSize` is an eighth of a cell — so a one-cell chunk is 64 / 8 + 1 = 9
vertices a side, a four-cell chunk is 256 / 32 + 1 = 9, and every chunk outside the active grid is
128 triangles. A vertex every 1024 units at one cell out and every 4096 at four is a rasterizer's
answer for a 2002 screen, and it is what this renderer has been tracing hills with.

**`update` waits for the quad tree.** `Scene::changeCellGrid` ends in `preloadTerrain(pos,
worldspace, sync = true)`, which waits on `QuadTreeWorld::preload` building every chunk of the
grid's view. The `off` legs read a p99 of 11 to 13 ms on that row with no paging in it, and a part of
that is ground this renderer stops asking for.

**And the composite is per chunk**, so a 512² map covers one cell at one cell out and sixteen cells
at four — 32 units a texel at the far reach, over ground that is nine vertices across.

What the storage reads is already a cell: `Terrain::Storage::fillVertexBuffers(lod, size, centre)`
and `getBlendmaps(size, centre)` are both documented as callable from any thread and both take a
size of one cell. `ESMTerrain::Storage` reads them out of the land manager's cache, which is what
`CellPreloader` reads from its own thread.

## The design

**The cell ring stands the ground as it stands the statics**: one cell at a time, read on the
ring's thread, adopted by the frame one cell a walk, placed inside the reach and dropped outside the
prepared band. `Rtx::StaticRing` becomes `Rtx::CellRing`, because what it stands is the cells and
not only their statics.

- **One mesh per cell at full detail**: the 65 × 65 heights `fillVertexBuffers` fills at level
  nought, the normals and the same triangulation `Terrain::BufferCache` builds — the diamond
  pattern, with no stitching flags because every cell is at one level. Local to the cell's centre,
  which is how the storage fills them, and one instance translated to it. Adjacent cells share their
  edge row by construction, so there is no seam to stitch and no chunk is rebuilt when a neighbour
  changes: a cell's mesh is a function of the content and nothing else. 8192 triangles a cell, 121
  cells prepared at the default reach. No fold: a heightfield is not a sheet and has no reversed
  twin, so `FoldedShape` is stated rather than searched for.
- **Layers per cell from `getBlendmaps`**: one `MaterialLayer` per ground texture the cell names,
  its 34 × 34 mask read as `readMask` reads a blend map today, and the two transforms derived from
  what `Terrain::createPasses` attaches — `LayerTexMat` is a scale by the tile count, and
  `BlendmapTexMat` is `T(0.5) · S(n / (n + 1)) · T(-0.5) · T(1 / 4n, -1 / 4n)` for a tile count
  `n`, which at sixteen is a scale of 16 / 17 and an offset of (0.75 / 17, 0.25 / 17). Both classes
  are private to `material.cpp`, so the formula is derived here and the numbers are asserted by a
  test.
- **A cell with no land record is a flat quad** at the default height, wearing the default ground
  texture `getBlendmaps` names for it: two triangles rather than 8192 of a plane. The quad tree
  stands nothing there beyond the active grid and the grid's own fallback stands the plane; a ray
  tracer wants a sea bed under every cell of the reach, and two triangles is what a constant costs.
- **Flattened outside the active grid**, as `mFlatten` on the material, and rewritten with
  `setMaterial` as the grid moves over the cell — a cell entering the grid shades from its stack and
  gives its composite back, a cell leaving asks for one. The queue that bakes them is unchanged: it
  reads `mFlatten` off the rows the walk wrote, and a cell's material is a row like a chunk's. The
  rule was "a chunk at least a cell wide", which the quad tree reached at about a cell out; this is
  the same rule stated on cells.
- **The images are read on the thread**, by path through the content source the templates come from
  — `TemplateSource` becomes `ContentSource` and hands out images as well — so the layer textures
  reach the frame's describe as readings, as a model's do. A reading is lent to every cell whose
  ground names it and given back through the ring when the frame drops the cell, which is the
  model's protocol applied to the ground.
- **The rows are the ring's own.** A cell's ground has no drawable and no state set, so nothing in
  the graph will ever meet either and the identity maps have no key for them. The extractor gains
  `keepOwnedMesh`, `keepOwnedMaterial` and `disownRows`: a residency names the rows it owns on every
  walk, they join the survivor lists the sweep hands `SceneDesc::release`, and a residency that let
  one go says so, so that the sweep after the walk runs the release even where the maps stand whole.
  The placement is the ring's own slot, as a static's is.
- **The game builds no ground for this renderer.** `Renderer::wantsTerrainChunks()` answers no on
  `RtxRenderer`, and `RenderingManager::getWorldspaceChunkMgr` gives it a bare `Terrain::World` —
  the storage, a root node and nothing under it — so `preload`, `cacheCell` and `loadCell` build
  nothing and `update`'s synchronous terrain wait is over before it starts. `wantsPagedTerrain`
  stays true, because the map window reads the terrain's reach through it and this renderer's ground
  does reach that far.

**What goes.** `TerrainResidency` and its warming thread, `ChunkRuns` and the replay,
`MirrorTraversal::takeChunk`, `MaterialResolver::resolveTerrain`, the `Terrain` branch of
`mirrorDrawable`, `sCompositeFrom`, the `warm` timing row, `ExtractionStats::mComposites` and
`mUndescribedGround`, and their tests. `Collector` stops being a `Terrain::ChunkTaker`. What is
left of the fork's additions to `components/terrain/` — `ChunkTaker`, `Vantage`, `World::collect`,
`isEnabled`, `getTerrainRoot`, `getActiveGrid`, `QuadTreeWorld::collect` and `handOver` — is then
read by nobody, and is named below as a change to upstream files to wait on.

**What does not change.** The composite bake, its queue, its per-frame bound and its settled rule;
the shader's terrain path, which reads layers, masks and transforms off the same rows; the statics;
the sea.

## What it costs

**Triangles.** 121 cells at 8192 is 991k triangles in bottom-level structures, against the quad
tree's few tens of thousands. A ray tracer traverses that in log time and a structure per cell is
built once, on the frame the cell is adopted a band out of sight; the `blas` zone and the structure
size are readings to take, not a reason to build less. The picture question the loading plan
deferred — full-detail ground everywhere — is answered by measuring `trace` at the shore and on the
crossing, ring against quad tree.

**Memory.** A cell's geometry is 233 KB in the shared buffers (4225 vertices with normals and
texture coordinates, 24576 indices): 28 MB at 121 cells. Its masks are about 40 KB. Its composite
is what a chunk's was, 1.33 MiB, and there is one per cell outside the active grid: 112 at the
default reach, about 150 MiB in the texture array against about 40 for the quad tree's chunks. That
is the price of one composite per cell at one extent, and it is bounded by the reach.

**The bake.** 27 ms a composite on the queue's thread; a teleport queues 112 of them, three
seconds, during which the distant cells shade from their stacks — the picture the near cells always
have — and a crossing queues eleven. A settled run waits for them, as it does today. If the reading
says the queue falls behind a crossing, the levers are an extent by band and a second baker; neither
is built on a guess.

**The picture changes, once.** Distant ground goes from nine vertices a chunk to sixty-five a cell,
and its composite from one per chunk to one per cell. `verify` moves at every exterior view and is
read once as the new reference.

**Vertex colours are outside this change and were never read.** `fillVertexBuffers` fills the land
record's colours and the rasterizer multiplies its ground by them; `MeshReading` carries no colour
and no scene table holds one. Logged in `.notes/ISSUES.md`.

## Measurements

1. **The crossing**, `bench --views=island-crossing --seconds=20 --settled=false`, three pairs
   interleaved against the 06:30 batch: the `fold` row, which the design says goes to nought; the
   `update` row, which the seam change says falls; `place` and `upload` with a cell's geometry in
   them; the tail.
2. **The shore and the ship**, standing: `trace`, `blas`, structures, textures — what full-detail
   ground costs a frame that only looks at it.
3. **`verify --exposure=1`** at every exterior view, read once as the reference.

## Stages

Each stage builds and passes the gate of `loading-plan.md`.

### Stage 5a — the ground on the ring

`Rtx::GroundReader` and `PreparedGround`, `ContentSource`, `CellReader` reading both halves of a
cell, the extractor's owned rows, `CellRing` adopting, flattening, placing and stamping the ground,
the deletions above, `WorldMirror` wired without `TerrainResidency`. Tests: the reader against
`Terrain::BufferCache`'s own index buffer and hand-computed transforms; the ring standing one ground
placement per cell of the reach, flattening by the grid, dropping by the band, at zero allocations
on a steady walk; the extractor keeping and releasing owned rows.

### Stage 5b — the game builds no ground

Three lines in two upstream files, named here and waited on: `RenderingManager::getWorldspaceChunkMgr`
makes a bare `Terrain::World` where `wantsTerrainChunks()` answers no, and `Terrain::World::
setTargetFrameRate` and `setBordersVisible` guard the members that constructor leaves null. Then the
dead seam in `components/terrain/` is deleted. Gate: the `update` row on the crossing.
