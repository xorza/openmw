#include "offscreenframing.hpp"

#include <cmath>

#include <osg/Math>

#include <components/fallback/fallback.hpp>

namespace SceneUtil
{
    PreviewCamera inventoryCamera()
    {
        return PreviewCamera{
            .mOrigin = osg::Vec3f(0.0f, 700.0f, 71.0f),
            .mTarget = osg::Vec3f(0.0f, 0.0f, 71.0f),
        };
    }

    FlatLight inventoryLight()
    {
        const float azimuth = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationX"));
        const float altitude = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationY"));

        return FlatLight{
            .mDirection = osg::Vec3f(
                -std::cos(azimuth) * std::sin(altitude), std::sin(azimuth) * std::sin(altitude), std::cos(altitude)),
            .mDiffuse = osg::Vec4f(Fallback::Map::getFloat("Inventory_DirectionalDiffuseR"),
                Fallback::Map::getFloat("Inventory_DirectionalDiffuseG"),
                Fallback::Map::getFloat("Inventory_DirectionalDiffuseB"), 1.0f),
            .mAmbient = osg::Vec4f(Fallback::Map::getFloat("Inventory_DirectionalAmbientR"),
                Fallback::Map::getFloat("Inventory_DirectionalAmbientG"),
                Fallback::Map::getFloat("Inventory_DirectionalAmbientB"), 1.0f),
        };
    }

    FlatLight mapLight()
    {
        return FlatLight{
            .mDirection = osg::Vec3f(-0.3f, -0.3f, 0.7f),
            .mDiffuse = osg::Vec4f(0.7f, 0.7f, 0.7f, 1.0f),
            .mAmbient = osg::Vec4f(0.3f, 0.3f, 0.3f, 1.0f),
        };
    }
}
