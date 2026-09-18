#ifndef OPENMW_COMPONENTS_SKY_TIMEOFDAY_H
#define OPENMW_COMPONENTS_SKY_TIMEOFDAY_H

#include <map>
#include <string>

#include <osg/Vec4f>

#include <components/fallback/fallback.hpp>

namespace Sky
{
    struct WeatherSetting
    {
        float mPreSunriseTime;
        float mPostSunriseTime;
        float mPreSunsetTime;
        float mPostSunsetTime;
    };

    /// Where the day's four phases begin and end, and how fast each quantity crosses them. Lifted
    /// from `MWWorld::WeatherManager` so the ray tracer can light an hour without the game; the
    /// names stay as upstream spells them.
    struct TimeOfDaySettings
    {
        float mNightStart;
        float mNightEnd;
        float mDayStart;
        float mDayEnd;

        /// `Weather_Sunrise_Duration` as recorded, which is `mDayStart - mNightEnd` before that
        /// subtraction rounds: the disc's alpha ramps over half of it, and the weather manager
        /// reads the recorded number.
        float mSunriseDuration;

        std::map<std::string, WeatherSetting> mSunriseTransitions;

        float mStarsPostSunsetStart;
        float mStarsPreSunriseFinish;
        float mStarsFadingDuration;

        WeatherSetting getSetting(const std::string& type) const
        {
            std::map<std::string, WeatherSetting>::const_iterator it = mSunriseTransitions.find(type);
            if (it != mSunriseTransitions.end())
            {
                return it->second;
            }
            else
            {
                return { 1.f, 1.f, 1.f, 1.f };
            }
        }

        void addSetting(const std::string& type)
        {
            WeatherSetting setting = { Fallback::Map::getFloat("Weather_" + type + "_Pre-Sunrise_Time"),
                Fallback::Map::getFloat("Weather_" + type + "_Post-Sunrise_Time"),
                Fallback::Map::getFloat("Weather_" + type + "_Pre-Sunset_Time"),
                Fallback::Map::getFloat("Weather_" + type + "_Post-Sunset_Time") };

            mSunriseTransitions[type] = setting;
        }

        /// The whole of it, out of the `Weather_*` fallback settings: the assembly the weather
        /// manager's constructor used to do, so that a harness with no weather manager ramps its
        /// dawn on the same hours as the game.
        static TimeOfDaySettings fromFallback();
    };

    /// Interpolates between 4 data points (sunrise, day, sunset, night) based on the time of day.
    /// The template value could be a floating point number, or a color.
    template <typename T>
    class TimeOfDayInterpolator
    {
    public:
        TimeOfDayInterpolator(const T& sunrise, const T& day, const T& sunset, const T& night)
            : mSunriseValue(sunrise)
            , mDayValue(day)
            , mSunsetValue(sunset)
            , mNightValue(night)
        {
        }

        T getValue(const float gameHour, const TimeOfDaySettings& timeSettings, const std::string& prefix) const;

        const T& getSunriseValue() const { return mSunriseValue; }
        const T& getDayValue() const { return mDayValue; }
        const T& getSunsetValue() const { return mSunsetValue; }
        const T& getNightValue() const { return mNightValue; }

        void setSunriseValue(const T& sunriseValue) { mSunriseValue = sunriseValue; }
        void setDayValue(const T& dayValue) { mDayValue = dayValue; }
        void setSunsetValue(const T& sunsetValue) { mSunsetValue = sunsetValue; }
        void setNightValue(const T& nightValue) { mNightValue = nightValue; }

    private:
        T mSunriseValue, mDayValue, mSunsetValue, mNightValue;
    };
}

#endif
