#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Vec4ub>

#include "surface.hpp"

namespace Rtx
{
    /// sRGB's transfer function, and its inverse clamped to the unit range. Whatever is averaged is
    /// averaged between these two: half of one ground type and half of another meet at 188 in light
    /// and at 128 in bytes. One curve for everything the content hands over — a texel, a weather's
    /// colour, a lamp's. A value that is one of the 256 is answered from the table below whichever
    /// overload is called: the float one recovers the byte first, so the two answers agree.
    float toLinear(float encoded);
    float toEncoded(float linear);

    /// The same for a stored byte: the two hundred and fifty-six answers there are, worked out
    /// once, because a 512-square chain asks two million times and `std::pow` is a libm call.
    float toLinear(std::uint8_t encoded);

    /// The same over the three channels of a colour, which is how most of them arrive.
    osg::Vec3f toLinear(const osg::Vec3f& encoded);

    /// A colour as the content files store one, decoded — the one crossing, and every colour
    /// entering this renderer takes it: the game states four components and the trace holds three
    /// in light, so the narrowing and the decode are one step, and a particle's ramp cannot reach
    /// the sprite table in the space the file wrote it.
    osg::Vec3f decodeColour(std::uint32_t packed);

    /// The same decode, for a colour something else has already unpacked to `[0, 1]`. What the
    /// game hands over is display-encoded too: OpenMW's own renderer never converts. The alpha is
    /// dropped.
    osg::Vec3f decodeColour(const osg::Vec4f& encoded);

    /// The same again, for the four bytes `NifOsg` and `Terrain` write a vertex colour as.
    osg::Vec3f decodeColour(const osg::Vec4ub& encoded);

    /// And for what a content file said a surface is, which carries its space in its type.
    osg::Vec3f decodeColour(const EncodedColour& encoded);

    /// The colour half of one block-compressed block: four colours and the texels that chose them.
    /// Eight bytes that every block-compressed format this renderer reads ends in — BC2 and BC3 put
    /// eight bytes of alpha in front — so one reader serves all three. The colours are as stored,
    /// display-encoded.
    struct ColourBlock
    {
        /// In index order. The fourth is meaningless where `mCutout` is set.
        std::array<osg::Vec3f, 4> mPalette;

        /// Sixteen two-bit indices, the first texel in the lowest bits.
        std::uint32_t mIndices = 0;

        /// Whether the fourth entry is transparent rather than a colour. BC1 spells that by storing
        /// its endpoints in ascending order, which costs it the fourth palette entry. BC2 and BC3
        /// carry alpha of their own and never do.
        bool mCutout = false;

        /// @param punchThrough whether the ascending spelling means transparency, which is BC1's
        ///        alone.
        static ColourBlock read(std::span<const std::byte, 8> bytes, bool punchThrough);

        /// Which of the four a texel chose, counting along rows from the top left.
        std::uint32_t indexAt(std::size_t texel) const { return mIndices >> (texel * 2) & 0x3u; }

        /// Whether a texel is the transparent entry, and so is not a colour at all.
        bool isTransparent(std::size_t texel) const { return mCutout && indexAt(texel) == 3; }
    };
}
