#pragma once

#include <cstdint>

#include <osg/Vec3f>

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
}
