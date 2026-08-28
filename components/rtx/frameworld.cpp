#include "frameworld.hpp"

#include "shaders/scene.h"
#include <components/weather/precipitation.hpp>

namespace Rtx
{
    float rainOnWater(const Weather::Precipitation* fall)
    {
        return fall != nullptr && fall->ripplesEnabled() ? fall->getPrecipitationAlpha() : 0.0f;
    }

    Shaders::CloudDeck noDeck()
    {
        Shaders::CloudDeck deck{};
        deck.mTexture = Shaders::NO_TEXTURE;
        deck.mNext = Shaders::NO_TEXTURE;
        return deck;
    }

    Shaders::StarField noStars()
    {
        Shaders::StarField stars{};
        stars.mTexture = Shaders::NO_TEXTURE;
        return stars;
    }

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

        // The sea runs the way the deck does, and as its tiles were drawn where nothing blows.
        constants.mSeaHeading = heading.length2() > 0.0f ? heading / heading.length() : osg::Vec2f(1.0f, 0.0f);

        constants.mFogEdge = world.mAir.mEdge;

        // The same hair the water's own placement is dropped by, so that what the shader calls the
        // water level and where the surface actually is stay one number.
        constants.mWaterLevel = world.mWaterLevel - Shaders::WATER_TIE_BREAK;
        constants.mTime = world.mSeconds;
        constants.mRainOnWater = world.mRainOnWater;

        constants.mClouds = world.mClouds;
        constants.mStars = world.mStars;

        for (std::size_t patch = 0; patch < world.mSkyPatches.size(); ++patch)
            constants.mSkyPatches[patch] = world.mSkyPatches[patch];

        for (std::size_t moon = 0; moon < world.mMoons.size(); ++moon)
            constants.mMoons[moon] = describeMoon(world.mMoons[moon]);
    }
}
