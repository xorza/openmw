# One light model: what forks today, and the order it is unified in

Written at `64d3d638b2` plus the uncommitted removal of the sort, against a read of every shader
under `components/rtxvulkan/shaders/`. Every ray count below is derived from the code and says so;
every millisecond is a reading from `.notes/rtx/gpu-performance.md` or is marked as not yet taken.

## What is already one thing

The lamps are one model in every place that weighs them: one `Lamp` and one `falloff`, one
`considerLamp` rule behind both reservoirs, one `litCosine`, one `lightThrough`, one
`ambientReaching`, one `paintedOver`, one `PuffLayer` filled by two walks. Nothing in this plan
touches those.

## What forks, and what each fork costs

| what | surfaces | the air | water shafts | puffs |
|---|---|---|---|---|
| the sky as a source | `skyGlow` at a bounce's escape, `mAmbient` at a path's end | `mFogColour` | `mAmbient` through `daylightReaching` | `mAmbient` through the froxel |
| the sun's visibility | cone-sampled disc, `cloudShadow`, `lightThroughWater` | one straight ray, no cloud | eight straight rays a water ray, no cloud | the air's answer |
| the moons | one drawn by luminance, divided by the draw | both weighed, one ray at the brighter | none | the air's answer |
| indirect light at a seen surface | a traced bounce | — | `pathEnd` with one occlusion ray | — |
| indirect light at a pane | none: `shadeSurface` with zero incoming | — | — | — |

Two of those are pictures that disagree with themselves: a cloud darkens the ground and not the
fog over it or the shaft under it, and a faded actor stands in a room with no bounce while the
solid beside them gets one.

## The stages, in order

Each stage is a commit. Each says what it is gated by, and the gate for all of them is stated once
at the end.

### 1 — One sun and one moon, asked the same way everywhere

**What changes.** `lib/lights.glsl` gains `sunVisible(position, footprint, draw)` and
`moonVisible(position, moon, draw)`: the cone-sampled ray, the cloud shadow and the water's slant
path, in one place. `gather` calls them in place of its two inline blocks. `fogscatter.comp` calls
them in place of its two straight `lightThrough` rays. `waterColumn`'s march calls `sunVisible` at
each step's entry point in place of `lightThrough`. `fogSourcesAlong` stops carrying `mMoonward`,
because the pick is the helper's.

**What it buys.** The cloud shadow reaches the air, the shafts and, through the froxel, every puff
of smoke. The sun's penumbra reaches the air. One statement of what "the sun reaches this point"
means, where there were four.

**What it costs.** Two texture reads per froxel for the cloud sheet, on the sun's ray and the
moon's. Not measured. The `air` zone is 0.16 to 0.25 ms today and the sheet is one texture.

**Risk.** The picture moves wherever a cloud stands over fog or water. That is the point, and the
reference is what says whether it moved the right way.

### 2 — The sun and the two moons are one array and one rule

**What changes.** `VisibilityConstants` carries `DirectionalSource mSky[3]` — a direction, an
irradiance, a limb, and a cloud flag — filled by `FrameWorld` and `OffscreenTrace` from what they
fill `mSunPosition`, `mSunIrradiance` and `mMoons[2]` from today. A source that is down carries an
irradiance of nought and nothing reads its direction. `gather` becomes one loop over the three
with the moons' own rule: weigh each by what it delivers unshadowed, draw one, divide by the draw.
`fogSourcesAlong` becomes the same loop over the same array. `MoonDisc` keeps what the disc itself
needs — the face, the phase, the limb's axes — and loses its irradiance and direction to the source.

**What it buys.** The three hand-written blocks in `gather` become one. The two moon fields and the
two flags in `FogSources` go. At dusk a surface traces one directional ray where it traces two
today, and so does a froxel. `HAS_SUN` and `HAS_MOONS` fold into one `SKY_SOURCES` count.

**What it costs.** Noise where the sun and a moon both reach a point, which is the hour either
side of dusk. The reference measures it, and the accumulator is what carries it.

**Risk.** The pick's weight is the unshadowed delivery, so a moon behind a headland still draws its
share of the pixels and returns nought for them. That is the same estimator the lamps run on, and
`lib/lights.glsl` records what the lamps' version cost against tracing every candidate: 0.03 per
cent at Seyda Neen's customs office and 0.32 at Wolverine Hall.

### 3 — A pane gets the same indirect term a water ray's hit gets

**What changes.** `answerSolid`'s pane path calls `shadeSurface` with
`pathEnd(position, ambientReaching(...))` for its incoming, seeded off `paneSeed` beside the
lamps. One occlusion ray on a pane pixel.

**What it buys.** A faded actor and a pane of glass stop being the two surfaces in the frame with
nothing bouncing near them.

**What it costs.** One ray on the pixels the eye sees through, counted with a see-through map
over the twenty-two `verify` views: about six per cent of the frame at the guild, under two per
cent at Ald-ruhn, and under a tenth of a per cent at every other view that holds any.

### 4 — A reservoir holds a lamp's index and not a copy of it

**What changes.** `Reservoir` loses `mTowards`, `mDistance`, `mSourceRadius` and `mClearance` and
gains `uint mLamp`. `lampVisible` rebuilds the `Lamp` with `lampAt(lightAt(kept.mLamp), kept.mFrom)`
before it aims. `aimLampFrom` becomes one assignment to `mFrom`, because the rebuild answers the
direction from the new origin for nothing — the recovery arithmetic it does today goes.

**What it buys.** Five words off the live state of every closest-hit shader and of the froxel
pass, and one fewer way to hold a lamp. The closest-hit shaders were last read at 96 registers
with spills, and no driver here reports the count any more, so the `trace` zone is the reading.

**What it costs.** One lamp row read at the shadow ray, which the walk already read once.

**Risk.** None to the picture: the same lamp, the same origin, the same direction.

### 5 — The air's sky term is decided and written down

**What changes.** Either nothing, and `fogscatter.comp` gains the sentence that says `mFogColour`
is Morrowind's own record of what the air scatters and is kept as the one deliberate fork — or the
froxel scatters the sphere mean of `skyGlow` where a room scatters `mAmbient`, the way a puff and
a path's end already do.

**What it buys.** The second form lights a fog bank and the ground under it by one sky. The first
form keeps the weather's own colour, which is the content's.

**How it is decided.** Both drawn at `seyda-neen-ship-dawn`, `balmora-fog-night` and
`dagoth-ur-caldera`, against the accumulated reference and against the rasterizer's own frame.
This is a look decision and the pictures decide it.

### 6 — Three measurements, each its own commit if it pays

- **The shaft march at four steps.** `WATER_SHAFT_STEPS` from eight to four. The ratio estimator
  normalises over its own steps, so the beam's body is exact either way and only the pattern's
  quadrature coarsens. Halves a water pixel's worst case from sixteen shaft rays to eight, derived.
  Gated by the accumulated reference at `seyda-neen-shore` and `vivec`.
- **`fogSourcesAlong` per column.** It depends on the column's direction and nothing along the
  ray, and runs once per froxel today. `fogdepth.comp` runs once per column and can store it beside
  the depth: two colours and the pick, in one more column image on `Rtx::FogVolume`. Sixty-four
  phase evaluations become one. Read on the `air` zone.
- **The albedo view as a specialization constant.** `frame.mShowAlbedo` is fixed for a run, the way
  `COUNT_HITS` is, and it stands as a uniform branch inside `answerSolid`. A sixth constant folds
  it out of the production closest-hit shaders. Read on the `trace` zone, and expected to be inside
  the spread.

### 7 — Small consolidations, one commit

- `moonFace` and `skyPatches` spell one disc mapping twice: a `discAt(direction, right, up, limb)`
  helper.
- `resolveFor` builds its empty `Surface` field by field where `noHit` has a function: a
  `noSurface()`.
- `waterRay` and `waterUnbounded` answer the miss question with two tests. The comment already
  says why they differ; the code should say it in one place, with the origin's side as the
  argument.

## What is not in this plan

- **The bounce traced from the launch and sorted.** The one arrangement a reorder could win on,
  and a restructure of the closest-hit shaders. `.notes/rtx/gpu-performance.md` Finding 3 says
  what it would take; it waits for the indirect light to deepen.
- **Every branch that guards a ray.** A ray skipped is the saving the branch buys, and the trace is
  rays. `sunUp() && cosine > 0`, the lamp's reach test, the shaft floor and the bounce's reach all
  stay.
- **The compile-time variants.** The sun, the moons, the sea and the two counters are the right
  things to fold, and stage 2 folds two of them into one.
- **The sprite shading pass.** It answers shadow between sprites of one emitter at sprite scale,
  which the froxel grid is eight pixels too coarse for.

## The gate for every stage

```
ninja -C build-release openmw-rtxtool components-tests
build-release/components-tests --gtest_filter='Rtx*'
build-release/openmw-rtxtool verify --validation=false --exposure=1 --upscale=off --filter=false --against=<previous>
apps/rtxtool/repeatable.sh --build=build-release
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```

A stage that may move the picture — 1, 2, 3, 5 and the shaft — is read against a converged
reference as well: `shot --accumulate=1000 --upscale=off --filter=false --exposure=1` at the
views its section names, before and after, and the single frame through the wavelet against each.
A stage that may move a frame time — 4 and the three in 6 — is benched interleaved, three rounds,
at `seyda-neen-ship` and `balmora-mages-guild`, on a warm card, reading the `trace` and `air`
zones with their spread.

## Ray budget, for reading the results against

Derived from the code at this commit, per pixel, out of doors, before any stage:

| pixel | traversal | shadow | bounce | at the bounce's hit | total |
|---|---:|---:|---:|---:|---:|
| solid | 1 | up to 3 | 1 | 1 occlusion, and 2 shadow at half rate | about 7 |
| pane | 1 per layer | up to 3 per layer | 0 | 0 | 4 per layer |
| water | 1 | 0 | 2 traced, 1 shore | 4 each | 20 to 35 with the shafts and the bed |
| sky | 1 | 0 | 0 | 0 | 1 |

And per froxel of the air: one sun, one moon, one lamp, one occlusion — four, which at the
volume's one froxel a pixel is four rays a pixel, coherent, and 0.16 to 0.25 ms measured.

## What happened

Implemented in order on the tree above, each stage snapshotted and read against the one before it
with the gate at the end of this file: `verify` over the twenty-two views, a thousand-frame
reference and one frame at six views, and three interleaved rounds of `bench` at the ship and the
guild. The bench's `trace` is the closest-hit shaders and the launch; `air` is the fog volume. All
in milliseconds, three rounds each, and the card sat at 64 to 71 °C throughout.

### Stage 1 — one sun and one moon

Fourteen views moved. The nine that moved by one of 255 are the arithmetic re-associated around the
new call. The five that moved for real are the views with a cloud deck over fog or water —
Ald-ruhn on 42 per cent of its pixels, Dagon Fel on 57, Vivec on 38, the shore on 54 and the
caldera on 7 — which is the deck's shadow reaching the air and the shafts.

| view | reference moved, of its mean | one frame against its reference, before → after |
|---|---:|---:|
| seyda-neen-shore | 0.9 % | 0.00290 → 0.00290 |
| seyda-neen-ship | 0.07 % | 0.00888 → 0.00888 |
| dagoth-ur-caldera | 0.05 % | 0.00833 → 0.00833 |
| the other three | under 0.05 % | unchanged |

| place | trace before | trace after | air before | air after |
|---|---:|---:|---:|---:|
| seyda-neen-ship | 1.53, 1.62, 1.57 | 1.56, 1.53 | 0.23, 0.24, 0.24 | 0.23, 0.21, 0.24 |
| balmora-mages-guild | 1.24, 1.24, 1.24 | 1.24, 1.25, 1.24 | 0.10 | 0.10 |

One leg of the ship's `after` read 4.91 and is struck: a validated debug shot of my own ran beside
it. The cloud sheet's two reads per froxel cost the `air` zone nothing the timer can see.

### Stage 2 — the sun and the moons as one array and one rule

Implemented as `skySourceAt` reading the frame's own sun and moons, and not as a fourth field the
host fills, because the frame already states each once. Eighteen views moved by one of 255, and
`seyda-neen-ship-dawn` moved on 9.6 per cent of its pixels, worst 73 — the hour when the sun and a
moon both reach a point, where one ray now goes to one of them.

The dawn's reference moved by 0.02 per cent of its mean, so the draw is unbiased, and its one frame
went from 0.00256 to 0.00258 against its reference: a noise cost of under one per cent, at that hour
only. Every other reference is unchanged to five figures.

| place | trace before | trace after |
|---|---:|---:|
| seyda-neen-ship | 1.56, 1.56, 1.57 | 1.57, 1.60, 1.57 |
| balmora-mages-guild | 1.24, 1.25, 1.25 | 1.26, 1.26, 1.24 |

Inside the spread at both, as expected: neither view has both up. What it bought is the code — one
loop where there were two blocks, and one pick rule shared with the air.

### Stage 3 — a pane gets the path's end

Eight views moved, all of them where the eye sees through something: the guild on 10.5 per cent of
its pixels, worst 41, and Ald-ruhn on 1.2. The guild's reference brightened by 2.3 per cent of its
mean, which is the panes and the faded actor gaining the term they lacked, and its one frame went
from 0.00048 to 0.00055 against its reference — the one occlusion ray's noise.

| place | trace before | trace after |
|---|---:|---:|
| seyda-neen-ship | 1.62, 1.59, 1.60 | 1.63, 1.63, 1.63 |
| balmora-mages-guild | 1.26, 1.28, 1.27 | 1.29, 1.29, 1.29 |

About two per cent of the trace at the guild, where six per cent of the pixels are see-through, and
the same at the ship, which has almost none — so the ship's is the spread, and the guild's is the
ray.

**One test moved with stage 1 and was re-measured.** `aShaftIsBlockedByWhatStandsOverTheWaterWhereTheSunEnters`
asserted that a strip over the shaft's entries leaves five per cent of it. The entries' ray is now
drawn across the sun's two-degree penumbra, which at a lid five hundred units up is seventeen units
either side of its edge, so the entries under the rim take part of the sun: 0.107 with eight steps
and 0.115 with four. The expectation is now 0.115 and the comment says why.

### Stage 4 — the reservoir names its lamp

Three views moved by one of 255 on a handful of pixels: the air's ray now aims at the lamp's own
row rather than at a position rebuilt from the walk's direction and distance, and the two differ in
the last place. Every reference is unchanged to five figures.

| place | trace before | trace after |
|---|---:|---:|
| seyda-neen-ship | 1.60, 1.62, 1.62 | 1.62, 1.63, 1.62 |
| balmora-mages-guild | 1.27, 1.28, 1.28 | 1.26, 1.27, 1.28 |

Inside the spread. Five words off the live state do not show on this timer, and no driver here
reports the register count that would say whether they showed anywhere. What it bought is the code:
`aimLampFrom` is one assignment, and a lamp is held one way.

### Stage 5 — the air's sky term stays the weather's, and the shaft takes four steps

The decision is written over the term in `fogscatter.comp`: the fog colour is the content's own
record and stays the one deliberate fork. No picture moved for it.

The shaft at four steps moved nine views by one of 255 on under a third of a per cent of their
pixels. The shore's reference — the one view in the set with shafts — moved by 0.01 per cent of its
mean, and its one frame went from 0.00290 to 0.00291 against it: the pattern's quadrature is
coarser and nothing else is. The ship and the guild show no shafts, so their trace did not move;
the shore is benched below on its own.

### Stage 6 — the phase terms move to the column pass

Every view is the same picture, bit for bit: the terms are the same numbers computed once a column
instead of once a froxel, and stored in one more column image three layers deep.

| place | air before | air after | trace before | trace after |
|---|---:|---:|---:|---:|
| seyda-neen-ship | 0.24, 0.24, 0.24 | 0.24, 0.24, 0.24 | 1.59, 1.61, 1.60 | 1.59, 1.62, 1.59 |
| balmora-mages-guild | 0.10, 0.10, 0.10 | 0.10, 0.10, 0.10 | 1.29, 1.30, 1.29 | 1.28, 1.28, 1.30 |

Nothing the timer resolves at two places. Three Mie evaluations a froxel were not a share of the
`air` zone this timer can see, and what stays is the simpler pass: the scatter pass reads three
texels where it evaluated three phases.

**The albedo view stays a uniform branch.** The plan's third measurement was to fold
`mShowAlbedo` into a specialization constant. The visibility tests toggle it per frame on one
built pass — nine sites — so a build-time constant would mean a second pipeline set per test for a
gain the plan already expected inside the spread. Not done, and this is why.

**The shaft at four steps, benched where the shafts are.** Three rounds interleaved at
`seyda-neen-shore`, the four-step build against the eight-step one:

| steps | trace | air |
|---|---:|---:|
| eight | 1.97, 1.96, 1.99 | 0.14 |
| four | 1.73, 1.74, 1.75 | 0.14 |

**0.23 ms off the trace, twelve per cent of it, at a view that is mostly water**, for a reference
that moved by 0.01 per cent of its mean. That is the one stage in this plan that bought frame time.

### Stage 7 — the small consolidations

`discAt` serves the moons and the painted patches, and `noSurface` builds the empty surface
`resolveFor` used to fill by hand. Every view is the same picture, bit for bit, and the 599 tests
pass. The third item — `waterRay` and `waterUnbounded` answering the miss question with two tests —
is left as it was: the two tests are two questions after all. One asks which side of the plane a
ray leaving the surface goes to, and the other whether the eye's own ray stands under the water,
and folding them needs an argument for the origin's side that neither caller has for nothing.

## Where it stands

| stage | picture | trace at the ship | trace at the guild | kept |
|---|---|---:|---:|---|
| 1 — one sun and moon | clouds now shadow the air and the shafts; under 1 % at the shore, less elsewhere | 1.57 → 1.55 | 1.24 → 1.24 | yes |
| 2 — one sky array and rule | dawn draws one ray for two; unbiased, under 1 % more noise there | 1.56 → 1.57 | 1.25 → 1.26 | yes |
| 3 — the pane's indirect term | the guild's panes brighten 2.3 % | 1.60 → 1.63 | 1.27 → 1.29 | yes |
| 4 — the reservoir by index | unchanged | 1.62 → 1.62 | 1.28 → 1.27 | yes |
| 5 — the shaft at four steps | the shore's reference moves 0.01 % | — | — | yes: −12 % at the shore |
| 6 — phase terms per column | bit-identical | 1.60 → 1.59 | 1.29 → 1.28 | yes |
| 6 — albedo view folded | — | — | — | no: the tests toggle it per frame |
| 7 — consolidations | bit-identical | — | — | yes |

Medians of three. Every stage's converged reference is within a per cent of the one before it
except where the stage's own purpose moved it, and the single frame's distance from its reference
moved by under one per cent anywhere, at the dawn and the guild only. Over the whole plan the ship's
trace read 1.57 at the start and 1.59 at the end, inside the spread; the guild's 1.24 and 1.28,
which is the pane's ray and the one real cost; the shore's fell by 0.23.
