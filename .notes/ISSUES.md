# Open issues

- `starField` and `skyPatches` return the night sky's texels as radiance, with no scale
  (`components/rtxvulkan/shaders/lib/sky.glsl`). Every other emitter in the renderer has one —
  `MOON_RADIANCE` takes a moon's portrait to 0.18, `DAYLIGHT` the sun, `MOONLIGHT` the moons as
  lights — so a nebula texel near one is drawn five times brighter than a full moon's disc.

- The surf takes `pathEnd` as its indirect light (`components/rtxvulkan/shaders/lib/water.glsl`),
  where every other surface the eye can see traces a hemisphere. That term stands in for the rest of
  a path one level down, and at a primary surface it is the cell's whole ambient with no cosine and
  no hemisphere over it. Measured at night it is 1.5 times what a traced bounce gives.
