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
  `OPENMW_RTX_BENCH=10s:2s@12000` on a save is the game's own crossing measurement. **It prints the
  harness's rows** — frame, wait, walk, place, and crossings with the rebuilds among them — off the
  same `Rtx::FrameSamples`, so a row of one can be read against a row of the other.
- **`scene hand`, `scene extend` and `scene build` in the log** (verbose) are what an arrival costs
  the frame it lands on, and between them they name every millisecond of the `place ms` row: the
  composites taken, the textures described, then the renderer's own draining, describing, building
  and placing, and inside the build the classification against the structures. Nothing else can see it — the device's zones cover what a crossing records, not the CPU
  that recorded it.
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
- **What a crossing is made of, in the game** (A5, Seyda Neen at 12,000 u/s, four runs of ~1000
  frames): 16–18 crossings apiece and **none of them a rebuild**, `wait` nought throughout — at a
  window this size the game is CPU-bound, so every millisecond of walk or place is a millisecond of
  frame. On an otherwise quiet machine the frame median is 7–8 ms and the **worst crossing 233–275**,
  of which `walk` and `place` are 223 and 239 — so **the crossing spike is this fork's own work**.
  The harness's synchronous reader was never the explanation, and the game's threaded preloading
  does not shorten the frame.
- **Take these with the editor closed.** Two runs taken with `rust-analyzer` and `clangd` busy
  tripled the median to 17 ms and put the worst crossing at 751 and 879 — and there walk and place
  were barely a third of it, the rest being the engine contending for cores. `place` worst held at
  127–154 across all four, which is the figure to trust.
- **And the placement is one call, split all the way down.** The worst crossing measured:
  `scene hand` 0.3 ms on composites, 7.3 describing 24 textures, **124.4 in the renderer**;
  `scene extend` 0.2 draining, 6.9 describing, **115.3 building**, 1.9 placing; and of that build,
  **104.7 ms is classifying opacity micromaps** for 443 arriving meshes against 2.2 ms of bottom
  level structures and 2.3 ms of `AlphaBounds`. A quarter of a millisecond a mesh, all on one frame.
  This is the figure the whole lane rests on.
- **The micromaps are worth 1.3% of the trace and cost 105 ms of a crossing frame.** Measured with
  the classification patched out, over the exteriors and interiors suites — thirteen places, paired:
  the trace zones sum to **23.46 ms without against 23.16 with**, and the worst single place moves
  0.17 ms. Eleven of the thirteen deltas are positive, so the effect is real and it is small. The
  tally says why: at seyda-neen-ship 3,153 microtriangles come back opaque and 3,078 transparent
  against **94,140 unknown**, so 94% of what the classifier looked at still asks the alpha test.
- **Neither knob binds.** `sTexelsPerMicrotriangle` 16 → 64 took the tally from 6.2% to 4.5% and the
  trace nowhere; `sSubdivisionCeiling` 5 → 4 took it to 5.6% and nothing else moved either. So the
  work is spread thinly over many triangles at low levels rather than clamped at the ceiling, and
  there is no constant to turn.
- **A trap: the first `scene hand` of a run is not a micromap measurement.** Its "in the renderer"
  figure is the startup `setScene`, and that is the texture array — 567 textures at Seyda Neen.
  Removing the micromap classification entirely leaves it at 254–261 ms against a baseline of
  253–263. It is beautifully repeatable and it measures the wrong thing. `scene build` is the line
  that isolates the classification, and bounding the opportunity — patching the term out and running
  the same measurement — is what should settle whether a metric points at it at all.
- **A hierarchical classifier does not help — measured, and it costs 28% more.** `AlphaBounds`
  answers for a whole box and a microtriangle's box lies inside its parent's, so a coarse-to-fine
  sweep that only asks about what its parent left open looks free. It is not: counted over a run,
  it made **92.2 million box tests against the flat walk's 72.1 million**, because a coarse
  microtriangle's box is wide enough to straddle the cutoff nearly always and almost nothing
  inherits. The intermediate levels are then pure overhead, and 129% is within a whisker of the
  arithmetic worst case of 133%. Counted rather than timed, so the result does not depend on what
  else the machine was doing.
- **Draining is not free either.** `finishFrames` cost 23.0 and 18.0 ms on two of the eighteen
  crossings, against 3.3 on the worst — a frame in flight the arrival had to wait out.

## 3 Attack plan

Ordered by what a player feels, then by milliseconds. Every step carries its own measurement gate:
the paired-run rule of §1, at the place named. What a step trades away is written beside it.

### Lane A — the crossing spike (worst 233 ms in the game; target: no crossing frame over ~25 ms)

Most of this lane is closed by measurement rather than by code. The frame thread neither builds
terrain chunks nor is outrun by the thread that does; the harness's synchronous reader is not what
made a crossing long, since the game preloads on threads and drops the same frame; and the classifier
that costs the frame cannot be made cheaper in place — a hierarchy over it was counted and lost, and
neither of its constants binds. §2 carries every number. **What is left is one call — the arrival
handed to the device. Nine tenths of what that call *builds* is classifying opacity micromaps, and
they are worth 1.3% of the trace.**

- **A2. Take the micromap classification off the frame.** 105 of the 115 ms worst build is it, over
  443 arriving meshes — and §2 prices what it buys at 1.3% of the trace. **A term worth 1.3% has no
  business costing a fifth of a second on the frame a player crosses a cell on**, and the preamble
  above says why no amount of making it faster settles that. What is left is when it runs. Two
  shapes, and the second is the tree's own precedent:
  (a) classify a bounded number a frame; (b) a thread of its own, the way `CompositeQueue` moved the
  composite bake, with the frame collecting what is finished.
  **Neither is free.** A bottom-level structure names its micromap when it is *built*, so a mesh
  that arrives without one and gets it later has to be built again — 2.2 ms for 443 meshes, cheap,
  but it is a second build and it has to be bounded too. Quality cost: none — a mesh tracing without
  a micromap draws the same picture and only asks the alpha test more often, and §2 prices the whole
  suite's worth of that at 1.3% of the trace. Gate: `scene build` and `place ms` worst in the game,
  with the suite trace held. **The unit to bound is meshes, not bytes** — the texture uploads beside
  them are 7 ms.
- **A7. Do not wait out a frame to place an arrival.** `finishFrames` cost 23.0 and 18.0 ms on two
  of eighteen crossings. It is there because an arrival writes every copy of the tables; a copy the
  arrival could write into instead would cost the wait nothing. Gate: the draining figure in
  `scene extend`.
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

A2 first — A5 says the crossing spike is this fork's, A2 is most of it, and §2 has settled what the
term it moves is worth. Then B1+B2 (cheap, measured, ~1.5 ms at the budget hour), then B3
(the interior's 7.7 is the worst median in the table). D1/D2 whenever touching that file. E after
B, starting at E0. A6 and C parked.
