#include "frameworld.hpp"

#include <cstddef>

#include <osg/Matrixf>

#include "sceneextractor.hpp"
#include "shaders/scene.h"

namespace Rtx
{
    void mirrorPrecipitation(SceneExtractor& extractor, osg::Node* fall, const osg::Vec3f& eye, const bool underwater,
        const std::size_t frameNumber)
    {
        if (fall == nullptr || underwater)
            return;

        // The same mask as everything else, because there is nothing here to select. The walk
        // starts at the precipitation node, so the subtree is already chosen; a mask is only ever
        // excluding what a renderer draws for itself, and none of that is under here.
        extractor.extract(*fall, osg::Matrixf::translate(eye), 0, frameNumber);
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

    Shaders::SkyPatch noPatch()
    {
        Shaders::SkyPatch none{};
        none.mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f);
        none.mRight = osg::Vec3f(1.0f, 0.0f, 0.0f);
        none.mUp = osg::Vec3f(0.0f, 1.0f, 0.0f);
        none.mLimb = 0.0f;
        none.mTexture = Shaders::NO_TEXTURE;

        return none;
    }

    float describeWorld(const WorldReading& reading, Shaders::VisibilityConstants& constants)
    {
        const Daylight& day = reading.mDaylight;
        const Skylight& light = day.mLight;

        const Shaders::StarField stars = reading.mOutdoors
            ? describeStars(day.mStarFade, reading.mGlare, reading.mStarRoll, reading.mSky)
            : noStars();

        const SkyBudget budget
            = reading.mOutdoors ? skyBudget(day.mSkyHorizon, day.mSkyZenith, stars.mGlow, light.mAmbient) : SkyBudget{};

        Fog air = day.mFog;
        if (reading.mOutdoors)
            air.mColour = fogColour(budget.mMean, air.mColour);

        constants.mSunPosition = light.mSun.mPosition;
        constants.mSunIrradiance = light.mSun.mIrradiance;
        constants.mSunDiscColour = light.mSun.mDiscColour;
        constants.mAmbient = light.mAmbient;
        constants.mAmbientFromSky = reading.mOutdoors ? 1.0f : 0.0f;

        constants.mSkyHorizon = day.mSkyHorizon;
        constants.mSkyZenith = day.mSkyZenith;
        constants.mSkyFill = budget.mFill;

        constants.mStars = stars;

        // A moon is a light as well as a disc, and both halves stop at the door: the weather
        // system stops reporting the moment the player steps inside, so what it last said is still
        // standing in the frame the room is drawn from — a moon left in one lights through every
        // seam the shell has, and traces a shadow ray at a body over the roof.
        constants.mClouds = noDeck();
        for (Shaders::SkyPatch& patch : constants.mSkyPatches)
            patch = noPatch();
        for (Shaders::MoonDisc& moon : constants.mMoons)
            moon = describeMoon(MoonPlacement{});

        if (reading.mOutdoors)
        {
            constants.mClouds = describeClouds(
                reading.mClouds, deckLight(light.mSunAloft, budget.mMean, reading.mMoons), reading.mSky);

            describePatches(reading.mStarRoll, reading.mSky, constants.mSkyPatches);

            for (std::size_t moon = 0; moon < reading.mMoons.size(); ++moon)
                constants.mMoons[moon] = describeMoon(reading.mMoons[moon]);
        }

        constants.mFogColour = air.mColour;
        constants.mFogExtinction = air.mExtinction;
        constants.mFogUniform = air.mUniform;
        constants.mFogLift = air.mLift;

        // On the deck's own heading, because there is one wind over a landscape and an air that
        // turned with a transition would read as two. Swapped, because `mBearing` is the cosine
        // and sine of the rotation from north, which for a unit `(x, y)` is `(y, x)`.
        const osg::Vec2f heading(constants.mClouds.mBearing.y(), constants.mClouds.mBearing.x());
        constants.mFogWind = heading * air.mWind;

        // The sea runs the way the deck does, and as its tiles were drawn where nothing blows.
        constants.mSeaHeading = heading.length2() > 0.0f ? heading / heading.length() : osg::Vec2f(1.0f, 0.0f);

        constants.mFogEdge = air.mEdge;

        // The same hair the water's own placement is dropped by, so that what the shader calls the
        // water level and where the surface actually is stay one number.
        constants.mWaterLevel = reading.mWaterLevel - Shaders::WATER_TIE_BREAK;
        constants.mTime = reading.mSeconds;
        constants.mRainOnWater = reading.mRainOnWater;

        return light.mExposureBias;
    }
}
