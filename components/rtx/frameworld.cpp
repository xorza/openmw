#include "frameworld.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>

#include <osg/Matrixf>
#include <osg/Vec2d>
#include <osg/Vec3d>

#include "fogbuilder.hpp"
#include "sceneextractor.hpp"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "shaders/sky.h"

namespace Rtx
{
    void mirrorPrecipitation(SceneExtractor& extractor, osg::Node* fall, const osg::Vec3f& eye, const bool underwater,
        const std::size_t anchor, const std::size_t frameNumber)
    {
        if (fall == nullptr || underwater)
            return;

        // The same mask as everything else, because there is nothing here to select. The walk
        // starts at the precipitation node, so the subtree is already chosen; a mask is only ever
        // excluding what a renderer draws for itself, and none of that is under here. As what
        // falls, so that a roof keeps it off.
        extractor.extractFalling(*fall, osg::Matrixf::translate(eye), anchor, frameNumber);
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

    void FogDrift::advance(const osg::Vec2f& heading, const float wind, const double seconds)
    {
        if (mLastSeconds.has_value())
        {
            assert(seconds >= *mLastSeconds && "the clock the air is carried by ran backwards");
            mCarried
                += osg::Vec2d(heading) * (static_cast<double>(wind * Shaders::FOG_GALE) * (seconds - *mLastSeconds));
        }

        mLastSeconds = seconds;
    }

    osg::Vec2f splitSeconds(const double seconds)
    {
        const float high = static_cast<float>(seconds);
        return osg::Vec2f(high, static_cast<float>(seconds - static_cast<double>(high)));
    }

    std::array<osg::Vec3f, Shaders::FOG_SCALES> fogOffsets(const osg::Vec2d& carried, const double skySeconds)
    {
        // The shader's own turns, and its tiles stepped as `fogShape` steps them.
        const std::array<osg::Vec3f, Shaders::FOG_SCALES> churns{ Shaders::FOG_CHURN_COARSE, Shaders::FOG_CHURN_MIDDLE,
            Shaders::FOG_CHURN_FINE };
        const std::array<osg::Vec2f, Shaders::FOG_SCALES> turns{ osg::Vec2f(1.0f, 0.0f), Shaders::FOG_TURN_MIDDLE,
            Shaders::FOG_TURN_FINE };

        std::array<osg::Vec3f, Shaders::FOG_SCALES> offsets{};
        float tile = Shaders::FOG_TILE;
        for (std::size_t scale = 0; scale < offsets.size(); ++scale)
        {
            if (scale > 0)
                tile /= Shaders::FOG_LACUNARITY;

            // **The air is read from upwind, so the drift goes in with its sign turned.** A bank
            // sits at a fixed coordinate in the field, so sampling from further upwind as the clock
            // runs is what carries it past, and adding would walk the whole field into the wind.
            // Turned as the scale's read is turned, because the shader turns the position the drift
            // was taken off.
            const double c = turns[scale].x();
            const double s = turns[scale].y();
            const osg::Vec2d upwind = -carried;
            const osg::Vec3d churn(churns[scale]);
            const osg::Vec3d moved(c * upwind.x() - s * upwind.y() + churn.x() * skySeconds,
                s * upwind.x() + c * upwind.y() + churn.y() * skySeconds, churn.z() * skySeconds);

            const auto fraction = [&](const double along) {
                const double tiles = along / static_cast<double>(tile);
                return static_cast<float>(tiles - std::floor(tiles));
            };
            offsets[scale] = osg::Vec3f(fraction(moved.x()), fraction(moved.y()), fraction(moved.z()));
        }

        return offsets;
    }

    void describeWorld(
        const WorldReading& reading, FogDrift& drift, Shaders::VisibilityConstants& constants, FrameOptions& options)
    {
        const Daylight& day = reading.mDaylight;

        // **The day's gain on the whole sky at once, before anything is derived from it**: the sun,
        // the ambient, the dome, the air, the moons and the stars, so the sky's budget, the fog's
        // colour and the deck's light all follow from lifted terms. `DAYLIGHT_GAIN` says why, and
        // `Skylight::mExposureBias` already adapts to it.
        const float gain = day.mLight.mDaylightGain;

        Skylight light = day.mLight;
        light.mSun.mIrradiance *= gain;
        light.mSunAloft.mIrradiance *= gain;
        light.mAmbient *= gain;

        const osg::Vec3f horizon = day.mSkyHorizon * gain;
        const osg::Vec3f zenith = day.mSkyZenith * gain;

        std::array<MoonPlacement, 2> moons = reading.mMoons;
        for (MoonPlacement& moon : moons)
            moon.mIrradiance *= gain;

        Shaders::StarField stars = reading.mOutdoors
            ? describeStars(day.mStarFade, reading.mGlare, reading.mStarRoll, reading.mSky)
            : noStars();
        stars.mFade *= gain;
        stars.mGlow *= gain;

        const SkyBudget budget
            = reading.mOutdoors ? skyBudget(horizon, zenith, stars.mGlow, light.mAmbient) : SkyBudget{};

        Fog air = day.mFog;
        air.mColour *= gain;
        if (reading.mOutdoors)
            air.mColour = fogColour(budget.mMean, air.mColour);

        constants.mSun = Shaders::sunSource(light.mSun.mPosition, light.mSun.mIrradiance);
        constants.mSunDiscColour = light.mSun.mDiscColour;
        constants.mAmbient = light.mAmbient;
        constants.mAmbientFromSky = reading.mOutdoors ? 1.0f : 0.0f;
        constants.mBounceRate = Shaders::BOUNCE_RATE;
        constants.mDaylightGain = gain;

        constants.mSkyHorizon = horizon;
        constants.mSkyZenith = zenith;
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
            constants.mClouds
                = describeClouds(reading.mClouds, deckLight(light.mSunAloft, budget.mMean, moons), reading.mSky);

            describePatches(reading.mStarRoll, reading.mSky, constants.mSkyPatches);

            for (std::size_t moon = 0; moon < moons.size(); ++moon)
                constants.mMoons[moon] = describeMoon(moons[moon]);
        }

        constants.mFogColour = air.mColour;
        constants.mFogExtinction = air.mExtinction;
        constants.mFogUniform = air.mUniform;
        constants.mFogLift = air.mLift;

        // On the deck's own heading, because there is one wind over a landscape and an air that
        // turned with a transition would read as two. Swapped, because `mBearing` is the cosine
        // and sine of the rotation from north, which for a unit `(x, y)` is `(y, x)`. Integrated
        // and not multiplied by the clock — `FogDrift` says what the product cost.
        const osg::Vec2f heading(constants.mClouds.mBearing.y(), constants.mClouds.mBearing.x());
        drift.advance(heading, air.mWind, reading.mSkySeconds);
        const std::array<osg::Vec3f, Shaders::FOG_SCALES> offsets = fogOffsets(drift.get(), reading.mSkySeconds);
        std::copy(offsets.begin(), offsets.end(), constants.mFogOffsets);

        // The sea runs the way the deck does, and as its tiles were drawn where nothing blows.
        constants.mSeaHeading = heading.length2() > 0.0f ? heading / heading.length() : osg::Vec2f(1.0f, 0.0f);

        constants.mFogEdge = air.mEdge;

        // The same hair the water's own placement is dropped by, so that what the shader calls the
        // water level and where the surface actually is stay one number.
        constants.mWaterLevel = reading.mWaterLevel - Shaders::WATER_TIE_BREAK;
        constants.mWaterTime = splitSeconds(reading.mSeconds);
        constants.mRainOnWater = reading.mRainOnWater;
        constants.mShelterHeight = reading.mShelterHeight;

        options.mExposureBias = light.mExposureBias;
        options.mSkySeconds = reading.mSkySeconds;
        options.mGlare = reading.mSunGlare;
    }
}
