# Open issues

- A surface lit by a lamp mounted on it is covered in black speckles: the wall behind a
  lantern, the beam a candle hangs from. Reproduce with
  `openmw-rtxtool shot --cell=-2,-9 --pos=-12144,-74148,1001 --look=-11915,-75001,532 --hour=4
  --weather=Foggy --upscale=off`, then look at the wall around the lantern. Present before the fog
  volume and after it, identically.

- A lamp's own fitting occludes it: the lantern's frame, the sconce's bracket, the candle's holder
  all sit between the emitter and everything the lamp lights. `lampVisible` therefore aims its
  shadow ray at the lamp's centre, and a lamp with a size casts no soft edge.
