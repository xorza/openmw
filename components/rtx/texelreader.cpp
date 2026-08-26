#include "texelreader.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "colourblock.hpp"

namespace Rtx
{
    osg::Vec3f texelAt(const TextureData& texture, const MipLevel& level, std::uint32_t x, std::uint32_t y)
    {
        assert(x < level.mWidth && y < level.mHeight);

        const std::uint32_t bytes = blockBytes(texture.mFormat);
        if (bytes == 0)
        {
            const std::size_t at = level.mOffset + (std::size_t{ y } * level.mWidth + x) * 4;
            const auto channel = [&](std::size_t offset) {
                return std::to_integer<std::uint32_t>(texture.mBytes[at + offset]) / 255.0f;
            };

            // The two loose spellings differ only in which end the three colours are stated from,
            // and a reader that took one order for both draws the sky with its red and blue swapped.
            if (texture.mFormat == TextureFormat::Bgra8Srgb)
                return osg::Vec3f(channel(2), channel(1), channel(0));

            return osg::Vec3f(channel(0), channel(1), channel(2));
        }

        // The colour half is the last eight bytes of a block whichever format it is: BC2 and BC3 put
        // their alpha in front of it and BC1 has none.
        const std::uint32_t columns = (level.mWidth + 3) / 4;
        const std::size_t at = level.mOffset + (std::size_t{ y / 4 } * columns + x / 4) * bytes + (bytes - 8);
        const ColourBlock block
            = ColourBlock::read(texture.mBytes.subspan(at).first<8>(), texture.mFormat == TextureFormat::Bc1RgbaSrgb);

        return block.mPalette[block.indexAt(std::size_t{ y % 4 } * 4 + x % 4)];
    }

    float toLinear(float encoded)
    {
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }

    float toEncoded(float linear)
    {
        const float value
            = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(std::max(linear, 0.0f), 1.0f / 2.4f) - 0.055f;

        return std::clamp(value, 0.0f, 1.0f);
    }
}
