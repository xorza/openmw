# Open issues

- Stars are dim and soft. Morrowind hands a star texel to an alpha blend that saturates at the
  display ceiling, so its bilinear spread clips into a hard white dot; nothing here clips, so the
  same spread shows and the level never reaches the top of the curve. Measured at the zenith on a
  clear midnight, the brightest pixel is 0.584 with the upscaler off and 0.449 with it on.

- The cloud deck is an emission and never a lit surface. It takes no light from the sun, the moons
  or the sky; it casts no shadow on the world below it; and it loses the sun at the same instant the
  ground does, where a real layer keeps it past the ground's horizon.

- `pathEnd` hands a bounce that hit something the light the open sky delivers, with nothing to say
  how enclosed the point is. The term that stands for the bounces nobody traces cannot darken a hole.
