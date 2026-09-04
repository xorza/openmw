// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_BLOOM_H
#define OPENMW_COMPONENTS_RTX_SHADERS_BLOOM_H

#include "portable.h"

// What the lens does with the light the frame already has. Included verbatim by both sides, for the
// reason `visibility.h` is.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec2f>

namespace Rtx::Shaders
{
    using vec2 = osg::Vec2f;
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of a bloom workgroup.
    const uint BLOOM_WORKGROUP = 8;

    /// How many halvings the pyramid is built over.
    ///
    /// **A count and not a stopping size, so the spread is the same picture at every resolution.**
    /// A pyramid taken down to a fixed number of texels is a wider blur on a bigger frame, and this
    /// renderer is looked at through a harness that renders a tenth of the pixels the game does.
    /// Six halvings put the coarsest level at a sixty-fourth of the frame's width, which is where
    /// the widest tap of the widest tent sits.
    const uint BLOOM_LEVELS = 6;

    /// How narrow a level may be before it is not worth building.
    ///
    /// A tent reads its own neighbours, so a level thinner than this is mostly its own edge clamp.
    /// Only a frame far smaller than anything played on reaches it.
    const uint BLOOM_NARROWEST = 4;

    /// How much of each coarser level survives into the one above it.
    ///
    /// **The pyramid is mixed rather than summed**, which is what keeps the total independent of
    /// how many levels there are: `mix(finer, coarser, this)` at every step, so a frame that built
    /// one level fewer is a narrower bloom and not a dimmer one. Higher is a wider, softer veil.
    const float BLOOM_SCATTER = 0.75f;

    /// How much of the pyramid is left in the picture.
    ///
    /// **No threshold anywhere, which is why this is small.** A lens spreads every photon that
    /// reaches it and not only the bright ones, so the whole frame is blurred and mixed back at a
    /// few per cent — where a threshold makes bloom arrive as an object crosses a brightness nobody
    /// can see, and takes the veil off everything under it. What makes a Morrowind sun read as a
    /// sun is that its disc is a hundred times the median of the frame around it, not that anything
    /// selected it.
    const float BLOOM_STRENGTH = 0.05f;

    /// What one dispatch of the pyramid is told.
    struct BloomConstants
    {
        /// The extent being written, which is the level this dispatch fills and not the one it
        /// reads.
        uint mWidth;
        uint mHeight;

        /// One texel of the image being *sampled*, in that image's own texture coordinates.
        ///
        /// **The source's and not the destination's**, because both kernels are written in taps of
        /// the image they read: the thirteen-tap downsample reaches two source texels out and the
        /// nine-tap tent reaches one, and each is a fixed shape in the source's grid whatever the
        /// destination's is.
        vec2 mTexel;

        /// How much of what was sampled replaces what the destination already holds.
        ///
        /// `BLOOM_SCATTER` between two levels of the pyramid, `BLOOM_STRENGTH` where the pyramid
        /// reaches the picture, and unread by the downsample, which overwrites.
        float mMix;
    };

#ifdef RTX_HOST
}
#endif

// The pyramid's own format, as a macro for the reason `gbuffer.h` gives.
//
// **Half floats, where the frame is whole ones.** What the pyramid carries is a blurred copy mixed
// back at a twentieth, so a step of one part in a thousand of it is one part in twenty thousand of
// the picture — and the levels are read and written far more often than anything else in the frame,
// which makes their bandwidth the only thing about them that costs.
#ifdef RTX_HOST
#define BLOOM_LEVEL VK_FORMAT_R16G16B16A16_SFLOAT
#else
#define BLOOM_LEVEL rgba16f
#endif

#endif
