#pragma once

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
    float toLinear(float encoded);
    float toEncoded(float linear);

    /// The same over the three channels of a colour, which is how most of them arrive.
    osg::Vec3f toLinear(const osg::Vec3f& encoded);
}
