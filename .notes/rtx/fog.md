# Fog integration

The froxel grid stores coverage, lamp irradiance, and visibility. Coverage and visibility are
sampled and filtered; lamp irradiance is integrated over each froxel without temporal filtering.
The exponential height profile and the water boundary are evaluated on the pixel's ray. The grid
contains no accumulated transmittance or in-scattering to interpolate across different rays.

This distinction matters at the horizon and at water. Averaging densities across a boundary
changes the medium. Averaging exp(-optical depth) across rays also does not give the transmittance
of their mean ray. The same error affects the sun's attenuation through an exponential atmosphere.

Sucker Punch's [density antialiasing derivation](https://www.advances.realtimerendering.com/s2021/jpatry_advances2021/index.html#/96)
factors opacity out of accumulated scattering and reapplies it per pixel, assuming incoming light
is nearly constant along the path. Here sunlight attenuation and local lamps vary along the path,
so integration retains that dependence. The
[volume rendering equation](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/The_Equation_of_Transfer)
weights each source by view-path transmittance as well as the source's own attenuation.

## Closed form inside an interval

Let the base extinction be sigma, the scale height H, and height relative to the base be z.
Density is sigma * exp(-max(z, 0) / H); a wet cell has no fog below zero. Intervals are clipped at
the water plane before evaluating their lighting. A horizontal ray exactly on the plane is in air.

`fogLayerDepth` integrates this profile, splitting at the base when necessary. With constant
coverage over an interval, its result is the interval's optical depth tau. For an upward light
with vertical component lz, the optical depth from height z to the sky is

    b(z) = sigma * (H * exp(-max(z, 0) / H) + max(-z, 0)) / lz.

The second term is the constant-density stretch below the base in a dry cell. It is zero for any
air sample in a wet cell. Along a view ray with vertical component vz, use view optical depth t
as the integration coordinate. Then

    b(t) = b(0) - (vz / lz) * t
    J = integral[0, tau] exp(-t - b(t)) dt
      = exp(-b(0)) * tau * E((1 - vz/lz) * tau)
    E(x) = (1 - exp(-x)) / x, E(0) = 1.

The implementation factors out the smaller endpoint exponent and evaluates E at a nonnegative
argument, avoiding overflow when vz/lz exceeds one. A Taylor expansion near zero avoids subtractive
cancellation. Multiplying J by incoming irradiance, phase per steradian, and visibility gives the
directional in-scattering. The ambient and lamp term uses 1 - exp(-tau).

The formula is exact for constant coverage and visibility, including horizontal rays, rays aligned
with the light, and intervals crossing the dry base. Its implementation is shared by C++ tests and
GLSL. Each pixel integrates two intervals per depth slice, with coverage and lighting sampled at
the interval midpoint. No field evaluation, light-grid traversal, or shadow ray is repeated per pixel.

The remaining approximations are the froxel representation of coverage, visibility and lamp
irradiance, and the locally constant coverage used for the light's atmospheric column. A bank's
full density field is not marched toward each directional light. Per-pixel integration costs more
texture reads and arithmetic than fetching an accumulated column; it requires fewer volume images
and keeps the known medium independent of grid resolution.
