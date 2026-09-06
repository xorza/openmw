#include "srgb.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Rtx
{
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

    float toLinear(std::uint8_t encoded)
    {
        // **Built on the first ask rather than at namespace scope**, so that no order between
        // translation units can put a reader before it: `terraincomposite.cpp` holds its one shared
        // shading map the same way and for the same reason.
        static const std::array<float, 256> sOfByte = [] {
            std::array<float, 256> made{};
            for (std::size_t at = 0; at < made.size(); ++at)
                made[at] = toLinear(static_cast<float>(at) / 255.0f);

            return made;
        }();

        return sOfByte[encoded];
    }

    osg::Vec3f toLinear(const osg::Vec3f& encoded)
    {
        return osg::Vec3f(toLinear(encoded.x()), toLinear(encoded.y()), toLinear(encoded.z()));
    }
}
