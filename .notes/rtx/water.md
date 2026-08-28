# Water: five open defects, and what to do about them

Five things in `.notes/ISSUES.md` are about the water. They are not one problem. Two are about the
caustic and share an instrument; one is a hole in the shaft; one is the shape of the foam; and one
turns out to be about the test rather than the shader.

The order below is the order to do them in. It is not severity — it is what each one needs from the
one before it.

| # | Issue | Depends on |
| --- | --- | --- |
| ~~1~~ | ~~The cone test's 0.10 of a mip level~~ | ~~nothing~~ |
| 2 | The caustic does not conserve at a fold of three | nothing |
| 3 | The caustic pattern tears at 66 per cent | 2 |
| 4 | A shaft is not shadowed by what stands over the water | nothing |
| 5 | The surf line is flecks rather than a band | nothing |

---

## ~~1. The cone test measures one sample of a field and compares it against an ensemble~~

**Done. The model is right and the measurement was not.** The test now reads the linear radiance over
an eleven-by-eleven patch of the bed and asserts to 0.02 of a level, where it measures 1.4328 against
a prediction of 1.4255 — a residual of 0.0073, which is the two Jensen terms the comment computes.

### What was measured

`waterTooFineToResolveWidensTheConeItRefractsThrough` reads the centre pixel of a mip ladder and
recovers the level from the byte. Two readings, taken this session:

| | centre pixel | mean of 121 pixels | standard deviation |
| --- | --- | --- | --- |
| still sea | 2.2485 | 2.2478 | 0.0005 |
| ruffled sea | 3.7751 | 3.6755 | 0.1212 |

The difference of the centre pixels is **1.5076** and the model predicts **1.4255** — the 0.082 the
issue names. The difference of the *patch means* is **1.4277**, which is the model to **0.0022**.

The per-pixel standard deviation is 0.12 of a level. The centre pixel sat 0.8 of one away from its
own patch mean, and the residual is that and nothing else.

### Why

`mLostSlope` is `E[|s|^2] - |E[s]|^2` off a mip chain. Both terms are averages over the cone's
footprint, and at this footprint — 66 units against a spectrum that stops at 32 — the cone holds
about four correlation cells of the waves that carry the slope. A mean over four cells is not an
ensemble mean. It has tens of per cent of spread in it, which is 0.12 of a level after the log.

The sRGB byte was ruled out on the way past: reading the linear radiance instead moves the centre
pixel's answer from 1.5076 to 1.5266, which is *away* from the prediction. The ladder's decode is
linear in the level, as `makeMipLadder` says, and the hypothesis in the issue is wrong.

### The change

1. Read `mRadiance` rather than `decodeSrgb(pixels[...])`. Six tests in the file already do, and the
   quantisation is 0.08 of a level a byte — the same size as the thing being measured.
2. Average the recovered *cone width* over a square of pixels wholly inside the bed, and take the
   log of the mean rather than the mean of the logs. Eleven by eleven is inside the 15-pixel bed and
   is what the numbers above were taken on.
3. Tighten the tolerance from 0.1 to 0.02. At 0.1 the test cannot tell the factor of two on the lobe
   from an error a fifth that size, and the whole of what it exists to pin is that factor.
4. Delete the two paragraphs of the comment that account for the residual, and the sentence in
   `lobeOf` that says the two "agreed only to 0.088 of a mip level". They describe a measurement
   error as a modelling one.
5. Take the entry out of `.notes/ISSUES.md`.

**Measured after the change:** the still sea reads level 2.25 and the ruffled 3.68, the residual is
0.0073 where the tolerance is 0.02, and adding the lobe once instead of twice would now stand
twenty-five tolerances away rather than seven.

---

## 2. The caustic does not conserve, and the correction is a series where no series converges

### What it is

The bed two metres down receives 12 per cent less light than falls on the water, and twenty metres
down 2 per cent more. `theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt` allows 13 per cent at
two metres and 3 per cent past it, which is a tolerance shaped around the failure.

### Why

`gathered` is `1 / (|det(I - bH)| * (1 + JENSEN * b^2 * traceVariance))`. The divisor is the second
order of `E[1/det]` in `u = b * tr(H)`, and `WATER_CAUSTIC_FOLD` of three means `u` has an rms of
three. A second-order expansion in a quantity of order three is not an approximation of anything.

Two separate things then go wrong at the two ends of the depth range, which is why one coefficient
cannot fit both:

- **Shallow.** The correction is at full strength — at two metres the read lands on level 0 of both
  tiles, so `traceVariance` is the whole of the tile's variance and the divisor is exactly
  `1 + 0.03 * 9 = 1.27`. The uncorrected mean is 1.118, so the correction overshoots by 14 per cent.
  Fitting `WATER_CAUSTIC_JENSEN` to two metres alone wants 0.013.
- **Deep.** `traceVariance` goes to nought as the cone reaches the coarse levels — that is what it is
  built to do — so the correction switches itself off. The mean is then 1.018 through a fade of
  0.071, which puts the uncorrected estimator at about 1.25. The reference implementation found the
  same thing and called it by name: past the first focus the map has folded, one branch is being read
  as though it were the whole density, and a single-branch reciprocal does not average to one. It
  capped the depth at 140 units to hold the error under 6 per cent.

### The change

**Replace the fitted second order with the exact expectation, as a function of one dimensionless
number.**

For an isotropic Gaussian height field the Hessian is Gaussian and its three entries have one free
parameter: `Var[Hxx] = Var[Hyy] = 3c`, `Var[Hxy] = c`, `Cov[Hxx, Hyy] = c`, so `E[(tr H)^2] = 8c`.
Everything about the estimator's mean is then set by the **resolved fold**

    f = b * sqrt(traceVariance)

which the shader already has both halves of, and by `WATER_CAUSTIC_MAX`, which is a constant. So

    E[1 / max(|det(I - b H)|, 1 / MAX)]

is a curve in one variable. Steps:

1. Write the Monte Carlo on the host: draw the Hessian from the covariance above, evaluate the
   clipped reciprocal, and tabulate the mean against `f` from nought to about six. This is a test
   file, not a shipped table.
2. Fit it — a rational in `f^2` is the shape, because the series it replaces is its own first term —
   and put the fit in `sea.glsl` as `causticGain(f)`, with the Monte Carlo asserting the fit.
3. Divide by `causticGain(f)` in place of `1 + WATER_CAUSTIC_JENSEN * f^2`, and delete
   `WATER_CAUSTIC_JENSEN`. Conservation then holds at every depth by construction, and
   `WATER_CAUSTIC_MAX` and `WATER_CAUSTIC_FOLD` stop having to move together — which is the standing
   trap in both of their comments.
4. Before any of it, instrument: report `E[raw]`, `E[divisor]` and the level read, at one, two, six,
   twenty and forty metres. The claim above about the deep end is arithmetic done backwards out of
   one mean and it should be measured, the way item 1 was.
5. Tighten `theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt` to 2 per cent at every depth.

**What this does not fix.** Where the determinant is negative the point is reached by three patches
of surface and this draws one of them. Normalising absorbs the mean of that and not its shape, so
the pattern past the fold is still one branch of three. Reading all three is a redesign — the fold's
own preimages have to be found, which is a root-finding problem per pixel — and it is not this.

---

## 3. The caustic tears because the fold weights the spectrum toward its fast end

### What it is

66.4 per cent of the pattern is new a twelfth of a second later. The sweep `sShortestWave` was
chosen on put tearing at half, and the leg that reshuffled 73 per cent "read as stripes running
across the bottom".

### Why

Curvature weights a wave by `A k^2`, so the Hessian is owned by the short end of the spectrum
whatever the spectrum does. The fold is a nonlinearity applied to that Hessian, and it puts the
contrast into thin filaments — the finest structure in the field, made of the fastest-turning waves
there are. A wave of 32 units, which is where `sShortestWave` cuts, turns at
`sqrt(2 pi g / lambda)` = 11.1 rad/s, so it decorrelates almost exactly over the twelfth of a second
being measured. The fold did not add short waves; it moved the contrast onto the ones already there.

The lever the test comment names is a lower `WATER_CAUSTIC_FOLD`, and that is the one the tuning
already rejected: it is also what makes the bright lines thicker.

### The change

**Give the lens its own band limit, separate from the pixel's.**

`widened` serves two masters today: it is the anti-aliasing limit *and* the choice of which waves
make the lens. At two metres it comes out at 9.5 units, which passes a 32-unit wave at 0.86 of its
amplitude. A floor of 27 units passes the same wave at 0.18.

1. Add `WATER_CAUSTIC_COARSEN`, a floor on `widened` in world units, and derive it from the
   coherence the frame can hold: a wave is worth drawing when `omega * dt` is under a radian, which
   is `lambda >= 2 pi g (dt / theta)^2` — 27 units at a twelfth of a second. State it as that
   arithmetic and not as a number.
2. Sweep `(coarsen, fold)` against the two figures the existing test already measures — the
   contrast, and the share of the pattern that is new. The claim to check is that a coarser lens at a
   higher fold reaches the same contrast at a lower reshuffle, because the fold is a nonlinearity and
   can put back what the filter took.
3. **Item 2 first.** Raising the fold to buy contrast back is exactly what does not conserve today,
   so this sweep is only affordable once the normaliser is exact.
4. If the sweep finds nothing, the honest answer is that contrast and coherence are one dial on this
   spectrum, and the choice goes to the user with both numbers beside it.

---

## 4. A shaft is not shadowed by what stands over the water

### What it is

A rock over the surface casts no gap in an underwater sun shaft.

### Why

`waterColumn` never asks whether the sun reaches the point it is scattering at. A submerged
*surface* is shadowed, because `shadeSurface` traces its own ray and water carries a mask bit that
keeps it out of occlusion. The volume has no such ray: the closed form is an integral of the sun's
own line with nothing on it, and the march multiplies in `caustic` and nothing else.

### The change

The march is already a ratio estimator — the same integrand twice, once with the surface's lens and
once without — so visibility belongs in the numerator beside the lens, and a stretch with nothing
over it comes back as exactly the closed form.

1. At each step, the sun met the surface at `at.xy - sun.mTravelling.xy * reach`, at the water level.
   The march has that point already; it is what `caustic` is read at.
2. Multiply the step's contribution by `lightThrough(entry, frame.mSunPosition, frame.mFar)` —
   `MASK_SOLID`, so the water does not shadow itself, and the same helper every lamp uses.
3. **Outside the `mix` that fades the pattern in, not inside it.** A rock's shadow is not fine
   detail and must not fade in with `WATER_SHAFT_SHOWN`; the pattern may.
4. Cost is `WATER_SHAFT_STEPS` shadow rays, and only on pixels past `WATER_SHAFT_FLOOR` — the gate
   already keeps that to the pixels where the beam is a real share of what the stretch sends. Measure
   it and write the number down; do not pre-empt it.
5. **What stays wrong:** a stretch below the floor keeps the unshadowed closed form. The beam is
   under 4 per cent of what the stretch sends there by the gate's own definition, so the error is
   bounded by that, and it is the same trade the gate already makes for the pattern.
6. Test: a sheet over the water, a camera under it looking along the sun, and the shaft's own ratio
   against the same frame with the sheet taken away. The gap has to be where the sheet's shadow
   falls on the water and not where the sheet is.

---

## 5. The surf line is flecks because the raft is thresholded against a field that is not uniform

### What it is

A thin scatter of white along one depth contour rather than a band of foam.

### Why

Two things, and they compound.

**The dissolve does not keep the amount it claims to.** `foamCells` returns `1 - min(F1 / 0.7, 1)`
over a jittered lattice, and that is not uniform on nought to one: most of a cell is far from its
own centre, so high values are rare. `smoothstep(1 - share, 1 - share + DISSOLVE, cells)` at a share
of a third therefore passes far less than a third of the area — only the crowns. The comment says
"the share sets where the water line across the raft sits, which is what keeps the amount", and the
amount is not kept.

**The raft is switched off by a test about the wave that made it.** `covered` is
`foamBreaking(...) * foamReaching(...)`, and every term inside `foamBreaking` is instantaneous: the
criterion against this moment's elevation, and `front`, which asks whether the surface is *now*
leaning into its own direction of travel. A raft floating ten metres shoreward of the breaker line
outlived the wave that made it by seconds, and `front` fluctuates about nought, so about half of the
surf zone is switched off at any instant by a question that no longer applies to it.

### The change

1. **Make the noise uniform.** Fit the F1 cumulative distribution — one Monte Carlo over the same
   lattice `foamCells` uses — and apply it, so that thresholding at `1 - share` passes exactly
   `share` of the area. Then a share of a third is a third of the pixel white, and the dissolve gives
   the shape without taking the amount. Assert it: the mean of the thresholded field against the
   share, over a sweep of shares.
2. **Separate what is breaking from what has broken.** `front` and the instantaneous elevation belong
   to the *generation* term — the bright leading edge of the breaker. The residual raft is a
   function of the still depth and the run-out, which is what `foamReaching` already answers. Take
   the larger of the two rather than their product, so the band has a body shoreward of the line and
   a bright edge at it.
3. `WATER_FOAM_LIFETIME` is 3.5 seconds and `foamRunout` already carries it. The residual term should
   fall off over that run-out rather than stopping at it, which is what makes the shoreward edge of
   the band fade instead of ending.
4. Test on the shelving bed the surf tests already use: the covered share across a transect from deep
   water to the waterline, asserted to rise from nought, hold across the zone, and fade at the
   ground. Today that transect is a spike.

---

## What is deliberately not here

- **Reading all three branches past the fold.** Item 2 normalises the mean of a single-branch
  density. Drawing the other two means finding the fold's preimages per pixel, and that is a
  different renderer.
- **Anything about the cost.** *Feature-complete first, then fast.* Item 4 adds rays and item 5 adds
  a fit; both get measured and written down, and neither gets optimised before the picture is right.
