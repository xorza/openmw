#include "lighting.hpp"

#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/skybuilder.hpp>
#include <components/sky/clouds.hpp>
#include <components/weather/downpour.hpp>

namespace RtxTool
{
    namespace
    {
        void settle(CellLighting& lighting, const Rtx::Daylight& daylight, int day, float hour)
        {
            lighting.mAmbient = daylight.mAmbient;
            lighting.mDaylight = daylight;
            lighting.mFog = daylight.mFog;
            lighting.mDay = day;
            lighting.mHour = hour;
        }
    }

    void relight(CellLighting& lighting, std::string_view weather, int day, float hour)
    {
        if (!lighting.mOutdoors)
            return;

        settle(lighting, Rtx::makeDaylight(weather, hour), day, hour);

        // The name reached `makeDaylight` intact, so it is one of the ten.
        lighting.mWeather = Rtx::weatherIndex(weather).value();
        lighting.mNextWeather = lighting.mWeather;
        lighting.mWeatherBlend = 0.0f;
        lighting.mWindSpeed = Weather::windSpeed(weather);
        lighting.mGlare = Rtx::glareView(weather);
        lighting.mCloudSpeed = Sky::cloudSpeed(weather);
    }

    void relight(CellLighting& lighting, std::string_view from, std::string_view to, float blend, int day, float hour)
    {
        if (!lighting.mOutdoors)
            return;

        settle(lighting, Rtx::makeDaylight(from, to, blend, hour), day, hour);

        lighting.mWeather = Rtx::weatherIndex(from).value();
        lighting.mNextWeather = Rtx::weatherIndex(to).value();
        lighting.mWeatherBlend = blend;

        // The wind is one of the quantities the engine mixes across a transition too, and so are
        // the glare and the deck's speed.
        const auto mix = [blend](float x, float y) { return x * (1.0f - blend) + y * blend; };
        lighting.mWindSpeed = mix(Weather::windSpeed(from), Weather::windSpeed(to));
        lighting.mGlare = mix(Rtx::glareView(from), Rtx::glareView(to));
        lighting.mCloudSpeed = mix(Sky::cloudSpeed(from), Sky::cloudSpeed(to));
    }

    void applyLighting(const CellLighting& lighting, Rtx::Shaders::VisibilityConstants& constants)
    {
        Rtx::FrameWorld world{
            .mSun = lighting.mDaylight.mSun,
            .mAmbient = lighting.mAmbient,
            .mSkyHorizon = lighting.mDaylight.mSkyHorizon,
            .mSkyZenith = lighting.mDaylight.mSkyZenith,
            .mAir = lighting.mFog,
            .mWaterLevel = lighting.mWaterLevel,
            .mSeconds = lighting.mSeconds,

            // A settled sky names the same weather twice at a blend of nothing, which is what a
            // `shot` always is; a window running a transition says where it has got to.
            .mWeather = lighting.mWeather,
            .mNextWeather = lighting.mNextWeather,
            .mWeatherBlend = lighting.mWeatherBlend,

            .mWindSpeed = lighting.mWindSpeed,

            // **Asked of the eye, which is the only body standing in this weather.** The game aims
            // an ashstorm at the player; every caller here has already put its camera in `mOrigin`,
            // so the same rule reaches the same answer for whoever is looking.
            .mStormDirection = Rtx::stormDirection(lighting.mWeather, constants.mOrigin),
        };

        // **The sky's own two layers, and an interior has neither.** A room has no deck over it and
        // no stars in it, and the defaults are what say so — a texture slot of `NO_TEXTURE` and
        // a fade of nothing, which the shader skips before it samples anything.
        if (lighting.mOutdoors)
        {
            world.mClouds = Rtx::describeClouds(lighting.mWeather, lighting.mNextWeather, lighting.mWeatherBlend,
                lighting.mDaylight.mHaze, world.mStormDirection, lighting.mRoll.mClouds, lighting.mSky);

            world.mStars = Rtx::describeStars(
                lighting.mDaylight.mStarFade, lighting.mGlare, lighting.mRoll.mStars, lighting.mSky);

            Rtx::describePatches(lighting.mRoll.mStars, lighting.mSky, world.mSkyPatches);
        }

        // **Left where a default leaves them for a room**, which is an alpha of nothing and so a
        // disc the sky skips: an interior has no moons over it, and `relight` will not put any there.
        if (lighting.mOutdoors)
            for (const Rtx::Moon moon : { Rtx::Moon::Masser, Rtx::Moon::Secunda })
            {
                Rtx::MoonPlacement placed = Rtx::makeMoon(moon, lighting.mDay, lighting.mHour);
                placed.mFace = lighting.mFaces.of(moon);
                world.mMoons[static_cast<std::size_t>(moon)] = placed;
            }

        Rtx::applyWorld(world, constants);
    }
}
