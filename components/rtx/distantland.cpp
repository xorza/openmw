#include "distantland.hpp"

#include <cmath>

#include <components/misc/constants.hpp>

namespace Rtx
{
    osg::Vec2i cellOf(const osg::Vec3f& position)
    {
        constexpr float size = static_cast<float>(Constants::CellSizeInUnits);

        return osg::Vec2i(
            static_cast<int>(std::floor(position.x() / size)), static_cast<int>(std::floor(position.y() / size)));
    }

    float distantLandReach(float cells, float viewingDistance)
    {
        if (!(cells > 0.0f))
            return viewingDistance;

        return cells * static_cast<float>(Constants::CellSizeInUnits);
    }
}
