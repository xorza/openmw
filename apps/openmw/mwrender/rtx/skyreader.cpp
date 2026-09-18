#include "skyreader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

#include "../precipitation.hpp"
#include "../sceneframe.hpp"
#include "../skystate.hpp"

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

    Rtx::WorldReading SkyReader::read(const SkyState& sky, const WorldState& world, const Precipitation& falling,
        const float seconds, const float reach) const
    {
        const WeatherResult& weather = sky.mWeather;

        // Where the sun is drawn, and the light comes back along it. Not the light's own position,
        // which `match sunlight to sun` may have left on the orbit; nothing that traces can hold both.
        const osg::Vec4f disc = sunDiscOf(sky, world);
        osg::Vec3f discAt(disc.x(), disc.y(), disc.z());
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
        const bool skyShown = world.isOutdoors() && world.mSkyShown;

        // An interior has no sky colour: the weather system stops writing it indoors, so the air's
        // own colour stands in. A quasi-exterior has weather and so has one. A sky turned off is
        // the fog colour to the top, which is what the rasterizer's clear shows there.
        const osg::Vec3f zenith = room.has_value() ? room->mSkyZenith
            : skyShown                             ? Rtx::decodeColour(weather.mSkyColor)
                                                   : haze;

        // The sun is not assembled here: everything the world says about it goes to the one builder
        // that decides what a sun may be, and the light is taken whole from whichever built it.
        const Rtx::SkyReading reading{
            .mSunPosition = discAt,

            // Off the hour rather than off the disc's alpha, which the rasterizer leaves at one all
            // night with the disc hidden.
            .mSunShare = Rtx::sunShareAt(world.mGameHour, sky.mTimes),
            .mSunShareAloft = Rtx::sunShareAloft(world.mGameHour, sky.mTimes),
            .mSunColour = Rtx::decodeColour(world.mSunColour),
            .mAmbient = Rtx::decodeColour(world.mAmbientColour),
            .mDiscColour = skyShown ? Rtx::decodeColour(weather.mSunDiscColor) : osg::Vec3f(),
            .mGlare = weather.mGlareView,
        };
        const Rtx::Skylight light = room.has_value() ? room->mLight : Rtx::makeSkylight(reading);

        // The recorded depth and not the ramp `FogManager` made of it, which exists to hide a far
        // clip plane. A quasi-exterior stands in the weather's air, because the weather system is
        // run for one and the Construction Set greys its `AMBI` out; handed to `roomFog` it closed
        // over the sky. The two open-air builders differ only in the ring they close over. The
        // base wind and not the gust: how deep the fog's layer stands and how fast its field is
        // carried are the weather's settled character, not the number the engine wanders about it.
        const auto openAir = world.mLocation == Location::Exterior ? &Rtx::exteriorFog : &Rtx::quasiExteriorFog;
        const Rtx::Fog air
            = room.has_value() ? room->mFog : openAir(haze, weather.mFogDepth, weather.mBaseWindSpeed, reach);

        // Before the frame rather than into it, because the deck is lit by them (`Rtx::deckLight`).
        std::array<Rtx::MoonPlacement, 2> moons{};
        for (std::size_t moon = 0; moon < moons.size(); ++moon)
        {
            const Sky::MoonState& state = sky.mMoons[moon];

            // The glare is applied here, where the rasterizer applies it too
            // (`SkyManager::setWeather` calls `Moon::adjustTransparency` after the hand-over).
            moons[moon] = Rtx::placeMoon(static_cast<Rtx::Moon>(moon), state.mRotationFromHorizon,
                state.mRotationFromNorth, state.mPhase, state.mDaylightFade * weather.mGlareView);
            moons[moon].mFace = mMoonFaces.of(static_cast<Rtx::Moon>(moon));
        }

        // Secunda alone, as `SkyManager::setMoonColour` paints it.
        if (world.mMoonRed)
            moons[static_cast<std::size_t>(Rtx::Moon::Secunda)].mPaint = mMoonPaint;

        const auto weatherId = static_cast<std::uint32_t>(world.mWeatherId);

        // **Nothing recorded is not a rate.** `Weather::transitionDelta` divides by
        // `Clouds_Maximum_Percent`, which the shipped fallbacks leave at nought for ash and blight,
        // so a transition into either hands over an infinity or a NaN. The rasterizer survives one —
        // a NaN opacity draws nothing and the old sky stays — and a tracer mixes its whole sky by
        // it. Nothing recorded means the deck has crossed at once.
        const float cloudBlend
            = std::isfinite(weather.mCloudBlendFactor) ? std::clamp(weather.mCloudBlendFactor, 0.f, 1.f) : 1.f;

        return Rtx::WorldReading{
            .mDaylight = Rtx::Daylight{
                .mLight = light,
                .mSkyHorizon = haze,
                .mSkyZenith = zenith,

                // The engine's four-point `Stars` ramp at this hour, before the weather's glare is
                // taken off it, and nothing by day.
                .mStarFade = weather.mNight ? weather.mNightFade : 0.f,
                .mFog = air,
            },
            .mOutdoors = skyShown,
            .mGlare = weather.mGlareView,
            .mStarRoll = mClock.mStarRoll,
            .mSky = mSkyContent,
            .mMoons = moons,
            .mClouds = Rtx::CloudCrossing{
                .mWeather = weatherId,
                // The current weather twice where nothing is arriving, since the deck crosses
                // unconditionally: naming it on both sides at a blend of nothing is what lets it.
                .mNext = world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                          : weatherId,
                .mBlend = cloudBlend,
                // Reported rather than derived, because an ash or blight storm blows off Red
                // Mountain at the player; one each, because the rasterizer turns each of its two
                // cloud meshes by its own weather's storm.
                .mDirection = weather.mStormDirection,
                .mNextDirection = weather.mNextStormDirection,
                .mScroll = mClock.mCloudScroll,
            },

            // Negative infinity and not zero: zero is sea level, and a cell with no water has to
            // answer "how deep is this point" with never.
            .mWaterLevel = world.mWaterEnabled ? world.mWaterHeight : -std::numeric_limits<float>::infinity(),

            // What the sea is animated by, in elapsed seconds rather than frames, or the sea would
            // slow down whenever the frame did.
            .mSeconds = seconds,
            .mSkySeconds = mClock.mSeconds,

            // How much of what is falling rings the water, nought to one: the precipitation's
            // alpha where its kind makes ripples. `Water::setRainIntensity` takes the same number.
            .mRainOnWater = falling.getRainRipplesEnabled() ? falling.getPrecipitationAlpha() : 0.0f,

            // The top of the box the rasterizer's `PrecipitationOccluder::update` draws its depth
            // map from: the precipitation's own range and a cell over it, above the eye. Nought
            // where the game says what is falling is not the kind a roof stops — ash and blight
            // blow under one, and rain and snow do not.
            .mShelterHeight
            = falling.isOccluded() ? falling.getOcclusionRange().z() + Constants::CellSizeInUnits : 0.0f,

            // The fader's strength as `SunGlareCallback` multiplies it up: `_Max` by the
            // time-of-day fade by the weather's `Glare_View`. The glare node hangs under the sun's
            // own transform, so a sun the weather manager has hidden for the night or a sky `tsky`
            // turned off draws none.
            .mGlareColour = mGlareColour,
            .mGlareAngleMax = mGlareAngleMax,
            .mGlareStrength = skyShown && sky.mSunUp ? mGlareMax * sky.mGlareFade * weather.mGlareView : 0.0f,
        };
    }
}
