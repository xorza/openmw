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

### What the cap should have been

The cap's comment gives the honest reason for it: past the first focus the refracted bundle has
folded and one Jacobian no longer describes what is there. What it got wrong is the *number*. A cap
at a stated depth is right, but 140 units was picked by eye where the measurements put the maximum
of the fluctuation, and it stated nothing about what happens past it.

Two things were missing beside it, and the depth law needs all three:

- **A band limit**, so the pattern broadens as the water deepens. The sun is a disc and the surface
  presents a spread of slopes, and both subtend an angle, so a point at depth `h` is lit by a patch
  `h * (sun's angular diameter + bend * rms slope)` across. The mip chain preserves the mean, so
  reading there costs no light.
- **A fade past the focus**, which is the measured `h^-1/2`.
- **A normalisation**, because the tiles carry about a fifth of a real sea's curvature and so cannot
  reach their own fold at the depth the sea does.

### Steps — done

1. The signature did not change. `caustic(vec2 at, ...)` now takes **where the light met the
   surface**, and the caller walks back along `mTravelling` by the slant path it already computed.
   So `sea.glsl` keeps its place below `underwater.glsl` and the geometry lives at one call.
2. `WATER_CAUSTIC_MAX_DEPTH` is gone. Two things replaced it, and neither is a depth.
   - **The band limit.** A point at depth `d` gathers from a patch `d * (sun's angular diameter +
     bend * rms slope)` across, so the tiles are read at that footprint. The rms slope is a property
     of the sea rather than of a place in it, so it rides in `VisibilityConstants::mWaveSlope` off
     `WavePass::getSlope` rather than costing two fetches at every step of a march.
   - **The fold.** `WATER_CAUSTIC_FOLD` holds `bend * sqrt(Var[tr H])` under 0.6, read off the chain
     the Hessian comes from. It recedes as the band limit coarsens the read, so a bed at thirty
     metres is still gathering where the old cap froze everything past two.
3. The contrast **peaks in shallow water and fades as the inverse square root of the depth**, which
   is what Snyder and Dera measured in the sea in 1970 and what every field campaign since has
   found. `WATER_CAUSTIC_SPREAD` broadens the pattern by the same power, which is the law's other
   half. Measured 0.57, 0.53, 0.31, 0.062 and 0.027 at one, two, six, twenty and forty metres, and
   the mean holds within two per cent of one at all of them.

   **The focal depth is a measured constant and not a derived one, and that is the band limit's
   doing.** A real sea's curvature is dominated by ripples far shorter than `sShortestWave`, so its
   first focus lies under a metre. The tiles stop at half a metre of wavelength and hold about a
   fifth of that curvature, so left to themselves they focus at eight metres and draw a pattern a
   fifth as bold as the water has. `WATER_CAUSTIC_FOCUS` puts the focus where it is measured to be,
   and `bend` is scaled so the carried pattern reaches its own fold there — the strength the light
   is measured to be redistributed with, drawn with the shape the transform can carry.

   **And the pattern is filaments rather than blobs, which is what the fold decides.** A caustic
   is bright where the determinant passes near zero, so a map held short of its fold has no
   filaments at all — only broad cells, which read as leopard skin. `WATER_CAUSTIC_FOLD` at 1.4 lets
   the map reach its fold, which is the whole of where filaments come from, and `WATER_CAUSTIC_MAX`
   says only where their tops are cut — so the ceiling is the brightness dial and turning it costs
   no shape. At 3.5 the brightest place on a bed is 3.1 times what a flat sea puts there, and the
   contrast is 0.60 at two metres against 0.53 before.

   The two move together, because `WATER_CAUSTIC_JENSEN`'s correction is fitted against a *clipped*
   tail: at a ceiling of six it had to rise to 1.5 to hold the mean at one, and at 3.5 the term is
   charged as written. The mean is within two per cent at every depth either way.

   **The scale grows as the square root of the depth**, which is `WATER_CAUSTIC_GRAIN` against the
   focus. That is Snyder and Dera's other half and it is a property of branching, not of a blur —
   which matters, because the two blur terms are linear in depth and are still under one texel at
   three metres, so on their own nothing changed in the shallows anyone looks at. The measured fall
   of contrast from two metres to six is 0.60 against the 0.58 the law asks for.

   Sources: [Snyder and Dera 1970](https://opg.optica.org/josa/abstract.cfm?uri=josa-60-8-1072),
   [Wei et al. 2014](https://agupubs.onlinelibrary.wiley.com/doi/abs/10.1002/2013JC009572),
   [Hieronymi 2012](https://os.copernicus.org/articles/8/455/2012/os-8-455-2012.pdf).
4. The march is in `waterColumn`, sun term only, eight even steps. Even rather than bunched: unlike
   the air there is no density falling with height for them to follow.
5. The gate is a **share and not an angle** — `WATER_SHAFT_FLOOR`, the beam against what the whole
   stretch sends. An angle sounds right, because a shaft is the phase function's forward peak, but
   what decides whether the pattern can be seen is the beam against the sky beside it, and that
   turns with the hour, the weather and the depth. Gated at twenty-six degrees the shafts appeared
   only when the sun was looked straight at; against the share they reach past forty-five.

   **And the march returns a ratio rather than a radiance.** The same integrand is summed twice, once
   with the lens at every step and once without, and the closed form is multiplied by the quotient.
   The step count, the jitter and the exponentials all cancel, so a ray with no pattern to show comes
   back as exactly `beam` — which is what lets the pattern fade in across the gate instead of
   switching on. Before that it switched, and drew a hard circle around the sun.
6. `STREAM_WATER` is a fourth blue-noise channel for the march offset. The fog's would have
   correlated: the two marches lie end to end along one ray.
7. `traceVariance` was **not** hoisted. Each step is a whole `caustic`, which is six fetches, and
   the gate is what keeps that off most of the frame. It is the first thing to cut if the shafts
   cost too much.

**Left open.** The shaft is not shadowed by geometry above the water, and it appears at the gate's
hard edge rather than fading in. Both are in `ISSUES.md`.

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

### Steps — done

1. `WATER_FOAM_EDGE` puts the sea's own height under the same root as what the cone lost:
   `noise = sqrt(mLostHeight + (EDGE * mRoughness)^2)`. Two independent widths, and only one of them
   is the camera's — a wave does not break at one depth to the centimetre.
2. `WATER_FOAM_FACE` weights the coverage by the face the wave runs into, which is where white water
   is thrown. The slope comes off `mNormal` and the direction of travel rides in
   `VisibilityConstants::mWaveTravel` as a vector rather than the angle `SeaState` states, so no
   pixel of surf pays a sine. Far enough off the cone averages the slope away and the weight settles
   at a half, which is the share of a wave that faces forward.
3. `WATER_FOAM_ALBEDO` did not move. The brightness was right and the coverage was wrong.
4. `theSurfLineIsAsWideAsTheSeaAndBrokenAcrossIt` looks straight down from three hundred units,
   where a five-unit footprint resolves a spectrum that stops at thirty-two — so the filtering term
   is nought and what is measured is what the water says. An eighth of the frame comes out neither
   covered nor clear, where a hard rim around a filled middle leaves only the pixels straddling the
   edge itself.

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

1. **Done.** No flag was needed: which way the ray went already says what a miss means. Water has
   absolute sides, so a ray leaving the surface *downward* that finds nothing found an unbounded
   column, and one going *upward* found the sky. `waterRay` branches on `direction.z` and returns
   `WATER_UNBOUNDED_PATH` with no radiance for the first. That also fixes the same misreading in the
   reflection from below, which was showing the sky under the sea.
2. The visible edge is gone at `--view seyda-neen-shore`. The honest step is still there — a dark bed
   at thirty metres really is darker than open water — but it is small, and nothing draws a line.
3. **Open.** To close that too, the water needs a bed everywhere it is drawn. Feed the terrain
   quadtree's coarsest level to the extractor, out to the view distance. This is the largest piece
   of work in this document, and it is the only one that also puts the far islands back.
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

### Steps — done

1. `WATER_EXTINCTION` is now an **absorption** spectrum, `(0.262, 0.059, 0.024)` per metre over
   `UNITS_PER_METRE`, which is a new shared constant so the conversion is done by the code. Two
   terms: pure water by Pope and Fry, and the dissolved organic matter that makes it a coastal sea.
2. `WATER_SCATTER` did not move. It is the game's own number and the one thing the game states
   outright about its water, so the extinction was written to agree with it rather than the reverse.
3. The similarity term is named and not carried. With this albedo and `g` at 0.92, `b (1 - g)` is
   between 0.3 and 1.2 per cent of `a` — water this forward-scattering loses a beam to absorption
   alone.
4. Blue is now the channel that outlasts the others, which is what a blue-peaked albedo requires.
5. `waterTakesWhatBeerLambertSaysOverThePathTheLightTook` asserts `red < green < blue`, and
   `surfCoversShallowWaterThatAWaveCanBothBreakInAndReach` asserts the same ordering of deep water.

**What could not be derived, and why.** Read as an albedo, the game's colour implies a scattering
coefficient `b = a w / (1 - w)` that falls toward blue, where every real water's rises with the
fourth power of the wavenumber. So `WATER_SCATTER` cannot be computed from `a` and `b`, and the two
constants stay two. The cost is confined to the colour a very deep column settles at, which is the
number the game states and this defers to. It is written down beside the constant.

---

## Order

Item 5 goes first. It is one constant block, and every other change here is judged by eye against
the colour it sets.

| # | Change | Size |
| --- | --- | --- |
| ~~1~~ | ~~**Item 5** — one derivation for extinction and scattering~~ | landed |
| ~~2~~ | ~~**Item 3, step 1** — a missed refraction is deep water, not sky~~ | landed |
| ~~3~~ | ~~**Item 6** — the backtrack, and the band limit for the cap~~ | landed |
| ~~4~~ | ~~**Item 1** — the march, and the shafts~~ | landed |
| 5 | **Item 2** — the surf line's own width, and its interior | one expression, one weight |
| 6 | **Item 4** — split one loose assertion into two tight ones | two tests |
| 7 | **Item 3, step 3** — a bed to the horizon | largest |

The frame cost sits almost entirely in step 4. Everything before it is fetches the shader already
makes, or constants. Step 7 is the only one that adds geometry.
