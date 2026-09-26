#include "tangent.hpp"

#include <osg/Vec3f>

#include "shaders/tangent.h"

namespace Rtx
{
    std::uint32_t packTangent(const osg::Vec4f& tangent)
    {
        return Shaders::packTangent(osg::Vec3f(tangent.x(), tangent.y(), tangent.z()), tangent.w() < 0.0f);
    }
}
