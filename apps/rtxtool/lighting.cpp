#include "lighting.hpp"

#include <array>
#include <cstddef>

#include <components/rtx/frameworld.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/sky/clouds.hpp>

#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        void settle(CellLighting& lighting, const Rtx::Daylight& daylight, int day, float hour)
        {
            lighting.mDaylight = daylight;
            lighting.mDay = day;
            lighting.mHour = hour;
        }
    }

    const Rtx::WeatherRamps& HeldWeathers::of(std::string_view weather)
    {
        for (std::size_t at = 0; at < sSlots; ++at)
            if (mHeld[at].has_value() && mNames[at] == weather)
                return *mHeld[at];

        const std::size_t slot = mNext;
        mNext = (mNext + 1) % sSlots;

        mNames[slot] = weather;
        mHeld[slot] = Rtx::readWeatherRamps(weather);

        return *mHeld[slot];
    }

    void relight(CellLighting& lighting, HeldWeathers& held, const SkyMoment& moment)
    {
        if (!lighting.mOutdoors)
            return;

        const Rtx::WeatherRamps& ramps = held.of(moment.mWeather);
        settle(lighting, Rtx::makeDaylight(ramps, moment.mHour, landReach()), moment.mDay, moment.mHour);

        // The name reached `readWeatherRamps` intact, so it is one of the ten.
        lighting.mWeather = Rtx::weatherIndex(moment.mWeather).value();
        lighting.mNextWeather = lighting.mWeather;
        lighting.mWeatherBlend = 0.0f;
        lighting.mCloudBlend = 0.0f;
        lighting.mGlare = ramps.mGlare;
        lighting.mCloudSpeed = ramps.mCloudSpeed;
    }

    void relight(
        CellLighting& lighting, HeldWeathers& held, const SkyMoment& moment, std::string_view into, float blend)
    {
        if (!lighting.mOutdoors)
            return;

        const Rtx::WeatherRamps& before = held.of(moment.mWeather);
        const Rtx::WeatherRamps& after = held.of(into);

        settle(lighting, Rtx::makeDaylight(before, after, blend, moment.mHour, landReach()), moment.mDay, moment.mHour);

        lighting.mWeather = Rtx::weatherIndex(moment.mWeather).value();
        lighting.mNextWeather = Rtx::weatherIndex(into).value();
        lighting.mWeatherBlend = blend;

        // The glare and the deck's speed are two of the quantities the engine mixes across a
        // transition.
        const auto mix = [blend](float x, float y) { return x * (1.0f - blend) + y * blend; };
        lighting.mGlare = mix(before.mGlare, after.mGlare);
        lighting.mCloudSpeed = mix(before.mCloudSpeed, after.mCloudSpeed);

        // **The deck's own crossing, which is not the weather's.** Each weather spreads its arrival
        // over a share of the transition, so a storm's sky rolls in ahead of its light —
        // `Sky::cloudBlend` is the curve the game runs and `WeatherResult::mCloudBlendFactor` is
        // where it reaches the deck there.
        lighting.mCloudBlend = Sky::cloudBlend(blend, after.mCloudsMaximumPercent);
    }

    void applyLighting(const CellLighting& lighting, Rtx::Shaders::VisibilityConstants& constants)
    {
        // **Worked out before the reading, because these two are what the game is handed rather
        // than derives.** A moon needs a date the sun never asked for, and a storm is aimed by a
        // weather system this harness does not run — so each side reaches them its own way and the
        // assembly takes them as they are.
        //
        // **Placed whatever the cell is, because whether a room has moons is not this caller's
        // answer to give.** `Rtx::describeWorld` takes the sky away from a room once, for both
        // hosts, and a second rule here is the same question with its own copy of the answer.
        std::array<Rtx::MoonPlacement, 2> moons{};
        for (const Rtx::Moon moon : { Rtx::Moon::Masser, Rtx::Moon::Secunda })
        {
            Rtx::MoonPlacement placed = Rtx::makeMoon(moon, lighting.mDay, lighting.mHour, lighting.mGlare);
            placed.mFace = lighting.mFaces.of(moon);
            moons[static_cast<std::size_t>(moon)] = placed;
        }

        // **Asked of the eye, which is the only body standing in this weather.** The game aims an
        // ashstorm at the player and reports where it settled; every caller here has already put
        // its camera in `mOrigin`, so the same rule reaches the same answer. A storm aimed at a
        // body under a roof is arithmetic nobody reads.
        osg::Vec3f storm;
        osg::Vec3f nextStorm;
        if (lighting.mOutdoors)
        {
            storm = Rtx::stormDirection(lighting.mWeather, constants.mOrigin);
            nextStorm = Rtx::stormDirection(lighting.mNextWeather, constants.mOrigin);
        }

        const Rtx::WorldReading reading{
            .mDaylight = lighting.mDaylight,
            .mOutdoors = lighting.mOutdoors,
            .mFogFromSky = lighting.mOutdoors,
            .mGlare = lighting.mGlare,
            .mStarRoll = lighting.mRoll.mStars,
            .mCloudRoll = lighting.mRoll.mClouds,
            .mSky = lighting.mSky,
            .mMoons = moons,
            .mWeather = lighting.mWeather,
            .mNextWeather = lighting.mNextWeather,
            .mCloudBlend = lighting.mCloudBlend,
            .mCloudDirection = storm,
            .mNextCloudDirection = nextStorm,
            .mWaterLevel = lighting.mWaterLevel,
            .mSeconds = lighting.mSeconds,
            .mRainOnWater = lighting.mRainOnWater,
        };

        Rtx::applyWorld(Rtx::describeWorld(reading), constants);
    }
}
