#include "downpour.hpp"

#include <algorithm>

#include <components/fallback/fallback.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/settings/values.hpp>

namespace Weather
{
    osg::Vec3f defaultStormDirection()
    {
        return osg::Vec3f(0.f, 1.f, 0.f);
    }

    std::string_view stormEffect(std::string_view weather)
    {
        if (Misc::StringUtils::ciEqual(weather, "Ashstorm"))
            return Settings::models().mWeatherashcloud.get();
        if (Misc::StringUtils::ciEqual(weather, "Blight"))
            return Settings::models().mWeatherblightcloud.get();
        if (Misc::StringUtils::ciEqual(weather, "Snow"))
            return Settings::models().mWeathersnow.get();
        if (Misc::StringUtils::ciEqual(weather, "Blizzard"))
            return Settings::models().mWeatherblizzard.get();

        return std::string_view();
    }

    float windSpeed(std::string_view weather)
    {
        return Fallback::Map::getFloat("Weather_" + std::string(weather) + "_Wind_Speed");
    }

    float gustSpeed(float baseWindSpeed)
    {
        return std::min(8.0f * baseWindSpeed, 70.0f);
    }

    osg::Vec3f stormDirection(std::string_view particleEffect, const osg::Vec3f& observer)
    {
        // **Not where Red Mountain is, and upstream moved it there on purpose.** OpenMW aimed ash
        // at the summit's real position until `e7208bb80e` (bug #4240) replaced (19950, 72032,
        // 27831) with this rounded, flattened pair — a storm aimed at the mountain's foot rather
        // than its peak is what makes a character on the coast shield their eyes in the right
        // direction. Vanilla numbers, kept because they are what the animation was tuned against.
        constexpr float mountainX = 25000.0f;
        constexpr float mountainY = 70000.0f;

        if (particleEffect != Settings::models().mWeatherashcloud.get()
            && particleEffect != Settings::models().mWeatherblightcloud.get())
            return defaultStormDirection();

        // Flat: a storm drives across the land, and the summit is the only place a height would
        // change the answer.
        osg::Vec3f away(observer.x() - mountainX, observer.y() - mountainY, 0.0f);

        // Standing on the summit, where the direction away from it is no direction at all.
        //
        // **The one place this does not answer what upstream answers.** `MWWorld` normalised
        // without asking, and `osg::Vec3f::normalize` leaves a zero vector zero — so a player at
        // exactly (25000, 70000) got a storm blowing nowhere, and whatever `makeRotate` does with
        // that. The ray tracer's own copy of this rule already guarded, so unifying the two had to
        // keep one behaviour or the other; north is the answer every weather that names no
        // direction gets, and the input is a point in the middle of Red Mountain's caldera.
        if (away.normalize() == 0.0f)
            return defaultStormDirection();

        return away;
    }

    Downpour downpourAt(std::string_view weather, float stormWindSpeed, float rainGravity)
    {
        const std::string name(weather);
        const float wind = windSpeed(weather);

        return Downpour{
            // **Morrowind hard-codes which weathers rain**, and the file it names is not the one the
            // engine draws: `Using_Precip` is the whole of the question, and the mesh below is the
            // answer upstream writes down for it.
            .mRainEffect = Fallback::Map::getBool("Weather_" + name + "_Using_Precip") ? "meshes\\raindrop.nif" : "",
            .mParticleEffect = std::string(stormEffect(weather)),
            .mPrecipitationAlpha = 1.f,
            .mRainSpeed = rainGravity,
            .mRainEntranceSpeed = Fallback::Map::getFloat("Weather_" + name + "_Rain_Entrance_Speed"),
            .mRainMaxRaindrops = Fallback::Map::getInt("Weather_" + name + "_Max_Raindrops"),
            .mRainDiameter = Fallback::Map::getFloat("Weather_" + name + "_Rain_Diameter"),
            .mRainMinHeight = Fallback::Map::getFloat("Weather_" + name + "_Rain_Height_Min"),
            .mRainMaxHeight = Fallback::Map::getFloat("Weather_" + name + "_Rain_Height_Max"),
            // **The gust and the recorded number, and they are not the same.** The first is what
            // leans the drops; the second is what the world is asked about.
            .mWindSpeed = gustSpeed(wind),
            .mBaseWindSpeed = wind,
            .mIsStorm = wind > stormWindSpeed,
        };
    }
}
