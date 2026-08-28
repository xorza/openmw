# Water: six reports, four causes

What is wrong with the water, where it is wrong, and the order to fix it in. Written against the
tree at the FFT wavefield — `WavePass`, `sea.glsl`, `water.glsl`, `underwater.glsl`.

Six things were reported. They are four causes. Two of the reports share one fix, and one of them is
a fault in a test rather than in the renderer.

Three were confirmed by render, at `--view seyda-neen-shore` and at a hand-placed camera in cell
`-2,-9`:

- the surf line draws as an even white outline around an island, hard on both sides;
- a rock two metres away underwater carries the same green as the open sea;
- a straight edge crosses the sea, and it does not move when the sun does.

---

## 1 and 6 — the caustic is read in the wrong place

**Reported.** Underwater shafts are missing. The caustics look the same at every depth.

### Cause

`caustic(vec2 at, float depth, float footprint)` at `sea.glsl:241` reads the curvature tiles under
the **shaded point's own** `xy`. The light did not enter the surface there. Refraction carries it
down-sun, so the surface patch that focused onto a point at depth `h` sits at

```
entry.xy = p.xy - travelling.xy * h / |travelling.z|
```

with `travelling` the refracted direction from `sunUnderWater`. Three things follow from sampling at
`p.xy` instead.

- **The pattern cannot move with depth.** Only the strength of the Jacobian varies, and see below
  for why even that does not.
- **`caustic` never sees the light direction.** A moon is above the water and stands somewhere
  else, and it draws the sun's pattern.
- **The depth cap freezes the strength.** `bend = WATER_REFRACTION_BEND * min(depth,
  WATER_CAUSTIC_MAX_DEPTH)` at `sea.glsl:274`, and the cap is 140 units — about two metres.
  Vvardenfell reaches thirty. Every bed below two metres gets one contrast.

That is the whole of the second report: the pattern is fixed in place and fixed in strength.

It is also why there are no shafts. `waterColumn` says so itself at `underwater.glsl:117` — *"No
caustic in it, so there are no shafts yet."* A shaft is this pattern read at a run of depths along
one ray. Without the backtrack the pattern would extrude straight down as columns rather than lean
down-sun, so marching the current `caustic` would not draw a shaft at all. **The backtrack is what
turns a march into a shaft.**

### Why the cap can go

The cap's comment gives the honest reason for it: past the first focus the refracted bundle has
folded, one Jacobian no longer describes what is there, and evaluated at the bed the model starts
*making* light rather than moving it. Both halves of that are consequences of evaluating at the bed.
Evaluated where the light left, the Jacobian is the right object, and what remains is a question
about band limit rather than about validity.

The band limit is derivable. At depth `h` the pattern is blurred by the sun's own angular width and
by the spread of surface slopes, so the tiles must be read at

```
footprint = max(cone, h * (2 * SUN_ANGULAR_RADIUS + 2 * bend * rmsSlope))
```

The mip chain preserves the mean, so the pattern softens and coarsens with depth and still conserves
light. Both terms grow linearly with depth, which is what makes deep caustics wash out on their own.

### Steps

1. `caustic(vec3 position, vec3 travelling, float footprint)`. The caller has `travelling` from
   `sunUnderWater`, so `sea.glsl` keeps its place below `underwater.glsl` and nothing circles.
2. Compute `depth` and `entry` inside. Sample the tiles at `entry`.
3. Delete `WATER_CAUSTIC_MAX_DEPTH`. Put the depth-driven band limit above in its place.
4. Test: the contrast falls as depth grows, and the pattern translates down-sun by
   `depth * tan(refracted angle)`.
5. Add a sun-term march to `waterColumn` (`underwater.glsl:132`). Keep the sky term closed form —
   it is exact and has no pattern in it.
6. Gate it as `fogAlong` gates shafts: no sun, no march. Reuse `fogDepth` for the step bunching and
   the per-pixel jitter, and `lightThrough` once per stretch for the shadow.
7. Hoist `traceVariance` out of the step loop. Each step then costs one curvature fetch per tile,
   which is two.

---

## 2 — the surf line is a step, not a probability

**Reported.** Close to the camera the surf line draws as a solid white ribbon, hard on both sides.

### Cause

`foamBreaking` returns `smoothstep(-1.6, 1.6, margin / noise)` with

```
const float noise = max(sqrt(surface.mLostHeight), 1.0e-3);   // sea.glsl:186
```

`mLostHeight` is the elevation variance the **ray cone** averaged away. It is a filtering quantity.
At close range the cone resolves everything, `mLostHeight` is nought, the floor of `1e-3` stands,
and the smoothstep collapses to an indicator on `margin`.

`covered` is then 1 across the whole surf zone (`water.glsl:314`) and `mix(water, foam, 1.0)` is a
lit white Lambertian. The band's two edges are the breaker line and the waterline, and both are
sharp. That is the ribbon, and it is why it is even in width: it is a region, not a fall-off.

**The width of the surf line is a property of the camera, and it has to be a property of the sea.**

### Steps

1. Floor the noise with a sea-state term rather than with a divide guard:
   `noise = sqrt(mLostHeight + pow(WATER_FOAM_EDGE * surface.mRoughness, 2.0))`. The band then
   softens by the height of the sea, and it never collapses however near the camera stands.
2. Break the interior. Aeration is on the steep shoreward face of a wave and not over the whole
   zone. Weight the coverage by the slope along the wind, which `field.yz` already carries — no new
   fetch. Carry the sea's bearing in `VisibilityConstants`, beside `mWaveExtent`.
3. Leave `WATER_FOAM_ALBEDO` at 0.55. The brightness is right. The coverage was wrong.
4. Test: at a footprint of nought the coverage still has a gradient across the zone, and its
   integral across the band does not move.

---

## 3 — the edge is the seabed's, and the sky leaks under the sea

**Reported.** The sea ends at a straight diagonal edge seen from a high camera, with a different
water colour beyond it.

### Cause

Not the sheet. `createWaterGeometry(CellSizeInUnits * 150, 40, 900)` is 150 cells across — about
seventeen kilometres, and re-centred on the cell. Three other things are at work, and all of them
are about the bed.

- **A missed refraction returns the sky.** `waterRay` writes `skyRadiance` on a miss
  (`water.glsl:112`). A refraction travelling **down** that finds nothing has found deep water, not
  sky.
- **The miss is given a finite path.** `WATER_MAX_PATH` is 2000 units, about twenty-nine metres
  (`water.glsl:26`). Green keeps 24 per cent over that, so the sky arrives under the sea at a
  quarter strength.
- **The bed stops at the distant-terrain grid.** With `distant land cells = 5` that is near 585
  metres. The sheet is 150 cells.

So the bright teal beyond the line is the sky, read through 2000 units of water. The line is
straight because the loaded grid is square, and it holds still across the day because it is geometry
rather than shadow — confirmed by rendering the same camera at hour 12 and hour 16.

### Steps

1. Split the miss. Give `waterRay` a flag, or read `mFound` at the call site. For a refraction from
   above, a miss sets the radiance to nought and the distance to infinity, and the column settles at
   its own limit.
2. That removes the bright side of the step. What is left is honest: a dark bed at thirty metres
   really is darker than open water.
3. To close that too, the water needs a bed everywhere it is drawn. Feed the terrain quadtree's
   coarsest level to the extractor, out to the view distance. This is the largest piece of work in
   this document, and it is the only one that also puts the far islands back.
4. Test: the water's colour is continuous as the bed's distance passes `WATER_MAX_PATH`.

---

## 4 — the cone test compares two different questions

**Reported.** `waterTooFineToResolveWidensTheConeItRefractsThrough` and the frame disagree by 0.088
of a mip level, against 0.025 before the surface became an FFT field.

### Cause

A fault in the test, not in the renderer.

`lobeOf` integrates the **whole** spectrum's slope variance. The shader reads the slope variance the
mip chain removed **at the level the cone reached**, blended across two levels, out of half floats.

At that camera the footprint on the water is 66 units. `waveLevel` then gives 3.05 for the wide tile
(texel 8 units) and 2.48 for the narrow one (texel 11.9). Neither is the coarsest level, so the
shader is not reading the whole spectrum's variance and never was. The two are not the same number,
and no tolerance makes them one.

The sinusoid table agreed to 0.025 because a table of sixty-four had no chain and no level to read
at — the whole of it was either resolved or not.

### Steps

1. In `RtxWavePassTest`, assert the chain's lost slope at a level equals the spectrum's variance
   above that level's cutoff. That pins the chain by itself, where `momentOf` already lives.
2. In the visibility test, read the lost slope off the textures at the level the footprint selects,
   with `Testing::readHalves`. Build the analytic cone from that number. Tighten to about 0.03.
3. If a residual survives, two suspects remain and each is measurable alone: a trilinear blend of
   two levels raises a variance by convexity, and `coneLod`'s `- log2(facing)` raises the level for
   any tilt at all.

---

## 5 — the water's colour has two sources that disagree

**Reported.** The water is too green near the surface, and so is anything seen through a short path
of it. Real water reads that green only after tens of metres.

### Cause

Two, and the first is the hue.

- `WATER_SCATTER` is Morrowind's own `Water_UnderwaterColor`, `(12, 30, 37) / 255 * 0.85` —
  **blue above green** (`scene.h:422`).
- `WATER_EXTINCTION` is Jerlov coastal, `(0.32, 0.05, 0.08)` per metre — **green survives and blue
  does not** (`scene.h:329`).

Extinction decides the hue of a lit column. So the water reads green, and the blue the game states
never appears. Two descriptions of one water, disagreeing about which end of the spectrum lives.

The second is the rate. `WATER_ASYMMETRY` is 0.92 (`scene.h:431`), so ninety-two per cent of what
scatters continues in the direction it was already going. The beam loses all of it and a
single-scattering term returns a little of it. Water is therefore far more murky per metre than that
asymmetry allows, which is what puts the colour on a two-metre path instead of a thirty-metre one.

Measured: a rock two metres away, lit through about three and a half metres of downward path, comes
out at roughly `(0.21, 1.0, 0.83)` relative — a saturated green, at a distance where real water is
nearly clear.

### Steps

1. Choose one water. State `a(λ)` and `b(λ)` for it in the comment, per metre, and cite the source.
2. Derive **both** constants from that one pair: `WATER_SCATTER = b / (a + b)` and
   `WATER_EXTINCTION = a + b`. One source, two constants, and no way for them to drift apart.
3. Apply the similarity approximation to the beam: `a + b * (1 - g)` with `g = WATER_ASYMMETRY`.
   Name the reason — the renderer removes forward-scattered light that it never gives back.
4. Check the hue holds together. With blue highest in `WATER_SCATTER`, blue must be **lowest** in
   `WATER_EXTINCTION`. Today green is lowest.
5. Test: a white surface one metre under water keeps most of its green and blue, and thirty metres
   reads blue rather than green.

---

## Order

Item 5 goes first. It is one constant block, and every other change here is judged by eye against
the colour it sets.

| # | Change | Size |
| --- | --- | --- |
| 1 | **Item 5** — one derivation for extinction and scattering | one constant block |
| 2 | **Item 3, step 1** — a missed refraction is deep water, not sky | a few lines |
| 3 | **Item 6** — the backtrack, and the band limit for the cap | one function |
| 4 | **Item 1** — the march, and the shafts | a march, gated |
| 5 | **Item 2** — the surf line's own width, and its interior | one expression, one weight |
| 6 | **Item 4** — split one loose assertion into two tight ones | two tests |
| 7 | **Item 3, step 3** — a bed to the horizon | largest |

The frame cost sits almost entirely in step 4. Everything before it is fetches the shader already
makes, or constants. Step 7 is the only one that adds geometry.
