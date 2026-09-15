#include <gtest/gtest.h>

#include <components/sky/skyclock.hpp>

namespace Sky
{
    namespace
    {
        /// At the shipped `timescale` the sky's clock is the simulation's own, bit for bit, which
        /// is what keeps every run made before it existed the same run; at any other it is the
        /// ratio, and a clock that stands or runs backwards holds the sky.
        TEST(SkyClockTest, theShippedTimescaleIsRealTimeAndAnyOtherIsItsRatio)
        {
            constexpr float step = 1.0f / 60.0f;

            EXPECT_EQ(skyStep(step, sVanillaTimeScale), step) << "not near: the same run";
            EXPECT_FLOAT_EQ(skyStep(step, 300.0f), step * 10.0f);
            EXPECT_FLOAT_EQ(skyStep(step, 3000.0f), step * 100.0f);
            EXPECT_FLOAT_EQ(skyStep(step, 15.0f), step * 0.5f);

            EXPECT_EQ(skyStep(step, 0.0f), 0.0f) << "a paused clock holds the sky";
            EXPECT_EQ(skyStep(step, -30.0f), 0.0f) << "and so does one running backwards";
        }
    }
}
