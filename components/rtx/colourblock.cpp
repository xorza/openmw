#include "colourblock.hpp"

namespace Rtx
{
    namespace
    {
        /// One endpoint out of the five-six-five pair a block stores its ends as.
        ///
        /// Five and six bits replicated into eight, which is what every decoder does and what makes
        /// the endpoints exactly representable as bytes.
        osg::Vec3f decode565(std::uint16_t packed)
        {
            const auto five = [](std::uint32_t bits) { return static_cast<float>(bits << 3 | bits >> 2) / 255.0f; };
            const auto six = [](std::uint32_t bits) { return static_cast<float>(bits << 2 | bits >> 4) / 255.0f; };

            return osg::Vec3f(five((packed >> 11) & 0x1Fu), six((packed >> 5) & 0x3Fu), five(packed & 0x1Fu));
        }
    }

    ColourBlock ColourBlock::read(std::span<const std::byte, 8> bytes, bool punchThrough)
    {
        const auto read16 = [&](std::size_t at) {
            return static_cast<std::uint16_t>(
                std::to_integer<std::uint32_t>(bytes[at]) | std::to_integer<std::uint32_t>(bytes[at + 1]) << 8);
        };

        const std::uint16_t first = read16(0);
        const std::uint16_t second = read16(2);
        const osg::Vec3f c0 = decode565(first);
        const osg::Vec3f c1 = decode565(second);

        ColourBlock block;
        block.mCutout = punchThrough && first <= second;
        block.mPalette[0] = c0;
        block.mPalette[1] = c1;

        if (block.mCutout)
        {
            block.mPalette[2] = (c0 + c1) * 0.5f;
            block.mPalette[3] = osg::Vec3f();
        }
        else
        {
            block.mPalette[2] = c0 * (2.0f / 3.0f) + c1 * (1.0f / 3.0f);
            block.mPalette[3] = c0 * (1.0f / 3.0f) + c1 * (2.0f / 3.0f);
        }

        for (std::size_t row = 0; row < 4; ++row)
            block.mIndices |= std::to_integer<std::uint32_t>(bytes[4 + row]) << (row * 8);

        return block;
    }
}
