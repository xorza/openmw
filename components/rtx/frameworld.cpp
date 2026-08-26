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

        constants.mSkyHorizon = world.mSkyHorizon;
        constants.mSkyZenith = world.mSkyZenith;

        constants.mFogColour = world.mAir.mColour;
        constants.mFogExtinction = world.mAir.mExtinction;
        constants.mFogUniform = world.mAir.mUniform;

        // The same hair the water's own placement is dropped by, so that what the shader calls the
        // water level and where the surface actually is stay one number.
        constants.mWaterLevel = world.mWaterLevel - Shaders::WATER_TIE_BREAK;
        constants.mTime = world.mSeconds;

        constants.mWeather = world.mWeather;
        constants.mNextWeather = world.mNextWeather;
        constants.mWeatherBlend = world.mWeatherBlend;

        constants.mWindSpeed = world.mWindSpeed;
        constants.mStormDirection = world.mStormDirection;

        constants.mClouds = world.mClouds;
        constants.mStars = world.mStars;

        for (std::size_t patch = 0; patch < world.mSkyPatches.size(); ++patch)
            constants.mSkyPatches[patch] = world.mSkyPatches[patch];

        for (std::size_t moon = 0; moon < world.mMoons.size(); ++moon)
            constants.mMoons[moon] = describeMoon(world.mMoons[moon]);
    }
}
