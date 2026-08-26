#pragma once

#include <map>
#include <string>

#include <osg/Vec4f>

namespace Sky
{
    /// How far either side of a boundary one quantity takes to change.
    ///
    /// **Every quantity crosses dawn at its own pace.** The sky, the ambient, the fog, the sun and
    /// the stars each carry their own four numbers, so the sun can be up before the sky has finished
    /// turning and the stars can outlast both.
    struct WeatherSetting
    {
        float mPreSunriseTime;
        float mPostSunriseTime;
        float mPreSunsetTime;
        float mPostSunsetTime;
    };

    /// Where the day's four phases begin and end, and what crosses them how fast.
    struct TimeOfDaySettings
    {
        float mNightStart;
        float mNightEnd;
        float mDayStart;
        float mDayEnd;

        std::map<std::string, WeatherSetting> mSunriseTransitions;

        float mStarsPostSunsetStart;
        float mStarsPreSunriseFinish;
        float mStarsFadingDuration;

        /// A quantity nothing recorded a window for crosses instantly at the boundary, which is what
        /// a window of one hour either side comes to once the phases are this wide.
        WeatherSetting getSetting(const std::string& type) const
        {
            const auto it = mSunriseTransitions.find(type);
            return it != mSunriseTransitions.end() ? it->second : WeatherSetting{ 1.f, 1.f, 1.f, 1.f };
        }

        void addSetting(const std::string& type);

        /// The whole of it, out of the `Weather_*` settings.
        ///
        /// **One assembly and two callers.** The game builds this in its weather manager's
        /// constructor and `openmw-rtxtool` has no weather manager, so the reading sits here rather
        /// than in either of them — a harness that assembled its own would ramp its dawn on
        /// different hours from the game's and nobody would notice until two screenshots disagreed.
        static TimeOfDaySettings fromFallback();
    };

    /// One quantity at the four times of day, read at any hour between them.
    ///
    /// Sunrise, day, sunset and night are what a content file records; everything in between is this
    /// crossfading — into the sunrise value from the night's on the way up and out of it into the
    /// day's, and the mirror of that at dusk.
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

        /// @param prefix which quantity this is — "Sky", "Ambient", "Fog", "Sun" or "Stars" — which
        ///        is what picks the window it crosses the boundaries over.
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
