# RTX performance — method, numbers, attack plan

Measured 2026-08-31 at `69a19dcf7e`, on the RTX 4090 Laptop (software power cap — see §1 on
variance); the streaming rows of §2 were taken again the same day, after the bake was cheapened. The
budget from `plan.md` §5: 3840×2160 out of 1920×1080 traced at 60 fps, which is 16.6 ms a frame with
`--upscale=performance`.

## 1 Method

- `openmw-rtxtool bench --window=false` is the number: frame/wait/walk/place percentiles and the
  GPU zone table per place. `--size=3840x2160 --upscale=performance` is the budget configuration;
  a plain run traces quarter the pixels and answers a different question.
- `apps/rtxtool/bench.sh` is the same measurement inside the game (prepends to `.notes/bench.txt`);
  `OPENMW_RTX_BENCH=10s:2s@12000` on a save is the game's own crossing measurement.
- `apps/rtxtool/profile.sh --view=<v>` is the CPU explanation (perf, measured frames only).
  Reports land in `build-release/perf*/`.
- **`composite bake` in the log** (verbose) is what a chunk's flattening costs, by layer count and
  mask extent. It happens on the queue's own thread and no frame timer reaches it, so without this
  line a profile can only say what share of a run it was.
- `ncu` is not installed, so a pass's inside is costed by removing the term and re-running —
  `shot --repeat` for a still, `bench` for a place. The A/B below that needed no edit was run by
  hour: dusk 17:45 has the sun as low as dawn 6:30 and no moons up.
- **The GPU clock is not a constant.** Under the power cap the core ran 1740–2190 MHz across the
  runs in §2, which is a ±15% spread on any single number. An A/B is two *paired* runs back to
  back, the printed clock line is part of the result, and a difference under ~10% on one pair is
  noise. Medians over 600 frames; p99 carries thermal ramp as well as content.

## 2 Numbers

Budget configuration (3840×2160 out, performance upscale, preset d), 600 frames per place:

| place                | median | p99   | trace | upscale | rest  | verdict            |
| -------------------- | ------ | ----- | ----- | ------- | ----- | ------------------ |
| seyda-neen-ship      | 13.1   | 18.7  | 4.9   | 4.9     | ~2.0  | tail over budget   |
| seyda-neen-ship-dawn | 16.4   | 26.9  | 8.1   | 4.8     | ~2.3  | over budget        |
| balmora-mages-guild  | 16.8   | 21.1  | 7.7   | 5.8     | ~1.5  | over budget        |
| island-crossing      | 11.8   | 109.4 | 2.4   | 4.9     | ~2.3  | crossing spikes    |

- The frame is GPU-bound everywhere: `wait` runs 10–15 ms at the budget configuration. Trace and
  upscale are over 80% of the GPU frame; everything else together — air, tlas, refit, waves,
  bloom, tone, exposure, composite — is ~2 ms.
- **Crossings**: island-crossing worst frame 206 ms, 1% low 9.1 fps. 19 crossings, worst 198 ms;
  1.6 s over the run, 1.0 reading and 0.6 building. `walk` p99 25.5 / worst 35.3; `place` p99
  12.3 / worst 47.3.
- **Moons**: dawn trace 8.10 against dusk 6.55 on a paired pair — the two moon shadow rays (and
  the moons variant's 128-register occupancy) cost **~1.6 ms** at the hour the budget is written
  against.
- **Upscale**: preset e = preset d within noise (~5.0 ms at 4K out). The in-tree denoiser costs
  2.3–3.0 ms at 1080p *without* producing 4K, so atrous + a separate SR would land at the same
  price as Ray Reconstruction. RR stays; there is no cheap seat in this row.
- **In the game** (1994×1366 window, quality upscale): 6.1–8.7 ms medians, wait ~4.6 —
  GPU-bound at game settings too; the game's own CPU (~3 ms) currently hides under the GPU.
- **CPU steady state** (harness, seyda-neen-ship): walk 2.5–3.6 ms. Of on-CPU time: the mirror
  walk ~50% (MirrorTraversal + `addDrawable` 7% self + `resolveMesh` 4.5% self + ~8% in hash-table
  lookups), `RigGeometry::cull` (skinning) 10%, terrain `collect` 10%, `placeScene` 8%,
  `retire` 2.3%, `__dynamic_cast` 3.3%.
- **CPU streaming** (island-crossing, re-measured 2026-08-31): the composite baker thread was
  **53% of on-CPU** over the run and cost **52.8 ms a chunk**, over three runs of 215-odd chunks
  each — nine ground types apiece against a mask 34 across. Walking the stack a layer and a row at a
  time rather than a texel at a time took that to **27.1 ms and 41%**, and the worst chunk from
  ~115 ms to ~62, with the picture bit-identical over all 17 `verify` views. Arriving meshes are
  canonicalised in the walk (`SheetFold::fold` 4.8%).
- **`collect` does not build chunks, and never did on this route.** `loadRenderingNode` is 9.6% of
  on-CPU and 9.1 of that sits under `QuadTreeWorld::preload` — the warming thread
  `Rtx::TerrainResidency` starts. What `Terrain::QuadTreeWorld::collect` spends (16% of on-CPU, and
  most of `walk ms`) is the **mirror walking the chunk geometry it hands over**: 12.8 of its 13.3
  points are under `MirrorTraversal`. That is Lane E's problem, not Lane A's, and it is where the
  steady-state walk goes on an exterior.
- **What a crossing is made of**: 19 of them, 1.8 s over the run — **1.1 s reading the cell and
  0.7 s building from it**. The read is the harness's own synchronous ESM reader, which the game
  does on preload threads instead, so more than half of the 220 ms worst frame is not the
  renderer's. A5 is what settles that split.

## 3 Attack plan

Ordered by what a player feels, then by milliseconds. Every step carries its own measurement gate:
the paired-run rule of §1, at the place named. What a step trades away is written beside it.

### Lane A — the crossing spike (worst 220 ms; target: no crossing frame over ~25 ms)

The frame thread neither builds terrain chunks nor is outrun by the thread that does — §2 says so
with numbers, and the two plan items that assumed otherwise are gone. What is left of a crossing is
the cell being read and the geometry being handed to the device.

- **A5. Measure the same crossing in the game** (`OPENMW_RTX_BENCH=10s:2s@12000` on an exterior
  save). The harness reads a cell synchronously and the game preloads on threads, and the harness's
  read is 1.1 s of the 1.8 the crossings cost — so the split between "the renderer" and "the
  harness's own reader" has to be taken in the game before anything else here is tuned.
- **A2. Bound what one frame places.** An arrival's BLAS builds and texture uploads ride the
  placement submit in one burst (`place` worst 48 ms). Extend the composite queue's discipline
  (2 composites a frame) to arrivals: a per-frame byte budget on the deferred batches, the rest
  waiting a frame. Quality cost: an arriving cell's pieces appear over ~2–5 frames instead of one.
  Gate: `place` p99/worst on island-crossing.
- **A6. Cheapen the bake further, if the latency still shows.** 27 ms a chunk is one thread busy
  for most of a crossing, and what is left is inherent — a quarter of a million texels each summing
  the ground types that reach them. The two terms with a shape left to change are `paintedLight`
  (7.0% of on-CPU, and the only tap in the loop whose V axis is still resolved per texel because
  `contactsheet` shares the function) and `buildChain` (9.4%, an sRGB encode a million times over).
  Gate: `composite bake` in the log.

### Lane B — the trace kernel (dawn 8.1, interior 7.7; target ~6 at both)

- **B1. One moon ray, not two.** Sample one of the two moons by irradiance weight per gather and
  divide by the pick probability. Saves ~0.8 ms whenever both moons are up; unbiased. Quality
  cost: the penumbra under crossed moonlight resolves at one sample a frame instead of two — a
  noise trade the filter and RR already carry for lamps. Gate: dawn trace, paired.
- **B2. No moon rays at the bounce depth.** The bounce-hit gather traces sun + two moons + a lamp
  ray for a term that is one bounce down and dim. Dropping the moons there loses per-moon shadows
  in *indirect* moonlight only. With B1 this recovers most of the measured 1.6 ms. Gate: dawn
  trace + a night `verify` still against the previous build.
- **B3. Size the interior's lamp pressure.** Interior trace (7.7 ms) is the bounce plus two
  reservoir walks per pixel (up to 256 lamps each) plus their shadow rays. First measure, then
  trade: (a) trace vs lamp count across the interiors suite; (b) a tighter light-grid cell so a
  point weighs fewer candidates; (c) an A/B that skips the depth-1 lamp *shadow ray* (keeps the
  reservoir, takes the unshadowed estimate) — biases indirect slightly bright in lamp-dense rooms.
  Gate: balmora-mages-guild and the interiors suite.
- **B4. `skyReaching` at half rate.** One binary ray per bounce hit, exteriors only; checkerboard
  it and let the filter carry the other half. Small (~0.2–0.4 ms expected); measure before
  keeping. Gate: noon exterior trace.
- **B5. Occupancy of the fat variants.** The sea/moons variants sit at 128 registers against 96.
  Measure a split of the kernel (or a register diet on the sea path) before believing in it; ray
  queries in compute cannot use SER, so thread coherence comes only from shape.

### Lane C — the upscale (4.7–5.8 ms): parked

Ray Reconstruction is the right price against the alternative (measured in §2), preset e buys
nothing, and the cost scales with the 4K output, not with what we trace. Re-measure on each DLSS
SDK update and otherwise leave it alone.

### Lane D — frame hygiene (merged from `review.md`)

- **D1. A frame that wrote any GUI texture pays a submit-and-wait inside the frame.**
  `GuiTextures::getView`/`read` call `flush()` → `CommandPool::endAndWait`, and
  `VulkanRenderer::drawGui` calls `getView` per batch — a playing video pays the wait every
  frame, the fog of war pays it as the player walks, a window opening pays it for its atlas.
  `Batch::defer()` (carried by the next frame's own submit, no wait) exists and nothing on this
  path uses it. Gate: in-game bench while a video plays / while walking with the map up.
- **D2. A video frame crosses host memory three times under the Vulkan backend.** Decoder image →
  `Picture::set` lock-and-copy → staging copy → device. `GuiRenderManager::shareTexture` exists
  for exactly this caller and the backend's manager returns null
  (`components/myguirtx/rendermanager.hpp:51`). Fixing D1 removes the stall; fixing this removes
  two of the three copies.

### Lane E — the steady-state CPU walk (2.5–3.6 ms a frame, every frame)

Hidden under the GPU today; it is the 1% low and the power bill, and it grows with the world.
- **E0. The terrain chunks are most of the walk on an exterior.** `collect` is 16% of on-CPU and
  nearly all of it is `MirrorTraversal` over the merged chunk graphs `ObjectPaging` hands back — a
  paged chunk is thousands of drawables that never move. Whatever E1 and E2 buy on a drawable, they
  buy it here most of all. Gate: `collect`'s share in the profile, island-crossing.
- **E1. The per-drawable hash lookups** (~12% self in `addDrawable`/`resolveMesh` + ~8% in
  `hashtable*`): a slot handle cached on the drawable (epoch-stamped, the placements are already
  slot-addressed) turns a map probe per drawable per frame into a pointer read.
- **E2. `dynamic_cast` (3.3%)**: the `className()`-gate pattern `posecull.hpp` already uses,
  applied to the remaining casts in the walk.
- **E3. The long lever: a walk that skips what did not change.** The mirror is deliberately
  stateless per frame (mark-and-sweep is what makes retirement sound); an incremental walk is a
  design change, not a tweak — it goes after B, with its own note.

### Order of attack

A5 first: it is the only thing that says how much of the crossing spike this fork owns, and A2 is
guesswork until it is answered. Then B1+B2 (cheap, measured, ~1.5 ms at the budget hour), then B3
(the interior's 7.7 is the worst median in the table). D1/D2 whenever touching that file. E after
B, starting at E0. A6 and C parked.
