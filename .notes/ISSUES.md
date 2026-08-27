# Open issues

- `starField` and `skyPatches` return the night sky's texels as radiance, with no scale
  (`components/rtxvulkan/shaders/lib/sky.glsl`). Every other emitter in the renderer has one —
  `MOON_RADIANCE` takes a moon's portrait to 0.18, `DAYLIGHT` the sun, `MOONLIGHT` the moons as
  lights — so a nebula texel near one is drawn five times brighter than a full moon's disc. Scaling
  both by a fiftieth moves the night sky's mean from 0.183 to 0.161.
