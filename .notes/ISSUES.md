# Open issues

- Stars shine through the moons. `skyRadiance` adds the star field and the patches before the moons
  and then dims the sun alone by what they cover, so a moon takes its share of nothing else. The
  rasterizer draws the night sky first and lets an opaque moon replace it.

- A moon is not drawn at all until it stands `Moons_<name>_Fade_End_Angle` above the horizon — 30
  degrees for Secunda and 40 for Masser — and shows no face until the same angle. Neither of them
  rises out of the horizon.

- The cloud deck is an emission and never a lit surface. It takes no light from the sun, the moons
  or the sky; it casts no shadow on the world below it; and it loses the sun at the same instant the
  ground does, where a real layer keeps it past the ground's horizon.

- The night sky's sheets light nothing. `skyGlow` returns the dome's gradient alone, so the star
  field and the six painted patches are drawn and never gathered — measured off the shipped files
  they are worth a further 13% of a clear night's sky.

- `pathEnd` hands a bounce that hit something the light the open sky delivers, with nothing to say
  how enclosed the point is. The term that stands for the bounces nobody traces cannot darken a hole.
