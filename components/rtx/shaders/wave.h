// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_WAVE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_WAVE_H

#include "portable.h"

// The sea as a set of transformed tiles rather than as a list of sinusoids.
//
// **A sum of plane waves is quasi-periodic, and curvature is where that shows.** The second
// derivative weights a component by `A k²`, so the shortest few own it however the spectrum falls —
// and a handful of plane waves crossing is a lattice, which is what a seabed drew. Measured over
// the sixty-four-component table this replaces, the shortest band carried 40% of the curvature in
// four directions. No allocation of sixty-four fixes that: the fix is thousands of components,
// which is a transform.
//
// **And it is the cheaper of the two.** A tile is one texture fetch where the sum was sixty-four
// sines, and the surface is read twice per water pixel — once for the normal and once for the
// curvature. The synthesis is one pass a frame over a few small grids.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// The largest grid any tile is sampled on, along each axis.
    ///
    /// **Large, because the component count is what this is for.** The widest tile holds fifty
    /// thousand wavevectors inside the spectrum's band against the sixty-four the sinusoid table
    /// carried, and that count is the whole difference between a lattice and water. It also sets
    /// how short a wave the tile can hold — two of its texels, sixteen units, against the thirty-two
    /// the spectrum stops at.
    ///
    /// **A ceiling and not the size**, because a narrower tile needs a smaller grid: the same band
    /// of wavelengths occupies fewer of its cells, so `Rtx::sWaveTiles` gives each tile its own and
    /// no transform runs over a quadrant of nothing.
    RTX_CONST uint WAVE_GRID = 512u;

    /// How many tiles the sea is summed from.
    ///
    /// **Two, and the reason is the tile rather than the band.** Morrowind's sea spans a factor of
    /// thirty-two in wavelength where one grid of this size spans five hundred, so splitting the
    /// spectrum between tiles leaves every grid nearly empty — measured at fifty-six live entries
    /// of sixteen thousand when it was tried. Both tiles carry the *whole* spectrum instead, at
    /// half its variance each: two independent fields of half the energy sum to one field of the
    /// full energy and the same spectrum, and their periods do not divide into one another, so the
    /// sum repeats only at a common multiple nothing looks across.
    RTX_CONST uint WAVE_CASCADES = 2u;

    /// Threads in a transform workgroup, one per butterfly.
    ///
    /// A radix-2 pass over `n` points is `n / 2` butterflies, so the largest grid wants half its own
    /// width. Vulkan promises a thousand and twenty-four threads to a workgroup and thirty-two
    /// kibibytes of shared memory, against the four this holds.
    RTX_CONST uint WAVE_WORKGROUP = WAVE_GRID / 2u;

    /// What one pass of the transform is told.
    ///
    /// **One shader for the rows and the columns**, because a two-dimensional transform is the
    /// one-dimensional one run twice over the same buffer along different strides. Naming the
    /// strides rather than the axis is what lets the second pass be the first with two numbers
    /// swapped.
    ///
    /// **Nothing here says what is being transformed**, and that is the separation the pass is built
    /// on: the field arrives already formed, so six real fields become three complex transforms —
    /// the spectrum of a real field is conjugate-symmetric, so `A + iB` inverse-transforms to
    /// `a + ib` and one pass carries two of them.
    struct WaveConstants
    {
        /// Points along the line being transformed, which is the tile's own grid.
        uint mCount;

        /// How far apart two points of one line are, and how far apart two lines are. `1` and
        /// `mCount` reads the rows, `mCount` and `1` reads the columns.
        uint mStride;
        uint mJump;

        /// Where in the buffer this cascade's grid starts, in complex numbers.
        uint mOffset;
    };

    /// What the pass that forms the spectra is told.
    ///
    /// **One thread a wavevector, and it writes all three pairs.** Every pair is the same `H(k, t)`
    /// times a different power of `ik`, so forming them together reads the amplitude once where
    /// three dispatches would read it three times.
    struct WaveFormConstants
    {
        /// Points along each axis of this tile's grid.
        uint mCount;

        /// How wide the tile is in world units, which turns a grid index into a wavevector.
        float mExtent;

        /// How far the sea has run, in seconds. The whole of what a frame changes.
        float mTime;
    };

    /// What the pass that unpacks the fields is told.
    struct WaveComposeConstants
    {
        /// Points along each axis of this tile's grid.
        uint mCount;
    };

#ifdef RTX_HOST
}
#endif

// What both shading languages read and the host does not, for the reason `RTX_SHADER` gives.
#ifndef RTX_HOST

/// A complex number turned by an angle, which is a multiply by `exp(i angle)`.
///
/// **Shared, because the pass that turns the spectrum and the pass that transforms it both do it.**
/// One is `h0` carried to a time and the other is a butterfly's twiddle, and they are the same four
/// lines — two copies of which are two places for a sign to be wrong.
RTX_SHADER vec2 turnedBy(vec2 value, float angle)
{
    const float sine = sin(angle);
    const float cosine = cos(angle);

    return vec2(value.x * cosine - value.y * sine, value.x * sine + value.y * cosine);
}

#endif

#endif
