#pragma once

#include <cstdint>

#include <osg/Vec3f>

#include "texturedata.hpp"

namespace Rtx
{
    /// The colour of one texel of one level, as it is stored.
    ///
    /// Display-encoded for every content format, because that is what the file holds and what the
    /// sampler would have converted on the way in — `Rtx::toLinear` is what turns it into light.
    ///
    /// **A texel at a time rather than a level decoded first.** Both callers ask for a scattered
    /// few: a thumbnail reads one texel in a few hundred, and a composite reads one per output texel
    /// out of a ground texture it is minifying hard. Decoding the level would be most of the work
    /// for none of the answer.
    ///
    /// `x` and `y` must lie inside `level`, and `level` must be one of the texture's own.
    osg::Vec3f texelAt(const TextureData& texture, const MipLevel& level, std::uint32_t x, std::uint32_t y);
}
