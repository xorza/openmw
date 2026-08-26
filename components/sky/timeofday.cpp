#include "timeofday.hpp"

#include <components/fallback/fallback.hpp>

namespace Sky
{
    namespace
    {
        /// Linear interpolation between x and y. Two of them because a colour is four floats and a
        /// value is one, and the ramp below is written once for both.
        float lerp(float x, float y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }

        osg::Vec4f lerp(const osg::Vec4f& x, const osg::Vec4f& y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }
    }

    void TimeOfDaySettings::addSetting(const std::string& type)
    {
        mSunriseTransitions[type] = WeatherSetting{ Fallback::Map::getFloat("Weather_" + type + "_Pre-Sunrise_Time"),
            Fallback::Map::getFloat("Weather_" + type + "_Post-Sunrise_Time"),
            Fallback::Map::getFloat("Weather_" + type + "_Pre-Sunset_Time"),
            Fallback::Map::getFloat("Weather_" + type + "_Post-Sunset_Time") };
    }

    TimeOfDaySettings TimeOfDaySettings::fromFallback()
    {
        const float sunrise = Fallback::Map::getFloat("Weather_Sunrise_Time");
        const float sunset = Fallback::Map::getFloat("Weather_Sunset_Time");

        TimeOfDaySettings settings;
        settings.mNightStart = sunset + Fallback::Map::getFloat("Weather_Sunset_Duration");
        settings.mNightEnd = sunrise;
        settings.mDayStart = sunrise + Fallback::Map::getFloat("Weather_Sunrise_Duration");
        settings.mDayEnd = sunset;

        settings.addSetting("Sky");
        settings.addSetting("Ambient");
        settings.addSetting("Fog");
        settings.addSetting("Sun");

        settings.mStarsPostSunsetStart = Fallback::Map::getFloat("Weather_Stars_Post-Sunset_Start");
        settings.mStarsPreSunriseFinish = Fallback::Map::getFloat("Weather_Stars_Pre-Sunrise_Finish");
        settings.mStarsFadingDuration = Fallback::Map::getFloat("Weather_Stars_Fading_Duration");

        // The stars' own window is derived rather than recorded: they begin after sunset and finish
        // before sunrise, and the fading duration is what is left of each.
        settings.mSunriseTransitions["Stars"] = WeatherSetting{ settings.mStarsPreSunriseFinish,
            settings.mStarsFadingDuration - settings.mStarsPreSunriseFinish, settings.mStarsPostSunsetStart,
            settings.mStarsFadingDuration - settings.mStarsPostSunsetStart };

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
