# Water: what is left, and what to do about it

Six defects were opened against the water. Two are closed. What follows is the root cause of each of
the four that are not, and the change each one needs.

| # | Issue | Depends on |
| --- | --- | --- |
| ~~1~~ | ~~The cone test's 0.10 of a mip level~~ | ~~nothing~~ |
| ~~2~~ | ~~The caustic does not conserve at a fold of three~~ | ~~nothing~~ |
| ~~3~~ | ~~The caustic still makes 4 per cent of light~~ | ~~nothing~~ |
| 4 | The caustic pattern reshuffles 67 per cent in a twelfth of a second | ~~3~~ |
| 5 | A shaft is not shadowed by what stands over the water | nothing |
| 6 | The surf line is flecks rather than a band | nothing |

---

## ~~1. The cone test measures one sample of a field and compares it against an ensemble~~

**Done. The model was right and the measurement was not.** The test reads the linear radiance over an
eleven-by-eleven patch of the bed and asserts to 0.02 of a level, where it measures 1.4328 against a
prediction of 1.4255 — a residual of 0.0073, which is the two Jensen terms the comment computes.

## ~~2. The caustic's correction was a series where no series converges~~

**Done, from 12 per cent to 4.** `causticGain` replaced `WATER_CAUSTIC_JENSEN`: the exact mean of the
clipped reciprocal over a Gaussian Hessian, as a curve in one variable, fitted to four million draws
and asserted by `RtxCausticGainTest`. The means at one, two, six, twenty and forty metres read 0.991,
1.039, 1.037, 1.008 and 1.001 against the estimator's own 1.128, 0.986, 1.203, 1.321 and 1.221. The
pattern came out bolder — contrast at two metres from 0.487 to 0.538 — because the old coefficient
was over-dividing the shallows.

---

## ~~3. The gain is evaluated at a fold measured from the very sample it corrects~~

**Done, from 4 per cent to under 1.** Measured at one, two, six, twenty and forty metres the bed now
receives 1.009, 1.002, 1.004, 0.998 and 1.000 of what falls on the water. The tolerance in
`theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt` is 0.02, where the plan asked for 0.02 and the
issue opened at 0.12.

**And the root cause was neither of the two the plan named.** The correlation was real and is gone,
but what actually mis-stated the fold was a filter nobody had written down: `textureLod` reconstructs
*between* a level's texels bilinearly, and that second filter passes `(2 + cos(k w)) / 3` of a
frequency's power over tap positions spread through a texel. Left out, the fold stood at four thirds
of the truth in the shallows and nearly three times it in deep water. Measured against the shader's
own Hessian, the table now agrees to 3 per cent where the pattern has contrast.

**The measurement had to be fixed before any of it was visible.** The test looked at 270 units of
bed — sixty correlation lengths — so every mean it reported carried about ten per cent of sampling
error, twice what it was asserting. It looks at 1080 units at 256 pixels now, which holds the
footprint where it was, and one frame agrees with sixteen decorrelated ones to 2 per cent. It also
read the ratio out of an 8-bit byte, which it no longer does.

**What went with it.** `waveVariance` had one reader in the tree and now has none, so the tile, its
mip chain, its slot in `wavecompose.comp` and binding 23 are all gone — two texture fetches a cascade
off the caustic's inner loop, and the wave pass composes two images rather than three.

### What it was

The bed two and six metres down receives 4 per cent more light than falls on the water.

### Why — and what it is not

`causticGain(f)` is the mean of the estimator *conditioned on the fold*, and it peaks at 1.294. What
the frame needs at six and twenty metres is a divisor of 1.32. **No argument to the curve produces
that**, so the fault is not in the fit and not in the curve's shape.

Two candidates were measured and neither survives:

- **Directional spreading.** A `cos^2` spread moves the curve under 1 per cent, and a wholly
  unidirectional field moves it the *wrong way* — down, where the residual needs it up.
- **The mip's own anisotropy.** A square box is separable and passes the axes better than the
  diagonals, so the resolved field could be four-fold anisotropic even from an isotropic sea. Over
  the spectrum's own band it is not: `c40` comes out 0.374 to 0.376 against the isotropic 0.375, at
  every box width the caustic reads at.

**What is left is that `f` is a per-pixel estimate correlated with the determinant it normalises.**
The shader computes

    traceVariance = whole - max(local - trace^2, 0)

where `local` is the footprint's own mean of `(tr H)^2` at this pixel. That term is *large exactly
where the local curvature is large* — so `traceVariance` is small there, `f` is small there, and `G`
is evaluated near its own left-hand side where it is close to one. The pixels that need the largest
divisor are handed the smallest. The sign is right, the mechanism is exact, and it explains why the
required divisor exceeds a curve whose maximum is a property of the ensemble.

### The change

**The fold is a property of the band, not of a place, and it should be computed as one.**

`Var[tr H]` after a box of width `w` is a sum over the tile's own amplitudes:

    V(w) = sum 2 |A|^2 k^4 T(k, w)^2

with `T` the Dirichlet kernel `Testing::lostSlopeOf` already uses — a level of the chain is a mean of
point *samples*, not a box over a continuous field.

1. Give `WaveCascade` that sum per mip level, as the dimensionless ratio `V(w) / V(0)`, so the shader
   reads `f = toward * sqrt(ratio)` and needs no scale.
2. Upload it in `VisibilityConstants`, one small array a cascade, and blend between levels the way
   `textureLod` does — which is the same blend `lostSlopeOf` models, so one statement covers both.
3. Read `traceVariance` from that instead of differencing two texture fetches.
4. **Then `waveVariance` has no reader left.** `caustic` is the only one in the tree, and both of its
   fetches go — so the tile, its mip chain, its slot in `wavecompose.comp` and binding 23 all go with
   it. Two fetches a cascade come off the caustic's inner loop, and the pass composes two images
   rather than three.
5. Test with `wavemoments.hpp`'s own machinery: the table against the chain, the way
   `RtxWavePassTest` already checks `lostSlopeOf`. Then tighten
   `theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt` to 2 per cent at every depth.

**What stays wrong.** Where the determinant is negative a point is reached by three patches of
surface and this draws one. Normalising absorbs the mean of that and never its shape. Reading all
three is a root-finding problem per pixel, and it is not this.

---

## 4. The caustic tears — and the judgement on it was made against a different surface

### What it is

62.1 per cent of the pattern is new a twelfth of a second later. The sweep `sShortestWave` was chosen
on put tearing at half, and its 73 per cent leg "read as stripes running across the bottom".

### Why

Curvature weights a wave by `A k^2`, so the Hessian belongs to the short end of the spectrum whatever
the spectrum does, and the fold is a nonlinearity applied to that Hessian. It puts the contrast into
thin filaments — the finest structure in the field, made of the fastest-turning waves. A 32-unit
wave, which is where `sShortestWave` cuts, turns at `sqrt(2 pi g / lambda)` = 11.1 rad/s and
decorrelates almost exactly over the twelfth of a second being measured.

**But the number is being read against a sweep taken on a table of sixty-four sinusoids**, where the
shortest few owned the Hessian in four directions and the pattern was a lattice. A lattice
reshuffling is what reads as stripes. Tens of thousands of wavevectors interfere into a mottle at the
same wavelengths, and the same percentage need not look the same at all.

### The change

1. **Look before tuning.** Two `shot` frames a twelfth of a second apart over a shallow bed, and the
   question is whether the pattern reads as stripes or as water. If it reads as water the issue
   closes on the evidence, and the 62 per cent is a number about a field rather than a fault.
2. If it does tear, give the lens its own band limit. `widened` serves two masters — the
   anti-aliasing limit and the choice of which waves make the lens — and at two metres it comes out
   at 9.5 units, which passes a 32-unit wave at 0.86 of its amplitude. A floor of 27 units passes the
   same wave at 0.18. Derive the floor from the coherence a frame can hold: a wave is worth drawing
   while `omega dt` is under a radian, which is `lambda >= 2 pi g (dt / theta)^2`.
3. Sweep `(coarsen, fold)` against the two figures the test already measures. The claim to check is
   that a coarser lens at a higher fold reaches the same contrast at a lower reshuffle, because the
   fold is a nonlinearity and can put back what the filter took.
4. **Item 3 first.** Raising the fold to buy contrast back is what did not conserve, and the fold's
   own reach is limited by how well the gain is normalised.

---

## 5. A shaft is not shadowed by what stands over the water

### What it is

A rock over the surface casts no gap in an underwater sun shaft.

### Why

`waterColumn` never asks whether the sun reaches the point it is scattering at. A submerged *surface*
is shadowed — `shadeSurface` traces its own ray, and water carries a mask bit that keeps it out of
occlusion — so the bed under the rock is dark and the water in front of it is not. The volume has no
such ray: the closed form is an integral along the sun's own line with nothing on it, and the march
multiplies in `caustic` and nothing else.

### The change

The march is already a ratio estimator — the same integrand twice, once with the surface's lens and
once without — so visibility belongs in the numerator beside the lens, and a stretch with nothing
over it comes back as exactly the closed form.

1. At each step the sun met the surface at `at.xy - sun.mTravelling.xy * reach`, at the water level.
   The march has that point already; it is where `caustic` is read.
2. Multiply the step by `lightThrough(entry, frame.mSunPosition, frame.mFar)` — `MASK_SOLID`, so the
   water does not shadow itself, and the same helper every lamp uses.
3. **Outside the `mix` that fades the pattern in, not inside it.** A rock's shadow is not fine detail
   and must not fade in with `WATER_SHAFT_SHOWN`; the pattern may.
4. Cost is `WATER_SHAFT_STEPS` shadow rays, and only past `WATER_SHAFT_FLOOR` — the gate already
   holds that to pixels where the beam is a real share of what the stretch sends. Measure it and
   write the number down; do not pre-empt it.
5. **What stays wrong:** a stretch below the floor keeps the unshadowed closed form, and the beam is
   under 4 per cent of what the stretch sends there by the gate's own definition. That is the same
   trade the gate already makes for the pattern.
6. Test: a sheet over the water, a camera under it looking along the sun, and the shaft's ratio
   against the same frame with the sheet taken away. The gap belongs where the sheet's shadow falls
   on the water, not under the sheet.

---

## 6. The raft is thresholded against a field whose middle is nearly empty of extremes

### What it is

A thin scatter of white flecks along one depth contour rather than a band of foam.

### Why — measured

**The dissolve does not keep the amount it says it keeps.** `foamCells` is `1 - min(F1 / 0.7, 1)` over
a jittered lattice, and thresholding it at `1 - share` passes nothing like `share` of the area:

| share asked | 0.1 | 0.2 | 0.3 | 0.5 | 0.8 | 1.0 |
| --- | --- | --- | --- | --- | --- | --- |
| area covered | 0.015 | 0.061 | 0.136 | 0.356 | 0.740 | 0.911 |
| as a fraction of it | 0.15 | 0.31 | 0.45 | 0.71 | 0.92 | 0.91 |

At the shares a surf zone produces, the raft delivers a seventh to a half of the white it was told
to. The field also has an **atom of about a tenth of the area at exactly nought** — the points more
than 0.7 of a cell from any centre, which the clamp collapses — so full cover is unreachable at any
share. The comment says "the share sets where the water line across the raft sits, which is what
keeps the amount", and the amount is not kept.

**And the raft is switched off by a question about the wave that made it.** `covered` is
`foamBreaking(...) * foamReaching(...)`, and every term inside `foamBreaking` is instantaneous: the
criterion against this moment's elevation, and `front`, which asks whether the surface is *now*
leaning into its own direction of travel. A raft ten metres shoreward of the breaker line outlived
the wave that made it by seconds, and `front` fluctuates about nought — so about half the surf zone
is switched off at any instant by a test that no longer applies to it.

### The change

1. **Equalise the noise.** Fit the cumulative distribution measured above — it is close to linear
   between a tenth and nine tenths, with the atom at the foot — and apply it, so that thresholding at
   `1 - share` passes `share` of the area. Assert it directly: the mean of the thresholded field
   against the share, swept.
2. **Keep the ramp one-sided at nought.** A ramp centred on the threshold passes the crown of every
   cell at half cover where the share is nothing, which drew a white speck on each cell of the open
   sea to the horizon. The ramp's width has to fall with the share, or the share has to be
   compensated for the ramp's own half-width. The test is a share of nothing covering nothing.
3. **Separate what is breaking from what has broken.** `front` and the instantaneous elevation belong
   to the *generation* term — the bright leading edge of the breaker. The residual raft is a function
   of the still depth and the run-out, which is what `foamReaching` answers. Take the larger of the
   two rather than their product, so the band has a body shoreward of the line and a bright edge at
   it.
4. `WATER_FOAM_LIFETIME` is 3.5 seconds and `foamRunout` already carries it. The residual should fall
   off *over* that run-out rather than stopping at it, which is what makes the shoreward edge fade
   instead of ending.
5. Test on the shelving bed the surf tests already use: the covered share across a transect from deep
   water to the waterline, asserted to rise from nought, hold across the zone, and fade at the
   ground. Today that transect is a spike.
