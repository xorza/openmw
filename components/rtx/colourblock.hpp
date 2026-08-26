#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <osg/Vec3f>

namespace Rtx
{
    /// The colour half of one block-compressed block: four colours and the texels that chose them.
    ///
    /// **Eight bytes, and every block-compressed format this renderer reads ends in them.** BC1 is
    /// these alone; BC2 and BC3 put eight bytes of alpha in front and leave this unchanged. So one
    /// reader serves all three, and anything that wants a block's colours — an average to divide
    /// out, a thumbnail to look at — asks it rather than carrying its own copy of the rule.
    ///
    /// The colours are as stored, which is display-encoded for every content format: whoever wants
    /// linear light converts, and whoever wants to write a PNG does not.
    struct ColourBlock
    {
        /// In index order. The fourth is meaningless where `mCutout` is set.
        std::array<osg::Vec3f, 4> mPalette;

        /// Sixteen two-bit indices, the first texel in the lowest bits.
        std::uint32_t mIndices = 0;

        /// Whether the fourth entry is transparent rather than a colour.
        ///
        /// BC1 spells that by storing its endpoints in ascending order, which costs it the fourth
        /// palette entry. BC2 and BC3 carry alpha of their own and never do.
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
