#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Rtx
{
    /// The fog's fractal field, drawn once into a wrapping volume with a chain of levels under it.
    ///
    /// **A field a sampler reads rather than one a march computes.** Value noise off a hashed
    /// lattice costs a hash per corner, so one octave is eight of them and the stack the fog wants is
    /// forty — paid at every step of a twenty-four step march, at every pixel. That was measured at
    /// 2.0 ms of a 2.1 ms trace and it is why an interior branches around the field entirely. A
    /// field drawn once costs one fetch, and the hardware does the interpolation.
    ///
    /// **The same noise the renderer this is ported from draws with, and nothing cleverer.** Its
    /// `fog_noise` is trilinear value noise off a hashed lattice, and what its fog looks like is
    /// mostly what that looks like — so this is that, baked, wrapping on all three axes so nothing
    /// has to be tiled by hand.
    ///
    /// **One octave, and the fractal is the shader's.** `fogShape` reads this at three scales that
    /// never come back into step, which is the same construction the reference makes over its
    /// hash; a stack of octaves baked in as well would only add a second set of lattice lines under
    /// the first.
    struct FogNoise
    {
        /// Every level end to end, the full one first, two channels a texel, slice by slice.
        std::vector<std::uint8_t> mBytes;

        /// Where each level begins in `mBytes`.
        std::vector<std::size_t> mOffsets;
    };

    /// Draws the tile.
    ///
    /// **Every level is normalised to one mean and one spread**, which is what lets the coverage band
    /// be cut against the field rather than against whichever level a step happened to reach. A level
    /// is the mean of the eight texels over it, so its spread narrows as the chain goes up. Stretched
    /// back about the mean — as a sampler reads it, and not as its texels hold it — what a coarse
    /// step loses is the detail and never the amount of air. That is the argument `resolved` makes
    /// for a wave against a ray cone, made once here instead of at every step of every march.
    FogNoise bakeFogNoise();
}
