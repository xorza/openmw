#include <gtest/gtest.h>

#include <components/fallback/fallback.hpp>
#include <components/sky/timeofday.hpp>

namespace Sky
{
    namespace
    {
        /// The day's phases come out of the four keys that record them, each on its own side. Read
        /// against `Fallback::Map` rather than against numbers, because whichever test in this
        /// binary seeds the map first decides what every later one sees.
        TEST(SkyTimeOfDayTest, theFourPhasesAreTheRecordedTimesAndTheirDurations)
        {
            const TimeOfDaySettings times = TimeOfDaySettings::fromFallback();

            const float sunrise = Fallback::Map::getFloat("Weather_Sunrise_Time");
            const float sunset = Fallback::Map::getFloat("Weather_Sunset_Time");

            EXPECT_EQ(times.mNightEnd, sunrise) << "night ends when the sun starts to rise";
            EXPECT_EQ(times.mDayEnd, sunset) << "day ends when it starts to set";
            EXPECT_EQ(times.mDayStart, sunrise + Fallback::Map::getFloat("Weather_Sunrise_Duration"));
            EXPECT_EQ(times.mNightStart, sunset + Fallback::Map::getFloat("Weather_Sunset_Duration"));
        }

        /// Every quantity the interpolators ask for has a window, and a missing one is silent:
        /// `getSetting` answers an hour either side for a name it does not hold, which is a usable
        /// window and so cannot be told from a recorded one by its value. Asked of the cache as
        /// well as of a fresh reading, and the two are not compared: `shared()` is filled on first
        /// use, and a test in this binary opens the real installation after that.
        TEST(SkyTimeOfDayTest, everyQuantityTheRampReadsCarriesAWindowOfItsOwn)
        {
            for (const TimeOfDaySettings& times : { TimeOfDaySettings::fromFallback(), TimeOfDaySettings::shared() })
                for (const char* quantity : { "Sky", "Ambient", "Fog", "Sun", "Stars" })
                    EXPECT_TRUE(times.mSunriseTransitions.contains(quantity)) << quantity << " has no window";
        }

        /// The stars' window is derived from their three recorded numbers rather than read: when
        /// they begin after sunset, when they finish before sunrise, and how long each fade lasts.
        TEST(SkyTimeOfDayTest, theStarsWindowIsWhatIsLeftOfTheirFade)
        {
            for (const TimeOfDaySettings& times : { TimeOfDaySettings::fromFallback(), TimeOfDaySettings::shared() })
            {
                const WeatherSetting stars = times.getSetting("Stars");

                EXPECT_EQ(stars.mPreSunriseTime, times.mStarsPreSunriseFinish);
                EXPECT_EQ(stars.mPostSunriseTime, times.mStarsFadingDuration - times.mStarsPreSunriseFinish);
                EXPECT_EQ(stars.mPreSunsetTime, times.mStarsPostSunsetStart);
                EXPECT_EQ(stars.mPostSunsetTime, times.mStarsFadingDuration - times.mStarsPostSunsetStart);
            }
        }
    }
}
