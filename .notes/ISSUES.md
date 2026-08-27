# Open issues

- The cloud deck is an emission and never a lit surface. It takes no light from the sun, the moons
  or the sky; it casts no shadow on the world below it; and it loses the sun at the same instant the
  ground does, where a real layer keeps it past the ground's horizon.

- The deck's horizon fade is a constant this renderer chose. The engine fades the same band by
  painting its cloud mesh's bottom two rings of vertices at a quarter alpha and none, which puts its
  own edge at 4.9 degrees of elevation and full cover at 27.4.

- The night sky's sheets light nothing. `skyGlow` returns the dome's gradient alone, so the star
  field and the six painted patches are drawn and never gathered — measured off the shipped files
  they are worth a further 13% of a clear night's sky.

- `pathEnd` hands a bounce that hit something the light the open sky delivers, with nothing to say
  how enclosed the point is. The term that stands for the bounces nobody traces cannot darken a hole.
