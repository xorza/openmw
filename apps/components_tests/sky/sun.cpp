#include <cmath>

#include <gtest/gtest.h>

#include <components/sky/sun.hpp>

namespace Sky
{
    namespace
    {
        /// Morrowind's own hours, built rather than read.
        ///
        /// **Nothing here touches `Fallback::Map`.** It is one global map for the whole binary,
        /// planted before any test runs and added to by whichever of them opens the real
        /// installation — so a test reading it is reading a configuration rather than its own
        /// premise.
        TimeOfDaySettings vanillaHours()
        {
            TimeOfDaySettings times{};
            times.mNightEnd = 6.0f;
            times.mDayStart = 8.0f;
            times.mDayEnd = 18.0f;
            times.mNightStart = 20.0f;
            return times;
        }

        /// How fast the disc falls, checked against the arc the weather manager walks.
        ///
        /// **A layer above the ground needs this because the engine's sunset is a clock.**
        /// `sunShareAt` ramps on the hour and nothing anywhere takes an elevation, so anything that
        /// keeps the sun a fraction of a degree longer has to convert that into hours.
        ///
        /// The arc is `MWWorld::WeatherManager::update`'s: the disc stands at `400 - |east|` over
        /// `(east, 75)`, with `east` running from 400 to -400 across the day. A difference over a
        /// hundredth of an hour, taken just inside sunset and just after sunrise, because the rate
        /// is constant and the two ends have to agree.
        TEST(RtxSunDescentTest, theDiscFallsAtTheRateTheWeatherManagersArcWalksAt)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float rate = sunDescentPerHour(times);

            const auto elevationAt = [&times](float hour) {
                const float orbit = 1.0f - 2.0f * (hour - times.mNightEnd) / (times.mNightStart - times.mNightEnd);
                osg::Vec3f position(400.0f * orbit, -75.0f, 0.0f);
                position.z() = 400.0f - std::abs(position.x());
                position.normalize();
                return std::asin(position.z());
            };

            constexpr float step = 0.01f;
            const float setting = (elevationAt(times.mNightStart - step) - elevationAt(times.mNightStart)) / step;
            const float rising = (elevationAt(times.mNightEnd + step) - elevationAt(times.mNightEnd)) / step;

            EXPECT_NEAR(setting, rate, 1.0e-3f) << "the disc sets at a rate this does not describe";
            EXPECT_NEAR(rising, rate, 1.0e-3f) << "and rises at the same one";

            // Eight degrees an hour over the shipped fourteen-hour day, which is what makes the
            // 0.72-degree dip a cloud deck sees worth about five game minutes.
            EXPECT_NEAR(osg::RadiansToDegrees(rate), 8.04f, 0.01f);

            // A day with no length has no rate, rather than a division by one.
            TimeOfDaySettings still = times;
            still.mNightStart = still.mNightEnd;
            EXPECT_EQ(sunDescentPerHour(still), 0.0f);
        }

        /// How much of the sun there is, which is one number and the only one.
        ///
        /// **The bug this is here for is a shadow with no sun.** The engine states this twice — a
        /// disc alpha that runs on through the night and comes back at one, and a separate switch
        /// saying whether the sprite is drawn — because a rasterizer that has hidden the sprite has
        /// no use for the other answer. A tracer asks it to decide whether to cast, so the two
        /// halves are folded here: what is nought lights nothing, casts nothing and draws nothing.
        TEST(RtxSunShareTest, thereIsNoSunAtNightAndItLandsOnTheHorizonAtEitherEnd)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float sunrise = 0.5f * (times.mDayStart - times.mNightEnd);
            const float dusk = times.mNightStart - times.mDayEnd;
            ASSERT_GT(sunrise, 0.0f);
            ASSERT_GT(dusk, 0.0f);

            // The two hours the weather manager puts the disc level with the horizon, and there is
            // none of it at either — so it lands on the horizon rather than being switched off above
            // one.
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightEnd, times), 0.0f) << "sunrise";
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightStart, times), 0.0f) << "sunset";

            // **And nothing anywhere in between them the other way round**, which is the half the
            // engine keeps elsewhere: its own curve returns one at every one of these hours.
            for (const float hour : { 0.0f, 2.0f, 5.0f, 20.5f, 22.0f, 23.9f })
                EXPECT_FLOAT_EQ(sunShareAt(hour, times), 0.0f) << "at hour " << hour;

            // Linear in over the first half of the sunrise window, and it is the hour past dawn
            // rather than a fraction of anything — which is why Morrowind's two-hour sunrise arrives
            // at exactly one at the end of it.
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightEnd + 0.5f * sunrise, times), 0.5f * sunrise);
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightEnd + sunrise, times), sunrise);
            EXPECT_FLOAT_EQ(sunShareAt(12.0f, times), 1.0f) << "and all of it through the day";

            // **Squared on the way out**, so the sun holds most of itself and then goes quickly:
            // halfway through dusk it is still three quarters there, `1 - 0.5^2`.
            EXPECT_FLOAT_EQ(sunShareAt(times.mDayEnd + 0.5f * dusk, times), 0.75f);
            EXPECT_LT(sunShareAt(times.mDayEnd + 0.75f * dusk, times), 0.45f);

            // Never past one, whatever a file says the sunrise is worth. The engine's dawn ramp is
            // unbounded and it did not matter while it was only an alpha; it scales the sunlight now.
            TimeOfDaySettings slow = times;
            slow.mDayStart = slow.mNightEnd + 8.0f;
            for (float hour = slow.mNightEnd; hour < slow.mNightStart; hour += 0.25f)
                EXPECT_LE(sunShareAt(hour, slow), 1.0f) << "at hour " << hour;

            // And it moves without a step anywhere, which is what stops the shadows jumping when the
            // sun goes out: the two curves and the night all meet at nought.
            float previous = sunShareAt(0.0f, times);
            for (float hour = 0.01f; hour < 24.0f; hour += 0.01f)
            {
                const float at = sunShareAt(hour, times);
                EXPECT_LT(std::abs(at - previous), 0.02f) << "the sun stepped at hour " << hour;
                previous = at;
            }
        }
    }
}
