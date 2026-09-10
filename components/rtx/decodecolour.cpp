#include "decodecolour.hpp"

#include "srgb.hpp"

namespace Rtx
{
    osg::Vec3f decodeColour(const osg::Vec4f& encoded)
    {
        return toLinear(osg::Vec3f(encoded.x(), encoded.y(), encoded.z()));
    }

    osg::Vec3f decodeColour(const osg::Vec4ub& encoded)
    {
        return osg::Vec3f(toLinear(encoded.r()), toLinear(encoded.g()), toLinear(encoded.b()));
    }

    osg::Vec3f decodeColour(std::uint32_t packed)
    {
        const auto channel = [](std::uint32_t bits) { return toLinear(static_cast<std::uint8_t>(bits & 0xFFu)); };

        return osg::Vec3f(channel(packed), channel(packed >> 8), channel(packed >> 16));
    }
}
