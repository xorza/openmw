#include "tangent.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        float signOf(float value)
        {
            return value >= 0.0f ? 1.0f : -1.0f;
        }

        std::uint32_t quantize(float value)
        {
            const long steps = std::lround(std::clamp(value, -1.0f, 1.0f) * static_cast<float>(Shaders::TANGENT_STEPS));
            return static_cast<std::uint32_t>(steps + static_cast<long>(Shaders::TANGENT_STEPS));
        }

        float dequantize(std::uint32_t stored)
        {
            return (static_cast<float>(stored) - static_cast<float>(Shaders::TANGENT_STEPS))
                / static_cast<float>(Shaders::TANGENT_STEPS);
        }
    }

    std::uint32_t packTangent(const osg::Vec4f& tangent)
    {
        const osg::Vec3f direction(tangent.x(), tangent.y(), tangent.z());
        const float sum = std::abs(direction.x()) + std::abs(direction.y()) + std::abs(direction.z());
        if (!(sum > 0.0f))
            return 0u;

        const osg::Vec3f folded = direction / sum;
        osg::Vec2f square(folded.x(), folded.y());
        if (folded.z() < 0.0f)
            square = osg::Vec2f(
                (1.0f - std::abs(folded.y())) * signOf(folded.x()), (1.0f - std::abs(folded.x())) * signOf(folded.y()));

        return Shaders::TANGENT_PRESENT | (tangent.w() < 0.0f ? Shaders::TANGENT_FLIPPED : 0u) | quantize(square.x())
            | (quantize(square.y()) << Shaders::TANGENT_COORDINATE_BITS);
    }

    osg::Vec4f unpackTangent(std::uint32_t packed)
    {
        if ((packed & Shaders::TANGENT_PRESENT) == 0u)
            return osg::Vec4f();

        const float u = dequantize(packed & Shaders::TANGENT_COORDINATE_MASK);
        const float v = dequantize((packed >> Shaders::TANGENT_COORDINATE_BITS) & Shaders::TANGENT_COORDINATE_MASK);

        osg::Vec3f unit(u, v, 1.0f - std::abs(u) - std::abs(v));
        if (unit.z() < 0.0f)
            unit = osg::Vec3f((1.0f - std::abs(v)) * signOf(u), (1.0f - std::abs(u)) * signOf(v), unit.z());
        unit.normalize();

        return osg::Vec4f(unit, (packed & Shaders::TANGENT_FLIPPED) != 0u ? -1.0f : 1.0f);
    }
}
