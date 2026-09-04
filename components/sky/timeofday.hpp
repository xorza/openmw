#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <osg/Vec4f>

namespace Sky
{
    /// Which quantity a window belongs to.
    ///
    /// **A closed set, because the content records exactly these five.** Every one of them is
    /// spelled into four fallback keys and read back by name, so the name is the content's and the
    /// enumerator is what the code carries between the two.
    enum class DayPhaseOf : std::uint8_t
    {
        Sky,
        Ambient,
        Fog,
        Sun,
        Stars,
    };

    inline constexpr std::size_t sDayPhaseCount = 5;

    /// The name the content records that quantity's four keys under.
    std::string_view nameOf(DayPhaseOf of);

    /// The quantity one of those names stands for. Empty for a name none of them is.
    std::optional<DayPhaseOf> dayPhaseOf(std::string_view name);

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

        /// One window per quantity, indexed by `DayPhaseOf`, empty where nothing recorded one.
        ///
        /// **Empty and `{1,1,1,1}` are the same answer and still not the same thing.** A quantity
        /// with no window crosses instantly at the boundary, which is what an hour either side
        /// comes to once the phases are this wide — so a dropped reading cannot be told from a
        /// recorded window by what `getSetting` answers, and `hasSetting` is what tells them apart.
        std::array<std::optional<WeatherSetting>, sDayPhaseCount> mSunriseTransitions;

        float mStarsPostSunsetStart;
        float mStarsPreSunriseFinish;
        float mStarsFadingDuration;

        /// A quantity nothing recorded a window for crosses instantly at the boundary, which is what
        /// a window of one hour either side comes to once the phases are this wide.
        WeatherSetting getSetting(DayPhaseOf of) const
        {
            return mSunriseTransitions[static_cast<std::size_t>(of)].value_or(WeatherSetting{ 1.f, 1.f, 1.f, 1.f });
        }

        /// The same by the content's own name, which is how the ramps ask: a name none of the five
        /// is answers the same window as one nothing recorded.
        WeatherSetting getSetting(std::string_view type) const
        {
            const std::optional<DayPhaseOf> of = dayPhaseOf(type);
            return of.has_value() ? getSetting(*of) : WeatherSetting{ 1.f, 1.f, 1.f, 1.f };
        }

        /// Whether a window was recorded for `of` at all. See `mSunriseTransitions`.
        bool hasSetting(DayPhaseOf of) const { return mSunriseTransitions[static_cast<std::size_t>(of)].has_value(); }

        void setSetting(DayPhaseOf of, const WeatherSetting& window)
        {
            mSunriseTransitions[static_cast<std::size_t>(of)] = window;
        }

        /// Reads `of`'s four keys out of the fallback map and records the window they make.
        void addSetting(DayPhaseOf of);

        /// The whole of it, out of the `Weather_*` settings.
        ///
        /// **One assembly and two callers.** The game builds this in its weather manager's
        /// constructor and `openmw-rtxtool` has no weather manager, so the reading sits here rather
        /// than in either of them — a harness that assembled its own would ramp its dawn on
        /// different hours from the game's and nobody would notice until two screenshots disagreed.
        static TimeOfDaySettings fromFallback();

        /// The one reading of them, made on first use.
        ///
        /// **Every caller wants the same answer and the reading is not free**: it walks the fallback
        /// map a dozen times over built-up strings and fills a small table. That is nothing at load
        /// and an allocation per frame in a window that asks the hour for its sky. The content
        /// cannot change while the process runs, so one reading serves all of it.
        ///
        /// First called after the configuration is read, which every renderer is.
        static const TimeOfDaySettings& shared();
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
        T getValue(const float gameHour, const TimeOfDaySettings& timeSettings, std::string_view prefix) const;

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

    /// The four-point ramp a weather records for one of its coloured quantities.
    ///
    /// **Which four keys make a ramp, said once.** The game builds one of these per weather at
    /// startup and `openmw-rtxtool` builds one per reading, and a second spelling of
    /// `Weather_<name>_<quantity>_<phase>_Color` is a key one of them could get wrong on its own.
    ///
    /// @param weather a weather's name as the fallback settings spell it — "Clear", "Overcast" and
    ///        the rest. A name the map does not whitelist throws out of it.
    /// @param quantity "Sky", "Fog", "Ambient" or "Sun", which is also the window it crosses dawn
    ///        over — `TimeOfDayInterpolator::getValue` takes the same word.
    TimeOfDayInterpolator<osg::Vec4f> colourRamp(std::string_view weather, std::string_view quantity);

    /// The land fog's own ramp, which the content records two points of rather than four.
    ///
    /// **The day depth serves three of the four**, so the layer holds through sunrise and the whole
    /// day and crosses to the night depth at dusk. Asking for a sunrise depth is not a key that
    /// reads nought: the fallback map does not know it at all and throws.
    TimeOfDayInterpolator<float> landFogRamp(std::string_view weather);
}
