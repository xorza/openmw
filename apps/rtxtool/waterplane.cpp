#include "waterplane.hpp"

#include <limits>

#include <osg/Geometry>

#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/sceneutil/waterutil.hpp>

namespace RtxTool
{
    namespace
    {
        /// The sheet the game builds: a hundred and fifty cells across, subdivided forty ways,
        /// with the texture repeating nine hundred times over it
        /// (`apps/openmw/mwrender/gl/water.cpp:442`). The subdivision is there because some drivers
        /// dislike enormous triangles; the ray tracer would not care, but agreeing with the game
        /// about what the sea *is* matters more than sixteen hundred quads it need not have.
        constexpr float sCells = 150.0f;
        constexpr int sSegments = 40;
        constexpr float sTextureRepeats = 900.0f;
    }

    WaterPlane::WaterPlane(osg::Group& root)
    {
        const osg::ref_ptr<osg::Geometry> sheet = SceneUtil::createWaterGeometry(
            static_cast<float>(Constants::CellSizeInUnits) * sCells, sSegments, sTextureRepeats);
        sheet->setNodeMask(sWaterMask);
        sheet->setName("Water Geometry");

        mNode = new osg::PositionAttitudeTransform;
        mNode->setName("Water Root");
        mNode->addChild(sheet);
        root.addChild(mNode);
    }

    float WaterPlane::follow(const ESM::Cell& cell)
    {
        // **The flag is the gate, not the height.** Every interior carries a water height whether or
        // not it has water, so reading the height and taking its presence as the answer floods the
        // several hundred dry rooms in the game. `hasWater` is the question, and it already answers
        // it the right way: the flag for an interior, true for every exterior.
        if (!cell.hasWater())
        {
            mNode->setNodeMask(0);
            return -std::numeric_limits<float>::infinity();
        }

        mNode->setNodeMask(~0u);

        // **The sea is at zero everywhere out of doors.** Not one shipped exterior carries a height
        // of its own, so Vvardenfell is one body of water and the sheet only has to be under
        // whichever cell is being looked at.
        const float level = cell.isExterior() ? 0.0f : cell.mWater;
        const auto side = static_cast<float>(Constants::CellSizeInUnits);

        mNode->setPosition(cell.isExterior() ? osg::Vec3f((static_cast<float>(cell.getGridX()) + 0.5f) * side,
                               (static_cast<float>(cell.getGridY()) + 0.5f) * side, level)
                                             : osg::Vec3f(0.0f, 0.0f, level));

        return level;
    }
}
