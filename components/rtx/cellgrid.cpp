#include "cellgrid.hpp"

#include <algorithm>
#include <cmath>

#include <osg/Vec2f>

namespace Rtx
{
    namespace
    {
        /// The eye's gap to the cell's square along each axis, nought inside it: what both
        /// metrics below are taken over.
        osg::Vec2f gapsTo(const osg::Vec2i& cell, const osg::Vec3f& eye)
        {
            const float low = static_cast<float>(cell.x()) * sCellSize;
            const float lowY = static_cast<float>(cell.y()) * sCellSize;
            return osg::Vec2f(std::max({ low - eye.x(), eye.x() - (low + sCellSize), 0.0f }),
                std::max({ lowY - eye.y(), eye.y() - (lowY + sCellSize), 0.0f }));
        }
    }

    osg::Vec2i cellOf(const osg::Vec3f& position)
    {
        return osg::Vec2i(static_cast<int>(std::floor(position.x() / sCellSize)),
            static_cast<int>(std::floor(position.y() / sCellSize)));
    }

    bool withinReach(const osg::Vec2i& cell, const osg::Vec3f& eye, const float radius)
    {
        return distanceSquaredTo(cell, eye) < radius * radius;
    }

    float distanceSquaredTo(const osg::Vec2i& cell, const osg::Vec3f& eye)
    {
        return gapsTo(cell, eye).length2();
    }

    float chebyshevDistanceTo(const osg::Vec2i& cell, const osg::Vec3f& eye)
    {
        const osg::Vec2f gap = gapsTo(cell, eye);
        return std::max(gap.x(), gap.y());
    }

    bool inActiveGrid(const osg::Vec2i& cell, const osg::Vec4i& activeGrid)
    {
        return cell.x() >= activeGrid.x() && cell.y() >= activeGrid.y() && cell.x() < activeGrid.z()
            && cell.y() < activeGrid.w();
    }

    float distantLandReach(float cells, float viewingDistance)
    {
        if (!(cells > 0.0f))
            return viewingDistance;

        return cells * sCellSize;
    }
}
