#include "srgb.hpp"

#include <algorithm>
#include <cmath>

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

    osg::Vec3f toLinear(const osg::Vec3f& encoded)
    {
        return osg::Vec3f(toLinear(encoded.x()), toLinear(encoded.y()), toLinear(encoded.z()));
    }
}
