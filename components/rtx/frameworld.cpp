#include "frameworld.hpp"

#include "shaders/scene.h"

namespace Rtx
{
    void applyWorld(const FrameWorld& world, Shaders::VisibilityConstants& constants)
    {
        constants.mSunPosition = world.mSun.mPosition;
        constants.mSunIrradiance = world.mSun.mIrradiance;
        constants.mSunDiscColour = world.mSun.mDiscColour;
        constants.mAmbient = world.mAmbient;
        constants.mAmbientFromSky = world.mAmbientFromSky;

        constants.mSkyHorizon = world.mSkyHorizon;
        constants.mSkyZenith = world.mSkyZenith;
        constants.mSkyFill = world.mSkyFill;

        constants.mFogColour = world.mAir.mColour;
        constants.mFogExtinction = world.mAir.mExtinction;
        constants.mFogUniform = world.mAir.mUniform;
        constants.mFogLift = world.mAir.mLift;

        // **On the deck's own heading, because there is one wind over a landscape.** The deck of the
        // weather that is here rather than the one arriving: the reference this follows holds one
        // heading for the whole sky, and an air that turned with a transition would read as two.
        //
        // **Swapped back, because the deck holds a turn and not a direction.** `mBearing` is the
        // cosine and sine of the rotation from north, which for a unit `(x, y)` is `(y, x)` — so
        // north was reaching the air as east.
        const osg::Vec2f heading(world.mClouds.mBearing.y(), world.mClouds.mBearing.x());
        constants.mFogWind = heading * world.mAir.mWind;
        constants.mFogEdge = world.mAir.mEdge;

        // The same hair the water's own placement is dropped by, so that what the shader calls the
        // water level and where the surface actually is stay one number.
        constants.mWaterLevel = world.mWaterLevel - Shaders::WATER_TIE_BREAK;
        constants.mTime = world.mSeconds;

        constants.mClouds = world.mClouds;
        constants.mStars = world.mStars;

        for (std::size_t patch = 0; patch < world.mSkyPatches.size(); ++patch)
            constants.mSkyPatches[patch] = world.mSkyPatches[patch];

        for (std::size_t moon = 0; moon < world.mMoons.size(); ++moon)
            constants.mMoons[moon] = describeMoon(world.mMoons[moon]);
    }
}
