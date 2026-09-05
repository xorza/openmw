# Open issues

- Sprites ignore a particle's own angle. `Weather::RainShooter` leans every drop into the wind
  through `Particle::setAngle`, and a NIF's `NiParticleRotation` turns its particles the same way;
  the rasterizer rotates each quad by `Particle::getAngle` before it draws it, and
  `Rtx::EmitterResolver` reads only the system's align vectors, so a ray-traced drop hangs vertical
  in a wind the rasterizer leans it into.
