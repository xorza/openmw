# The device side of the RTX frame

What the card spends a frame on, and what that says about the budget. Taken at `4f2b88db9d` with a
clean working tree, on an RTX 4090 Laptop and an i9-13980HX, NVIDIA 610.57.04. The build is
`apps/rtxtool/release.sh`.

Every reading is `bench --validation=false --window=false --seconds=20`, which is 1200 measured
frames after 180 warm-up frames at each place. The card was warm before the first suite, and no run
waited for it to cool. Unless a row says otherwise, the output is 1920×1080 and the upscaler is at
quality, which traces 1280×720.

**The numbers are the device's own.** `bench` closes a timestamp query around each pass and prints
what the card reported, so a zone is what the card took and not what the host waited. Every place
that stands still waits on the device, so these zones are the frame's budget.

## What each zone is

| zone | what it is |
|---|---|
| `upscale` | DLSS Ray Reconstruction, between the trace and the picture |
| `trace` | the ray-tracing pipeline: the picture |
| `air` | the fog volume's depth ray a column, and the scatter through its froxels |
| `column` | the same volume integrated along each column |
| `waves` | the sea's spectrum, synthesised once a frame for every ray |
| `bloom` | what the lens spreads |
| `composite` | where the bounce ended up, gathered for the upscaler |
| `tone`, `exposure` | the curve, and the measurement it is driven by |
| `shade`, `sprites` | the emitters' lighting and their screen-space bin |
| `tlas` | the top level, rebuilt every frame |
| `refit`, `skin` | the bottom levels a pose moved, and the pose itself |
| `blas`, `compact` | what a cell arriving builds. A `micromap` zone stood beside them until the bake was removed — Finding 4 |

## Where it stands

Milliseconds, medians, at 1920×1080 out and 1280×720 traced. `gpu` is the sum of the zones.

| view | frame | wait | gpu | upscale | trace | air | column | waves | tlas | refit | bloom | rest |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| vivec | 6.84 | 3.53 | 5.78 | 2.27 | 1.97 | 0.16 | 0.08 | 0.21 | 0.23 | 0.33 | 0.12 | 0.41 |
| seyda-neen-ship-overcast | 5.87 | 3.83 | 5.16 | 2.35 | 1.56 | 0.23 | 0.10 | 0.22 | 0.24 | 0.13 | 0.13 | 0.20 |
| seyda-neen-ship | 5.81 | 3.79 | 5.07 | 2.31 | 1.52 | 0.23 | 0.10 | 0.22 | 0.24 | 0.13 | 0.13 | 0.19 |
| balmora-storm-night | 5.63 | 3.35 | 4.91 | 2.42 | 1.24 | 0.16 | 0.10 | 0.22 | 0.26 | 0.16 | 0.13 | 0.22 |
| seyda-neen-shore | 5.58 | 4.04 | 5.13 | 2.45 | 1.50 | 0.25 | 0.10 | 0.22 | 0.26 | 0.06 | 0.13 | 0.16 |
| balmora | 5.42 | 3.37 | 4.76 | 2.37 | 1.20 | 0.17 | 0.09 | 0.22 | 0.26 | 0.15 | 0.13 | 0.17 |
| ald-ruhn | 5.41 | 3.16 | 4.69 | 2.36 | 1.01 | 0.20 | 0.11 | 0.22 | 0.24 | 0.18 | 0.13 | 0.24 |
| balmora-fog-night | 5.40 | 3.15 | 4.61 | 2.39 | 1.04 | 0.16 | 0.09 | 0.22 | 0.25 | 0.16 | 0.12 | 0.18 |
| dagon-fel | 5.27 | 4.06 | 4.83 | 2.48 | 1.20 | 0.17 | 0.10 | 0.23 | 0.24 | 0.08 | 0.13 | 0.20 |
| sadrith-mora | 5.25 | 3.19 | 4.54 | 2.40 | 0.86 | 0.19 | 0.11 | 0.22 | 0.26 | 0.18 | 0.13 | 0.19 |
| addamasartus | 5.15 | 4.06 | 4.71 | 2.52 | 1.22 | 0.10 | 0.07 | 0.23 | 0.18 | 0.09 | 0.13 | 0.17 |
| balmora-mages-guild | 5.01 | 3.92 | 4.54 | 2.55 | 1.25 | 0.10 | 0.06 | 0.00 | 0.20 | 0.10 | 0.13 | 0.15 |
| seyda-neen-customs | 4.62 | 3.64 | 4.22 | 2.56 | 0.95 | 0.09 | 0.06 | 0.00 | 0.18 | 0.06 | 0.13 | 0.19 |
| andrano-tomb | 4.55 | 3.33 | 4.10 | 2.57 | 0.82 | 0.10 | 0.06 | 0.00 | 0.17 | 0.08 | 0.13 | 0.17 |
| vivec-canalworks | 4.53 | 3.63 | 4.10 | 2.45 | 0.90 | 0.11 | 0.09 | 0.00 | 0.15 | 0.05 | 0.11 | 0.24 |
| arkngthand | 4.13 | 3.29 | 3.74 | 2.57 | 0.41 | 0.13 | 0.10 | 0.00 | 0.18 | 0.05 | 0.13 | 0.17 |
| island-crossing | 5.37 | 3.48 | 5.74 | 2.34 | 0.83 | 0.20 | 0.09 | 0.21 | 0.21 | 0.08 | 0.12 | 1.66 |

The crossing's `rest` is its arrivals: a `micromap` bake of 0.81 ms a frame and a `blas` build of
0.69, spread over the frames a ring lands on. **The bake has since been removed** — Finding 4 — so
that row's `rest` is now 0.69 and its p95 is six milliseconds lower.

**The upscaler is the largest zone at every one of the seventeen.** It is 39 per cent of the device
frame at Vivec, 46 at Seyda Neen and 69 in the Dwemer ruin. It is also the flattest: 2.27 to 2.57 ms
whatever is in front of the camera, because it is a function of the extent and not of the scene.

## The one law: both costs are linear in the pixels traced

The upscaler's ladder at `seyda-neen-ship`, output 1920×1080 throughout. `off` is the fork's own
wavelet instead of Ray Reconstruction, which traces the whole output.

| mode | traced | Mpx | trace | upscale | frame | trace ns/px | upscale ns/px |
|---|---|---:|---:|---:|---:|---:|---:|
| ultraperformance | 640×360 | 0.230 | 0.42 | 0.80 | 2.90 | 1.82 | 3.47 |
| performance | 960×540 | 0.518 | 0.84 | 1.29 | 3.96 | 1.62 | 2.49 |
| balanced | 1114×626 | 0.697 | 1.14 | 1.77 | 4.80 | 1.64 | 2.54 |
| quality | 1280×720 | 0.922 | 1.54 | 2.33 | 5.82 | 1.67 | 2.53 |
| dlaa | 1920×1080 | 2.074 | 3.44 | 5.38 | 11.22 | 1.66 | 2.59 |
| off, wavelet | 1920×1080 | 2.074 | 3.04 | 1.99 | 7.32 | 1.47 | 0.96 |

**The trace costs 1.7 nanoseconds a pixel and Ray Reconstruction costs 2.5, and both hold across a
nine-fold range of extents.** So the budget is arithmetic: pick an extent and the device frame
follows. The two 4K rows below were taken separately and land on the same two constants, which is
what says the law is the hardware's rather than one scene's.

**And the denoiser costs half again what the ray tracing costs.** That ratio is 1.5 at every mode
but the smallest, where a fixed cost inside the upscaler still shows.

**The fork's own wavelet is a third of the price per pixel** — 0.96 against 2.53 — and it is not
what the fork uses, because Ray Reconstruction is the picture the fork is for. What the table says
is the size of that decision, not that it is wrong.

## Finding 1 — the stated target is met, and the denoiser is half of it

`AGENTS.md` states the target as 1920×1080 internal to 3840×2160 at 60 fps. That is
`--size=3840x2160 --upscale=performance`, which traces exactly 1920×1080.

| view | frame | gpu | upscale | trace | fps | 1% low |
|---|---:|---:|---:|---:|---:|---:|
| balmora-mages-guild | 11.04 | 11.15 | 5.98 | 3.13 | 90.6 | 56.7 |
| seyda-neen-ship | 11.96 | 11.39 | 5.31 | 3.47 | 83.6 | 65.4 |
| vivec | 13.38 | 12.52 | 5.26 | 4.53 | 74.8 | 58.9 |

**The target is met at the median with three to six milliseconds to spare, and the one per cent low
sits on the line** — 56.7 to 65.4 fps against the 60 the target names.

Ray Reconstruction is 5.26 to 5.98 ms of that, which is 45 to 54 per cent of the device frame and
between 1.2 and 1.9 times the trace.

**Quality upscaling to 4K does not reach the target.** It traces 2560×1440 and reads 45.7 to 55.3
fps, with the upscaler alone at 9.48 to 10.29 ms. The mode is the budget.

## Finding 2 — the flat costs are a fifth of an interior's frame

Six zones barely move with the scene: `waves` 0.21–0.23 wherever the cell has water, `tlas`
0.15–0.26, `bloom` 0.11–0.13, `composite` 0.03–0.05, `tone` 0.04–0.06 and `exposure` 0.02–0.04.

Together they are 0.70 ms of an exterior frame and 0.42 of the Dwemer ruin's — 14 per cent at Seyda
Neen and 11 in the ruin, where the trace itself is 0.41 ms.

**`waves` is the one to look at first.** It is a spectrum synthesised once a frame for a sea that a
cell may show a sliver of or none at all, and it costs the same 0.22 ms either way. It is skipped
where a cell holds no water, which is what makes the interiors read nought — so the question is not
whether it can be skipped but whether it has to run on a frame where the sea moved by nothing a
pixel could show.

**And `tlas` is rebuilt every frame.** 0.24 ms is five per cent of an exterior's device frame. The
code says a rebuild costs an arrival nothing because it happens regardless; what it does not say is
what a refit would cost on the frames where only transforms moved.

## Finding 3 — the reorder loses everywhere, and it changes the picture

**Two separate things wear the name SER here, and only one of them is optional.** The trace is a ray
generation shader that launches through hit objects — `hitObjectTraceRayEXT` then
`hitObjectExecuteShaderEXT` — whatever mode is asked for, so `VK_EXT_ray_tracing_invocation_reorder`
is required for the launch and not for the sort. `Rtx::Reorder` records the launch as worth 6 to 10
per cent against the dispatch it replaced. That reading is inherited, not retaken: there is no
dispatch path left to measure it against.

**What is optional is the sort**, and it is `Reorder::Hit`, `Hint` and `Both`.

### It costs 17 to 23 per cent of the trace, at every place

Order-balanced — `off` first in rounds 1 and 3, `hit` first in 2 and 4 — four pairs of
`--seconds=20`, reading the `trace` zone.

| place | trace off | trace hit | hit − off | % | t | frame off | frame hit |
|---|---:|---:|---:|---:|---:|---:|---:|
| dagoth-ur-caldera | 2.453 | 3.015 | +0.562 | +22.9 | 29.1 | 6.47 | 7.01 |
| vivec | 1.887 | 2.305 | +0.417 | +22.1 | 23.2 | 6.72 | 7.10 |
| seyda-neen-ship | 1.948 | 2.312 | +0.365 | +18.7 | 56.5 | 6.31 | 6.60 |
| seyda-neen-shore | 1.677 | 1.970 | +0.293 | +17.4 | 16.3 | 5.79 | 6.06 |
| balmora-mages-guild | 1.478 | 1.750 | +0.273 | +18.4 | 24.6 | 5.23 | 5.50 |
| arkngthand | 0.140 | 0.200 | +0.060 | +42.9 | — | 3.85 | 3.91 |

**Every place, every mode, far outside the drift.** One round of the other two reads +19.1 to +24.6
per cent for `hint` and +17.8 to +28.0 for `both`, and the three are indistinguishable from one
another. `arkngthand` traces 0.14 ms and pays 0.06 of it, which is the call's fixed cost with almost
no divergence to recover — the sort's floor, showing on the smallest trace in the corpus.

**That is the third arrangement to lose.** `Rtx::Reorder` records one sort in front of a kernel
holding every kind at 7 to 17 per cent, the same sort in front of a shader per kind at 12 to 25, and
a reorder at the bounce at 20 to 30. This is the fourth reading and it agrees with all three.

### The cost is the call, and the sort adds nothing to it

Taken at `64d3d638b2`. A build whose `hint` mode asks `reorderThreadEXT(0u, 0)` — a reorder point
with no key at all — against `off`, then the real two-bit `hint` against `off`, three rounds each,
interleaved, at `seyda-neen-ship`. The `trace` zone, medians with the spread beside them.

| build | off | reorder | reorder − off | % |
|---|---:|---:|---:|---:|
| key of no bits | 1.67 (1.63–1.70) | 1.94 (1.90–2.03) | +0.27 | +16 |
| the real two-bit hint | 1.66 (1.66–1.70) | 1.95 (1.90–1.96) | +0.29 | +17 |

**A reorder with nothing to sort on costs what the sort costs.** The whole penalty is the reorder
point's fixed price — live state saved and restored, and the launch's own tiling given up in front
of eleven channel writes — and the sort adds nothing measurable on top of it.

### And there is almost nothing for the sort to recover

An instrument in the launch: `subgroupAllEqual` over the key the sort sees — the shader-table
record and the two flag bits — counted per warp before and after
`reorderThreadEXT(object, flags, 2)`, and printed through the hit counter as the share of pixels
whose warp already holds one key.

| place | before the sort | after |
|---|---:|---:|
| seyda-neen-ship | 88.8 | 99.8 |
| seyda-neen-shore | 94.2 | 99.9 |
| vivec | 98.6 | 99.8 |
| balmora-mages-guild | 100 | 100 |
| arkngthand | 100 | 100 |

**The launch is already 89 to 100 per cent coherent on the key before anyone sorts it.** That is
the whitepaper's own "primary rays" case, stated as a number: the sort can touch a tenth of the
warps at the ship and none in a room, and it moves them all to spend a fifth of the trace. What
diverges in this frame — the bounce, the lamp reservoir, the water's two traversals, the cutout loop
inside traversal — is not in the key and is out of a sort's reach.

**And `Reorder::Hint` carries no shader in its key at all.** The hint-only overload has no hit
object, so it sorts on the two flag bits alone; `lib/reorder.glsl` says the record's shader comes
first, which is true of `Hit` and `Both` and not of the mode the comment stands over.

### It does not draw the same picture, and the reason is the payload

A sort regroups threads; it must not change what they compute. It does, and the cause is now
found. `verify --exposure=1 --upscale=off --filter=false` over the twenty-two views, at
`64d3d638b2`:

| against `off` | views moved | worst | most pixels |
|---|---:|---:|---:|
| `hint` | 5 | 50 of 255 | 0.27 % at `balmora-mages-guild` |
| `both` | 5 | 54 | 0.27 % |
| `hit` | 5 | 54 | 0.27 % |

`hit` and `both` are bit-identical to each other, so the earlier reading that `hit` "differs by
more than its sort" was wrong. `hint` differs from both of them on the same five views. Every mode
is repeatable against itself and six runs of `off` agree, so none of it is noise.

**What moves is the see-through surfaces.** A build that paints every pixel whose eye ray lands on
a surface with opacity under one, read against the moved pixels of the five views:

| view | moved | of which see-through | within two pixels of one |
|---|---:|---:|---:|
| balmora-mages-guild | 5582 | 5507 | 5545 |
| ald-ruhn | 1665 | 1652 | 1654 |
| balmora | 5 | 4 | 4 |
| seyda-neen-shore | 7 | 7 | 7 |
| island-crossing-end | 3 | 3 | 3 |

The pane path is the one place a closest-hit shader reads what the launch wrote into the payload:
`mAsked`, which decides whether a pane is drawn as a pane or as the solid behind it, and which lamp
sequence the layer draws from. Under `--albedo` the guild's window panes come back as the solid's
albedo on 2.4 per cent of the frame, which is `ASK_BEHIND` read where the launch wrote nought.

**The payload the launch writes does not reach the closest-hit shader when a sort stands in the
shader.** Probes, all at `balmora-mages-guild`, `shot` unless said otherwise:

- A per-pixel, per-frame signature written into the payload after the trace and before the execute,
  checked by the closest-hit shader: wrong at 99.5 per cent of pixels under `hint`, `both` and `hit`,
  right at every pixel under `off`. The same signature written back by the closest-hit shader and
  checked by the launch is right under every mode — the return path is intact.
- The production field itself: a closest-hit shader reading `mAsked` off the eye's own ray, which
  the launch traces from `tmin` nought with `mAsked` nought, finds it nonzero at 99.5 per cent of
  pixels under `hint` and `both` in the pipeline `shot` builds, and at none in the one `verify`
  builds. So which pixels lose it is a property of the compiled variant and not of the sort.
- Two fields written side by side: `hint` delivers the first and loses the second, `both` the
  reverse, and a sort placed before the trace — with both fields written after it — loses both. A
  sort placed after the last channel write delivers both on the eye's ray and still moves the panes,
  so the peel's second execute loses one there.
- The launch's own reads off the hit object after the sort — the distance, the instance, the final
  opacity — are identical to `off` under every placement, so what the sort hands the launch is right
  and what the execute hands the closest-hit shader is not.
- Control: a reorder point with no key bits draws `off`'s picture exactly, and it still pays the
  fixed cost above. Any key of one bit or more — the flags, a pixel's parity, a constant — draws the
  moved one, and all three draw the same moved picture as each other.

The whitepaper's promise is that "reordering only affects performance, not correctness", and the
extension defines the fused calls as "equivalent" to the sequence. NVIDIA's own Vulkanised sample
writes the payload between `reorderThreadEXT` and `hitObjectExecuteShaderEXT`, which is what this
launch does. On driver 610.57.04 that write is what goes missing.

**The determinism gate cannot see it.** `repeatable.sh` passes under every mode, because it walks
`one-cell-walk` — which is one of the views that never moves — and forces `--upscale=off
--filter=false`. A gate that misses a picture change in the one switch whose whole promise is that
it changes nothing is a gap worth knowing about.

### What followed — the layer moved into the shader binding table, and the picture holds

The one word the launch wrote into the payload now rides in the hit record instead. Each closest-hit
shader stands behind `HIT_RECORD_LAYERS` records, one per layer of the peel; an instance's offset is
its kind times that, the launch adds the layer it traces for as the record offset, and the shader
reads `HitRecord::mLayer` through `shaderRecordEXT`. The payload flows outwards only. That is the
extension's own custom-indexing pattern, and the sort is allowed to read the record.

**`verify` over the twenty-two views: `off` is bit-identical to what it drew before, and `hint`,
`both` and `hit` are bit-identical to `off`.** The guild's albedo under `hint` matches `off` to the
pixel, `repeatable.sh` walks its 360 frames identical, and the 599 `Rtx*` tests pass. The switch is
an instrument again.

**Remeasured with the picture held, the sort still loses.** Three rounds each, interleaved
`off`, `hint`, `both`, `hit`, on a card at 72 °C under its power cap with the clock walking between
1.7 and 2.1 GHz — so the spread is wider than the morning's readings and the medians are what to
read.

| place | off | hint | both | hit |
|---|---:|---:|---:|---:|
| seyda-neen-ship | 1.62 (1.59–1.86) | 1.97 (1.90–2.09) | 2.00 (1.96–2.17) | 1.92 (1.88–2.05) |
| balmora-mages-guild | 1.36 (1.32–1.53) | 1.51 (1.45–1.53) | 1.43 (1.38–1.46) | 1.49 (1.38–1.51) |

A tenth to a quarter of the trace, in the shape every earlier reading had. The sort has nothing to
recover at the primary hit, and the fixed price of the call is what it costs. What would change the
answer is the bounce traced and sorted from the launch with the primary channels already written,
which is the arrangement every published win has and this frame does not.

**So the three modes are gone.** `Rtx::Reorder`, the `REORDER` specialization constant, the
`--reorder` option, the `reorder` setting, the device profile's flag and the sort's half of the
launch macro were removed on the readings above; the hit-object launch, the extension it needs and
the record per layer stay. The launch probe executes its one miss record now, in place of the
reorder that used to read it. What a future reorder at the bounce needs is all still here: hit
objects, a payload that flows one way, and a record the shader reads whatever stands in between.

## Finding 4 — the opacity micromap bake costs more than it buys, and it speckles distant foliage

**The measurement needs its order balanced.** An A/B that always runs `on` before `off` cannot
separate the setting from the card warming through the run. Taken that way over twelve places, a
tomb with sixteen cutouts read +4.4 per cent and a shore with 2628 read −1.0 — the noise was larger
than the effect and uncorrelated with the cutouts. Balanced instead, with `on` first in rounds 1 and
3 and `off` first in rounds 2 and 4, four pairs of `--seconds=20`, the effect separates. Two places
in the six carry no cutouts and are the null.

| place | cutouts | trace on | trace off | off − on | % | t |
|---|---:|---:|---:|---:|---:|---:|
| seyda-neen-shore | 2628 | 1.885 | 1.952 | +0.068 | +3.6 | 5.4 |
| balmora | 2174 | 1.238 | 1.270 | +0.033 | +2.6 | 5.2 |
| seyda-neen-ship | 1369 | 1.885 | 1.950 | +0.065 | +3.4 | 13.0 |
| andrano-tomb | 16 | 0.825 | 0.880 | +0.055 | +6.7 | 11.0 |
| wolverine-hall | 0 | 1.105 | 1.115 | +0.010 | +0.9 | 1.4 |
| addamasartus | 3 | 1.235 | 1.238 | +0.003 | +0.2 | 1.0 |

**The two nulls read nothing and every place with cutouts reads something**, which is what says the
design is clean rather than that the answer is large. **The saving is 0.03 to 0.07 ms, and it is not
proportional to the cutout count** — a tomb's sixteen candle flames save as much as a shore's 2628
plants — so how many cutouts a cell holds is not the predictor. Against a device frame of 4.6 to 5.9
ms, three per cent of the trace is under one per cent of the frame.

**What it costs is 0.82 ms a frame over the island route**, which is 6.3 ms on each of the 178 frames
of 1200 that bake. Standing still it costs nothing, because the bake is done. So one arrival frame
spends about what a hundred standing frames collect.

### And the bake is not picture-neutral

`verify --exposure=1`, micromaps on against off: **eighteen of twenty-two views draw a different
frame**, worst 129 of 255 on 26.68 per cent of the pixels at `seyda-neen-shore`. The four that do not
move are the four with almost no cutouts — `addamasartus` with 3, `arkngthand` with 11,
`vivec-canalworks` with 41 and `wolverine-hall` with 0. **A null control of two `on` runs against
each other draws the same picture at all twenty-two**, so none of this is run-to-run noise.

**The difference is a regression, and the tree predicted it.** `SceneAcceleration::placeRow` leaves a
micromapped row opaque, so a leaf commits without ever reaching the any-hit; a row without a micromap
is forced non-opaque and every leaf reaches it. The bake decides a microtriangle from the mask at
level zero. The any-hit reads the mask through the ray's cone, at whatever mip the cone resolves.
Where a leaf card is far enough that the cone reads a coarser mip, the micromap keeps every small
hole the finest level holds and the cone closes them — so a distant canopy under micromaps carries a
scatter of pinholes, and a trunk carries them down its length.

`candidateStops` in `lib/traversal.glsl` is the comment that says why that is the worse answer: a
mask read at its finest mip "is a coin toss per pixel — a canopy comes back as speckle, and it crawls
as the camera moves", and letting the cone average it first "is by a long way the better of the two
errors".

**So the bake is right against a comparison the renderer does not make.** `micromap.h` states it is
"conservative against `sampleDiffuse` at the finest level, and exact there", and that is true. No ray
that carries a cone reads the finest level.

### What followed — the bake is gone

`AGENTS.md` ranks the picture first and asks an optional accelerator for a measurement saying the
gain is real. The gain was real and small; the picture cost was real and was not.

**So the whole subsystem was removed** — 2,212 lines in eight files, plus edits in forty-six more.
`SceneMicromaps`, `MicromapPass`, `micromap.h`, `micromap.comp` and their two test files are gone,
and with them `VK_EXT_opacity_micromap` as a required extension and the device gate that refused any
card cutting a four-state triangle below level 6. Every cutout row is now forced non-opaque and every
candidate reaches the any-hit, which is the path `--micromaps=false` already exercised.

Three things went with it because nothing else read them: `SceneStats::mMicromapBytes` and
`mMicromapsUntextured`, `InstanceCounts::mMicromapped`, and `ExtractionStats::mUnbakeable` — a count
of placements wearing an animated cutout, whose only meaning was what no bake could answer for. Four
accessors existed for the bake alone and went with it: `SceneAcceleration::getEveryMesh` and its
`getIndices(MeshRange)`, `SceneBuffers::getTexCoords(MeshRange)` and `TextureArray::getExtent`.

**The one repair not taken** was to make the bake answer the question the any-hit answers: mark a
microtriangle opaque or transparent only where the mask agrees across the mips a cone may read, and
unknown otherwise. It would have removed the speckle and kept some of the three per cent. It was not
worth a subsystem for under one per cent of a device frame, and the reasoning is written here so that
it need not be rediscovered.

**What would bring micromaps back** is a deeper path. The published 55 per cent comes from a path
tracer whose frame is mostly any-hit invocations. If the indirect light grows several bounces, the
saving grows with it, and git holds the code and its tests.

## Finding 5 — the Ray Reconstruction preset does not move the cost

`preset d` against `preset e` at `seyda-neen-ship`, one leg each: `upscale` 2.37 against 2.30 and the
frame 5.87 against 5.80. That is inside the run-to-run spread, so the choice is a picture question
rather than a cost one.

## What the tools can and cannot say

**Nsight Systems needs `--trace=vulkan-annotations`, and it needs a build that labels.** Plain
`--trace=vulkan` records no debug-utils label at all, so `vulkan_gpu_marker_sum` comes back empty and
`vulkan_marker_sum` says the capture holds no such data. That is the switch and not the fork.

The labels are `GpuTimer::open`'s own — every zone this document names is already one — and
`components/rtxvulkan/CMakeLists.txt` compiles them out of `Release`. So a timeline is available on
`build-debug` and not on the build the numbers come from:

```
nsys profile --trace=vulkan-annotations --sample=none \
    build-debug/openmw-rtxtool bench --validation=false --window=false --views=seyda-neen-ship --seconds=4
nsys stats --report vulkan_gpu_marker_sum <capture>.nsys-rep
```

**What it adds over the fork's own timer is one row.** Inside the `upscale` zone sits
`nv.ngx.dlssd.Evaluate`, and it is 1.772 ms of the zone's 1.775 — so what the upscaler costs is the
network's and not this fork's binding of it.

**And what it cannot be trusted for is the ranges themselves.** `waves` reads 0.223 ms against the
timer's 0.22, and `trace` reads 8.6 microseconds against the timer's 1.5 ms. A label bounds a
recording rather than the work the card then does asynchronously inside it, so the timestamps stay
the authority and nsys is for the shape.

What the host trace added: 47 pipeline barriers a frame at 205 ns of host time each, and a
`vkQueuePresentKHR` on every frame even under `--window=false`.

**What a pass costs inside the trace kernel needs the pass removed.** `ncu` is not installed, and the
trace is one dispatch — so the bounce, the caustics, the shadow rays and the sea are one number here.
Pricing them apart is a build per pass and a `shot` each.

## What to do next, in order

1. **Ask what the sea's spectrum costs on a frame that does not need it.** 0.22 ms every frame with
   water in the cell, whatever the camera can see of it.
2. **Price a top-level refit against the rebuild.** 0.24 ms a frame, on frames where only transforms
   moved.
3. **Price the trace's own passes by removing them.** The trace is 0.41 to 4.53 ms depending on the
   place and the extent, and it is one number today.
4. **Leave the upscaler alone unless the picture changes.** It is the largest cost in every frame and
   it is a fixed function of the extent — the only knob on it is which extent to trace, and the
   target already names one that fits.

## How to repeat these measurements

```
cd build-release
for suite in default exteriors skies interiors; do
    ./openmw-rtxtool bench --validation=false --window=false --suite=$suite --seconds=20
done
./openmw-rtxtool bench --validation=false --window=false --suite=streaming --seconds=20 --settled=false

for mode in off ultraperformance performance balanced quality dlaa; do
    ./openmw-rtxtool bench --validation=false --window=false --views=seyda-neen-ship --seconds=20 --upscale=$mode
done

./openmw-rtxtool bench --validation=false --window=false --views=seyda-neen-ship --seconds=20 \
    --size=3840x2160 --upscale=performance
```

Warm the card with one thrown-away `bench` first. The clock and the temperature are printed beside
every result, and a leg whose clock differs from its neighbour's is the leg to repeat.
