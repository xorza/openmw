# Open issues

- A local-map tile traces the world's sprites — rain, smoke, magic effects — over the ground. The
  mask `LocalMap` hands `OffscreenViewSpec` excludes `Mask_ParticleSystem` and `Mask_Effect`, which
  is what the rasterizer's map camera culls, and `OffscreenViewSpec::mMask` says the field is
  ignored for a picture of the world.
