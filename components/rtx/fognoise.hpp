#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Rtx
{
    /// The fog's fractal field, drawn once into a wrapping tile with a chain of levels under it.
    ///
    /// **A field a sampler reads rather than one a march computes.** Value noise off a hashed
    /// lattice costs a hash per corner, so one octave is eight of them and the stack the fog wants is
    /// forty — paid at every step of a twenty-four step march, at every pixel. That was measured at
    /// 2.0 ms of a 2.1 ms trace and it is why an interior branches around the field entirely. A
    /// field drawn once costs one fetch, and the hardware does the interpolation.
    ///
    /// **What that buys is a better field and not only a cheaper one.** Nothing here is on the frame
    /// path, so the octaves are gradient noise rather than value noise — no lattice showing as a grid
    /// of creases — and there are as many of them as the tile can hold rather than as many as a
    /// march can afford.
    struct FogNoise
    {
        /// Every level end to end, the full one first, two channels a texel.
        std::vector<std::uint8_t> mBytes;

        /// Where each level begins in `mBytes`.
        std::vector<std::size_t> mOffsets;
    };

    /// Draws the tile.
    ///
    /// **Every level is normalised to one mean and one spread**, which is what lets the coverage band
    /// be cut against the field rather than against whichever level a step happened to reach. A level
    /// is the mean of the eight texels over it, so its spread narrows as the chain goes up; stretched
    /// back about the mean, what a coarse step loses is the detail and never the amount of air. That
    /// is the argument `resolved` makes for a wave against a ray cone, made once here instead of at
    /// every step of every march.
    FogNoise bakeFogNoise();
}
