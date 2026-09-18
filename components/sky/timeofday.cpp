#include "timeofday.hpp"

#include <string>

#include <components/fallback/fallback.hpp>

namespace Sky
{
    namespace
    {
        float lerp(float x, float y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }

        osg::Vec4f lerp(const osg::Vec4f& x, const osg::Vec4f& y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }
    }

    TimeOfDaySettings TimeOfDaySettings::fromFallback()
    {
        const float sunrise = Fallback::Map::getFloat("Weather_Sunrise_Time");
        const float sunset = Fallback::Map::getFloat("Weather_Sunset_Time");

        TimeOfDaySettings settings;
        settings.mSunriseDuration = Fallback::Map::getFloat("Weather_Sunrise_Duration");
        settings.mNightStart = sunset + Fallback::Map::getFloat("Weather_Sunset_Duration");
        settings.mNightEnd = sunrise;
        settings.mDayStart = sunrise + settings.mSunriseDuration;
        settings.mDayEnd = sunset;

        settings.addSetting("Sky");
        settings.addSetting("Ambient");
        settings.addSetting("Fog");
        settings.addSetting("Sun");

        // Morrowind handles stars settings differently for other ones
        settings.mStarsPostSunsetStart = Fallback::Map::getFloat("Weather_Stars_Post-Sunset_Start");
        settings.mStarsPreSunriseFinish = Fallback::Map::getFloat("Weather_Stars_Pre-Sunrise_Finish");
        settings.mStarsFadingDuration = Fallback::Map::getFloat("Weather_Stars_Fading_Duration");

        WeatherSetting starSetting
            = { settings.mStarsPreSunriseFinish, settings.mStarsFadingDuration - settings.mStarsPreSunriseFinish,
                  settings.mStarsPostSunsetStart, settings.mStarsFadingDuration - settings.mStarsPostSunsetStart };

        settings.mSunriseTransitions["Stars"] = starSetting;

        return settings;
    }

    template <typename T>
    T TimeOfDayInterpolator<T>::getValue(
        const float gameHour, const TimeOfDaySettings& timeSettings, const std::string& prefix) const
    {
        WeatherSetting setting = timeSettings.getSetting(prefix);
        float preSunriseTime = setting.mPreSunriseTime;
        float postSunriseTime = setting.mPostSunriseTime;
        float preSunsetTime = setting.mPreSunsetTime;
        float postSunsetTime = setting.mPostSunsetTime;

        // night
        if (gameHour < timeSettings.mNightEnd - preSunriseTime || gameHour > timeSettings.mNightStart + postSunsetTime)
            return mNightValue;
        // sunrise
        else if (gameHour >= timeSettings.mNightEnd - preSunriseTime
            && gameHour <= timeSettings.mDayStart + postSunriseTime)
        {
            float duration = timeSettings.mDayStart + postSunriseTime - timeSettings.mNightEnd + preSunriseTime;
            float middle = timeSettings.mNightEnd - preSunriseTime + duration / 2.f;

            if (gameHour <= middle)
            {
                // fade in
                float advance = middle - gameHour;
                float factor = 0.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunriseValue, mNightValue, factor);
            }
            else
            {
                // fade out
                float advance = gameHour - middle;
                float factor = 1.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunriseValue, mDayValue, factor);
            }
        }
        // day
        else if (gameHour > timeSettings.mDayStart + postSunriseTime && gameHour < timeSettings.mDayEnd - preSunsetTime)
            return mDayValue;
        // sunset
        else if (gameHour >= timeSettings.mDayEnd - preSunsetTime
            && gameHour <= timeSettings.mNightStart + postSunsetTime)
        {
            float duration = timeSettings.mNightStart + postSunsetTime - timeSettings.mDayEnd + preSunsetTime;
            float middle = timeSettings.mDayEnd - preSunsetTime + duration / 2.f;

            if (gameHour <= middle)
            {
                // fade in
                float advance = middle - gameHour;
                float factor = 0.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunsetValue, mDayValue, factor);
            }
            else
            {
                // fade out
                float advance = gameHour - middle;
                float factor = 1.f;
                if (duration > 0)
                    factor = advance / duration * 2;
                return lerp(mSunsetValue, mNightValue, factor);
            }
        }
        // shut up compiler
        return T();
    }

    template class TimeOfDayInterpolator<float>;
    template class TimeOfDayInterpolator<osg::Vec4f>;
}
