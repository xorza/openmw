# Open issues

- Ray Reconstruction costs a star a fifth of its peak and most of its edge. The same clear midnight
  at 1920 by 1080 reaches 0.973 of the display range drawn without it and 0.914 through it, and the
  pixel-wide crossfade that gives 245 pixels over half brightness natively gives no gain at all once
  it has been through. The sky's point sources are drawn before an upscaler that is built to remove
  exactly what they are.

- The cloud deck is an emission and never a lit surface. It takes no light from the sun, the moons
  or the sky; it casts no shadow on the world below it; and it loses the sun at the same instant the
  ground does, where a real layer keeps it past the ground's horizon.

- `pathEnd` hands a bounce that hit something the light the open sky delivers, with nothing to say
  how enclosed the point is. The term that stands for the bounces nobody traces cannot darken a hole.
