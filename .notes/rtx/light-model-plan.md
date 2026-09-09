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
