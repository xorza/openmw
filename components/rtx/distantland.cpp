#include "distantland.hpp"

#include <cmath>
#include <cstdlib>

namespace Rtx
{
    osg::Vec2i cellOf(const osg::Vec3f& position)
    {
        return osg::Vec2i(static_cast<int>(std::floor(position.x() / sCellSize)),
            static_cast<int>(std::floor(position.y() / sCellSize)));
    }

    bool withinCells(const osg::Vec2i& cell, const osg::Vec2i& eye, const int band)
    {
        return std::abs(cell.x() - eye.x()) <= band && std::abs(cell.y() - eye.y()) <= band;
    }

    float distantLandReach(float cells, float viewingDistance)
    {
        if (!(cells > 0.0f))
            return viewingDistance;

        return cells * sCellSize;
    }
}
