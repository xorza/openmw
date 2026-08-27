# Open issues

- The cloud deck is an emission and never a lit surface. It takes no light from the sun, the moons
  or the sky; it casts no shadow on the world below it; and it loses the sun at the same instant the
  ground does, where a real layer keeps it past the ground's horizon.

- The deck's horizon fade is a constant this renderer chose. The engine fades the same band by
  painting its cloud mesh's bottom two rings of vertices at a quarter alpha and none, which puts its
  own edge at 4.9 degrees of elevation and full cover at 28.8.

- The night sky's sheets light nothing. `skyGlow` returns the dome's gradient alone, so the star
  field and the six painted patches are drawn and never gathered — measured off the shipped files
  they are worth a further 13% of a clear night's sky.

- `mAmbient` at night is larger than the sky it stands for a bounce of: 0.0168 against the dome's
  0.0030 for a clear night. A surface in a crevice is therefore lit more than the open ground beside
  it.

- Secunda appears at about 22:30 already high in the sky rather than rising at the horizon.
