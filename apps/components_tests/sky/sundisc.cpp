#include <gtest/gtest.h>

#include <components/sky/sundisc.hpp>
#include <components/sky/timeofday.hpp>

namespace Sky
{
    namespace
    {
        /// Morrowind's shipped day: sunrise at six for two hours, sunset at eighteen for two.
        TimeOfDaySettings shippedDay()
        {
            TimeOfDaySettings times{};
            times.mNightEnd = 6.f;
            times.mSunriseDuration = 2.f;
            times.mDayStart = 8.f;
            times.mDayEnd = 18.f;
            times.mNightStart = 20.f;
            return times;
        }

        /// The disc is hidden from the night's start to its end, both ends inclusive, and shown
        /// between: `WeatherManager::update`'s own test, which is `>= mNightStart || <= mSunriseTime`.
        TEST(SkySunDiscTest, theSunIsUpBetweenTheEndOfOneNightAndTheStartOfTheNext)
        {
            const TimeOfDaySettings times = shippedDay();

            EXPECT_FALSE(sunUp(0.f, times));
            EXPECT_FALSE(sunUp(6.f, times)) << "the night's end is still night";
            EXPECT_TRUE(sunUp(6.01f, times));
            EXPECT_TRUE(sunUp(12.f, times));
            EXPECT_TRUE(sunUp(19.99f, times));
            EXPECT_FALSE(sunUp(20.f, times)) << "the night's start is night";
            EXPECT_FALSE(sunUp(23.f, times));
        }

        /// The alpha as the weather manager writes it: the hour past dawn over the first half of
        /// the sunrise window, one through the day, one less the square of the fade across dusk,
        /// and back to one through the night — the ramp knows nothing of the night; `sunUp` does.
        TEST(SkySunDiscTest, theDiscsAlphaIsTheWeatherManagersOwnRamp)
        {
            const TimeOfDaySettings times = shippedDay();

            // Sunrise: the hour past six, up to seven, which is half the two-hour window.
            EXPECT_FLOAT_EQ(sunDiscAlpha(6.f, times), 0.f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(6.25f, times), 0.25f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(7.f, times), 1.f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(7.5f, times), 1.f) << "past the window's half it is one";

            // A four-hour sunrise passes one at seven and is not clamped: an alpha of two is what
            // the game writes, and the tracer bounds it for itself.
            TimeOfDaySettings longDawn = times;
            longDawn.mSunriseDuration = 4.f;
            longDawn.mDayStart = 10.f;
            EXPECT_FLOAT_EQ(sunDiscAlpha(8.f, longDawn), 2.f);

            // Day.
            EXPECT_FLOAT_EQ(sunDiscAlpha(12.f, times), 1.f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(17.99f, times), 1.f);

            // Dusk: the fade is the share of the two hours gone, squared, so at nineteen the disc
            // is three quarters there and at twenty exactly nothing.
            EXPECT_FLOAT_EQ(sunDiscAlpha(18.f, times), 1.f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(19.f, times), 0.75f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(20.f, times), 0.f);

            // And the ramp alone says nothing about the night: the fade is held at one past dusk.
            EXPECT_FLOAT_EQ(sunDiscAlpha(23.f, times), 0.f);
            EXPECT_FLOAT_EQ(sunDiscAlpha(3.f, times), 1.f) << "before dawn the ramp is one; sunUp is the gate";
        }
    }
}
