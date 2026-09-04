# Open issues

- `runInfo` in `apps/rtxtool/main.cpp` builds its `Rtx::RendererOptions` with a designated
  initializer list that skips `mCacheDirectory`, so every build of the harness warns
  `missing initializer for member 'Rtx::RendererOptions::mCacheDirectory'`.

- The fog volume disagrees with the analytic answer for an even layer in two places the closed form
  it replaced did not. A ray lying exactly on the water surface reads up to thirteen levels apart
  between a dry cell and a cell whose water is at nought. And a level ray toward a sun a quarter of
  the way up reads the sun's in-scattering a fifth over the closed form's
  `irradiance * phase * column * crossed`: 0.597 against 0.500. Both are in
  `apps/components_tests/rtx/visibility/fog.cpp` with their tolerances widened to say so, and both
  were already so for every exterior.
