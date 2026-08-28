# Open issues

- An underwater sun shaft is not blocked by geometry above the water. A rock over the surface
  casts no gap in it.

- The surf line draws as a thin scatter of white flecks along one depth contour rather than as a
  band of foam.

- The seabed's caustic pattern reshuffles 62 per cent of itself in a twelfth of a second, where the
  sweep `sShortestWave` was chosen on put tearing at half. It should read as stripes running across
  the bottom rather than as water.

- The caustic makes 4 per cent of light at two and at six metres. `causticGain` is fitted to the
  Hessian of an isotropic Gaussian field, whose mean peaks at 1.294, and the estimator's own
  conditional mean reaches 1.32 — so no argument to the curve divides that away. Saying what the sea
  actually does there would take a second directional moment the wave tiles do not carry.

