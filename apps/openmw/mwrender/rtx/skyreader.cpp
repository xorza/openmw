#include "skyreader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include <osg/Math>
#include <osg/Vec3f>

#include <components/fallback/fallback.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/colour.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/skylight.hpp>
#include <components/settings/values.hpp>
#include <components/sky/moonstate.hpp>
#include <components/sky/timeofday.hpp>

#include "../sceneframe.hpp"

namespace MWRender
{
    namespace
    {
        /// `Weather_Sun_Glare_Fader_Color` as `SunGlareCallback` takes it: doubled and clamped,
        /// replicating the original's flaw of setting one colour on two material terms, which the
        /// fixed-function pipeline then saturated — only the red does, at the shipped values, so
        /// the wash is orange. In the display's own values and not decoded, because that is the
        /// space the rasterizer adds it in and `tone.comp` adds it in the same.
        osg::Vec3f glareFaderColour()
        {
            const osg::Vec4f read = Fallback::Map::getColour("Weather_Sun_Glare_Fader_Color");
            return osg::Vec3f(
                std::min(1.0f, 2.0f * read.r()), std::min(1.0f, 2.0f * read.g()), std::min(1.0f, 2.0f * read.b()));
        }
    }

    SkyReader::SkyReader()
        : mMoonPaint(Rtx::decodeColour(Fallback::Map::getColour("Moons_Script_Color")))
        , mGlareColour(glareFaderColour())
        , mGlareMax(Fallback::Map::getFloat("Weather_Sun_Glare_Fader_Max"))
        , mGlareAngleMax(osg::DegreesToRadians(Fallback::Map::getFloat("Weather_Sun_Glare_Fader_Angle_Max")))
    {
    }

    Rtx::SkyMeshes SkyReader::meshes()
    {
        return Rtx::SkyMeshes{
            .mClouds = Settings::models().mSkyclouds,
            .mStars = Settings::models().mSkynight02,
            .mStarsFallback = Settings::models().mSkynight01,
        };
    }

    void SkyReader::attach(Rtx::SceneDesc& scene, Resource::SceneManager& scenes)
    {
        mMoonFaces = Rtx::addMoonFaces(scene);
        mSkyContent = Rtx::addSkyContent(scene, scenes, meshes());
    }

    void SkyReader::detach(Rtx::SceneDesc& scene)
    {
        Rtx::dropSkyContent(scene, mSkyContent);
        Rtx::dropMoonFaces(scene, mMoonFaces);
        mSkyContent = Rtx::SkyContent{};
        mMoonFaces = Rtx::MoonFaces{};
    }

    Rtx::WorldReading SkyReader::read(const WorldState& world, const float seconds, const float reach) const
    {
        // Where the sun is, and the light comes back along it. `mSunVector` is where the
        // rasterizer's light travels and is not the negation of this; nothing that traces can hold both.
        osg::Vec3f discAt(world.mSky.mSunPosition.x(), world.mSky.mSunPosition.y(), world.mSky.mSunPosition.z());
        if (discAt.length2() > 0.0f)
            discAt.normalize();

        // A room's light out of the record the cell wrote and not the rasterizer's reading of it,
        // which lifts the ambient for its own falloff and points a directional light.
        const std::optional<Rtx::Daylight> room = world.mRoom.has_value()
            ? std::optional(Rtx::makeRoomLight(
                *world.mRoom, osg::Vec3f(world.mNightEye.x(), world.mNightEye.y(), world.mNightEye.z())))
            : std::nullopt;

        // The horizon is the fog and the zenith is the sky's own, which is the pair Morrowind
        // records. Decoded here, because the world does not know what a transport is.
        const osg::Vec3f haze = room.has_value() ? room->mSkyHorizon : Rtx::decodeColour(world.mAir.mColour);

        // Whether there is a sky to draw: outdoors, and `tsky` has not turned it off. Off, the
        // rasterizer hides the sky node whole — the dome, the decks, the stars, the sun's disc and
        // the moons — and clears to the fog colour, while the sun and the weather go on lighting.
        const bool skyShown = world.isOutdoors() && world.mSky.mSkyEnabled;

        // An interior has no sky colour: the weather system stops writing it indoors, so the air's
        // own colour stands in. A quasi-exterior has weather and so has one. A sky turned off is
        // the fog colour to the top, which is what the rasterizer's clear shows there.
        const osg::Vec3f zenith = room.has_value() ? room->mSkyZenith
            : skyShown                             ? Rtx::decodeColour(world.mSky.mSkyColour)
                                                   : haze;

        // The sun is not assembled here: everything the world says about it goes to the one builder
        // that decides what a sun may be, and the light is taken whole from whichever built it.
        const Sky::TimeOfDaySettings& times = Sky::TimeOfDaySettings::shared();
        const Rtx::SkyReading reading{
            .mSunPosition = discAt,

            // Off the hour rather than off the disc's alpha, which the rasterizer leaves at one all
            // night with the disc hidden.
            .mSunShare = Rtx::sunShareAt(world.mGameHour, times),
            .mSunShareAloft = Rtx::sunShareAloft(world.mGameHour, times),
            .mSunColour = Rtx::decodeColour(world.mSunColour),
            .mAmbient = Rtx::decodeColour(world.mAmbientColour),
            .mDiscColour = skyShown ? Rtx::decodeColour(world.mSky.mSunDiscColour) : osg::Vec3f(),
            .mGlare = world.mSky.mSunGlare,
        };
        const Rtx::Skylight light = room.has_value() ? room->mLight : Rtx::makeSkylight(reading);

        // The recorded depth and not the ramp `FogManager` made of it, which exists to hide a far
        // clip plane. A quasi-exterior stands in the weather's air, because the weather system is
        // run for one and the Construction Set greys its `AMBI` out; handed to `roomFog` it closed
        // over the sky. The two open-air builders differ only in the ring they close over.
        const auto openAir = world.mLocation == Location::Exterior ? &Rtx::exteriorFog : &Rtx::quasiExteriorFog;
        const Rtx::Fog air
            = room.has_value() ? room->mFog : openAir(haze, world.mSky.mFogDepth, world.mSky.mBaseWindSpeed, reach);

        // Before the frame rather than into it, because the deck is lit by them (`Rtx::deckLight`).
        std::array<Rtx::MoonPlacement, 2> moons{};
        for (std::size_t moon = 0; moon < moons.size(); ++moon)
        {
            const Sky::MoonState& state = world.mSky.mMoons[moon];

            // The glare is applied here, where the rasterizer applies it too
            // (`SkyManager::setWeather` calls `Moon::adjustTransparency` after the hand-over).
            moons[moon] = Rtx::placeMoon(static_cast<Rtx::Moon>(moon), state.mRotationFromHorizon,
                state.mRotationFromNorth, state.mPhase, state.mDaylightFade * world.mSky.mSunGlare);
            moons[moon].mFace = mMoonFaces.of(static_cast<Rtx::Moon>(moon));
        }

        // Secunda alone, as `SkyManager::setMoonColour` paints it.
        if (world.mSky.mMoonRed)
            moons[static_cast<std::size_t>(Rtx::Moon::Secunda)].mPaint = mMoonPaint;

        const auto weatherId = static_cast<std::uint32_t>(world.mWeatherId);

        return Rtx::WorldReading{
            .mDaylight = Rtx::Daylight{
                .mLight = light,
                .mSkyHorizon = haze,
                .mSkyZenith = zenith,
                .mStarFade = world.mSky.mNightFade,
                .mFog = air,
            },
            .mOutdoors = skyShown,
            .mGlare = world.mSky.mSunGlare,
            .mStarRoll = world.mSky.mStarRoll,
            .mSky = mSkyContent,
            .mMoons = moons,
            .mClouds = Rtx::CloudCrossing{
                .mWeather = weatherId,
                // The current weather twice where nothing is arriving, since the deck crosses
                // unconditionally: naming it on both sides at a blend of nothing is what lets it.
                .mNext = world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                          : weatherId,
                .mBlend = world.mSky.mCloudBlend,
                .mDirection = world.mSky.mCloudDirection,
                .mNextDirection = world.mSky.mNextCloudDirection,
                .mScroll = world.mSky.mSkyCloudScroll,
            },

            // Negative infinity and not zero: zero is sea level, and a cell with no water has to
            // answer "how deep is this point" with never.
            .mWaterLevel = world.mWaterEnabled ? world.mWaterHeight : -std::numeric_limits<float>::infinity(),

            // What the sea is animated by, in elapsed seconds rather than frames, or the sea would
            // slow down whenever the frame did.
            .mSeconds = seconds,
            .mSkySeconds = world.mSky.mSkySeconds,
            .mRainOnWater = world.mRainOnWater,

            // The top of the box the rasterizer's `PrecipitationOccluder::update` draws its depth
            // map from: the precipitation's own range and a cell over it, above the eye. Nought
            // where the game says what is falling is not the kind a roof stops — ash and blight
            // blow under one, and rain and snow do not.
            .mShelterHeight
            = world.mPrecipitating ? world.mPrecipitationRange.z() + Constants::CellSizeInUnits : 0.0f,

            // The fader's strength as `SunGlareCallback` multiplies it up: `_Max` by the
            // time-of-day fade by the weather's `Glare_View`. The glare node hangs under the sun's
            // own transform, so a sun the weather manager has hidden for the night or a sky `tsky`
            // turned off draws none.
            .mGlareColour = mGlareColour,
            .mGlareAngleMax = mGlareAngleMax,
            .mGlareStrength = skyShown && world.mSky.mSunEnabled
                ? mGlareMax * world.mSky.mGlareFade * world.mSky.mSunGlare
                : 0.0f,
        };
    }
}
