#pragma once

#include <cstdint>

#include <osg/Vec3f>

#include "texturedata.hpp"

namespace Rtx
{
    /// The colour of one texel of one level, as it is stored.
    ///
    /// Display-encoded for every content format, because that is what the file holds and what the
    /// sampler would have converted on the way in — `toLinear` is what turns it into light.
    ///
    /// **A texel at a time rather than a level decoded first.** Both callers ask for a scattered
    /// few: a thumbnail reads one texel in a few hundred, and a composite reads one per output texel
    /// out of a ground texture it is minifying hard. Decoding the level would be most of the work
    /// for none of the answer.
    ///
    /// `x` and `y` must lie inside `level`, and `level` must be one of the texture's own.
    osg::Vec3f texelAt(const TextureData& texture, const MipLevel& level, std::uint32_t x, std::uint32_t y);

    /// sRGB's transfer function, and its inverse clamped to the unit range.
    ///
    /// **Whatever is averaged is averaged between these two.** A weighted sum of stored bytes is not
    /// the encoding of the weighted sum: half of one ground type and half of another meet at 188 in
    /// light and at 128 in bytes, and the second is every blend between two types coming out muddy.
    float toLinear(float encoded);
    float toEncoded(float linear);
}
