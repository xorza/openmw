# Open issues

- The sky's point sources are drawn before the upscaler. On a clear midnight at 1920 by 1080 the
  pixels over half brightness go 1093 without it, 698 at DLAA, 347 at quality and 258 at performance
  — a third to the network at the same internal resolution and the rest to the resolution itself.
  No guide buffer moves it: an eye-facing normal, the bias mask over every sky pixel and the four
  colour pairs all measure neutral or worse.

- The cloud deck is an emission and never a lit surface. It takes no light from the sun, the moons
  or the sky; it casts no shadow on the world below it; and it loses the sun at the same instant the
  ground does, where a real layer keeps it past the ground's horizon.

- `pathEnd` hands a bounce that hit something the light the open sky delivers, with nothing to say
  how enclosed the point is. The term that stands for the bounces nobody traces cannot darken a hole.
