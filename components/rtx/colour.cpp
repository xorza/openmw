#include "colour.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Rtx
{
    namespace
    {
        /// sRGB's transfer function itself. Both overloads answer with this one, so what a stored
        /// byte is worth and what a value that was never a byte is worth cannot come from two
        /// spellings of the same three constants.
        float curve(float encoded)
        {
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        /// The two hundred and fifty-six answers there are, worked out on the first ask so that no
        /// order between translation units can put a reader before it.
        const std::array<float, 256>& ofByte()
        {
            static const std::array<float, 256> sMade = [] {
                std::array<float, 256> made{};
                for (std::size_t at = 0; at < made.size(); ++at)
                    made[at] = curve(static_cast<float>(at) / 255.0f);

                return made;
            }();

            return sMade;
        }
    }

    float toLinear(float encoded)
    {
        // The table where the value arrived as a stored byte, because nearly everything the
        // content states is `k / 255` and `std::pow` is a libm call no compiler inlines. Exact
        // rather than a guess, because the comparison is the same `k / 255.0f` the table was
        // built from, and anything else takes the curve. The bounds are asked first so that a NaN
        // never reaches the conversion to an integer.
        if (encoded > 0.0f && encoded <= 1.0f)
        {
            const auto byte = static_cast<std::uint8_t>(encoded * 255.0f + 0.5f);
            if (static_cast<float>(byte) / 255.0f == encoded)
                return ofByte()[byte];
        }

        return curve(encoded);
    }

    float toEncoded(float linear)
    {
        const float value
            = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(std::max(linear, 0.0f), 1.0f / 2.4f) - 0.055f;

        return std::clamp(value, 0.0f, 1.0f);
    }

    float toLinear(std::uint8_t encoded)
    {
        return ofByte()[encoded];
    }

    osg::Vec3f toLinear(const osg::Vec3f& encoded)
    {
        return osg::Vec3f(toLinear(encoded.x()), toLinear(encoded.y()), toLinear(encoded.z()));
    }

    osg::Vec3f decodeColour(const osg::Vec4f& encoded)
    {
        return toLinear(osg::Vec3f(encoded.x(), encoded.y(), encoded.z()));
    }

    osg::Vec3f decodeColour(const osg::Vec4ub& encoded)
    {
        return osg::Vec3f(toLinear(encoded.r()), toLinear(encoded.g()), toLinear(encoded.b()));
    }

    osg::Vec3f decodeColour(const Surface::Colour& encoded)
    {
        return toLinear(osg::Vec3f(encoded.mRed, encoded.mGreen, encoded.mBlue));
    }

    osg::Vec3f decodeColour(std::uint32_t packed)
    {
        const auto channel = [](std::uint32_t bits) { return toLinear(static_cast<std::uint8_t>(bits & 0xFFu)); };

        return osg::Vec3f(channel(packed), channel(packed >> 8), channel(packed >> 16));
    }

    namespace
    {
        /// One endpoint out of the five-six-five pair a block stores its ends as: five and six bits
        /// replicated into eight, which is what every decoder does and what makes the endpoints
        /// exactly representable as bytes.
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
