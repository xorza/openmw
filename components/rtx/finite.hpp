#pragma once

#include <cmath>

#include <osg/Vec3f>

namespace Rtx
{
    /// Whether every component of `value` is a finite number. What content places, turns or tints
    /// is data, and one that is not finite is left out where it is read rather than carried into a
    /// bound, a grid or a shader.
    inline bool isFinite(const osg::Vec3f& value)
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
    }
}
