#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Vec4ub>

#include <components/surface/material.hpp>

namespace Rtx
{
    /// sRGB's transfer function, and its inverse clamped to the unit range.
    ///
    /// **Whatever is averaged is averaged between these two.** A weighted sum of stored bytes is not
    /// the encoding of the weighted sum: half of one ground type and half of another meet at 188 in
    /// light and at 128 in bytes, and the second is every blend between two types coming out muddy.
    ///
    /// **One curve for everything the content hands over**, whether it arrives as a texel, as a
    /// weather record's colour or as a lamp's. Every one of them is display-encoded, and a second
    /// spelling of the same three constants is a second idea of what a stored byte is worth.
    ///
    /// **A value that is one of the 256 is answered from the table below whichever overload is
    /// called.** The float one recovers the byte and divides it back before it believes it, so what
    /// it hands back for `k / 255` is the table's own entry and not an approximation of it; a value
    /// that fails that comparison takes the curve. So a caller need not know which kind it holds to
    /// keep `std::pow` off a frame — only to be sure the two answers agree, which is what recovering
    /// the byte exactly is for.
    float toLinear(float encoded);
    float toEncoded(float linear);

    /// The same for a stored byte.
    ///
    /// **The two hundred and fifty-six answers there are, worked out once.** Where a value did
    /// arrive as a stored byte, the argument to the curve above is `k / 255` — so a table over `k`
    /// is not an approximation of that curve, it is the same number for every input it can be
    /// given. A 512-square chain asks two million times, and `std::pow` is a libm call no compiler
    /// inlines.
    ///
    /// **The byte the caller already holds, and so no recovery at all.** This is the overload to
    /// reach for where the type says what the value is; the float one is for a value whose kind the
    /// caller does not know.
    float toLinear(std::uint8_t encoded);

    /// The same over the three channels of a colour, which is how most of them arrive.
    osg::Vec3f toLinear(const osg::Vec3f& encoded);

    /// A colour as the content files store one, decoded.
    ///
    /// Morrowind's colours are display-encoded, and the light transport downstream is linear. The
    /// two differ most in the middle, so mid grey is where a renderer that skips this is most
    /// obviously wrong and where a test pins it.
    ///
    /// **The one crossing, and every colour entering this renderer takes it.** The game states a
    /// colour in four components — a `Vec4f`, four bytes, or a packed word — and the trace holds
    /// three in light, so the narrowing and the decode are one step. Writing that narrowing by hand
    /// is the mistake this exists to stop — a particle's ramp reaching the sprite table in the
    /// space the file wrote it — and `Surface::Colour` gives the description a type a renderer
    /// cannot copy out of.
    osg::Vec3f decodeColour(std::uint32_t packed);

    /// The same decode, for a colour something else has already unpacked to `[0, 1]`.
    ///
    /// **What the game hands over is display-encoded too.** OpenMW's own renderer works in that
    /// space from end to end and never converts, so every colour read off a light, a fog, the sky
    /// or a model's vertex is the file's own number divided by 255 — and a ray tracer that took it
    /// as linear would be as wrong there as it would be reading the record itself. The alpha is
    /// dropped: nothing downstream has a use for it.
    osg::Vec3f decodeColour(const osg::Vec4f& encoded);

    /// The same again, for the four bytes `NifOsg` and `Terrain` write a vertex colour as.
    osg::Vec3f decodeColour(const osg::Vec4ub& encoded);

    /// And for what a content file said a surface is, which carries its space in its type.
    osg::Vec3f decodeColour(const Surface::Colour& encoded);

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
