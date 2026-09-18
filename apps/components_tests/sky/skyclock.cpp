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

        /// One frame at the shipped scale under Morrowind's fastest deck: the scroll moves by the
        /// frame, the stars by a sixtieth of a second of game time over the four days they take
        /// round, and the sky's seconds by the frame. Twice the scale doubles all three, so the
        /// scale is read; and the scroll wraps at four, where the sheet repeats.
        TEST(SkyClockTest, theClockStepsTheDeckTheStarsAndTheSecondsByTheFrame)
        {
            constexpr float step = 1.0f / 60.0f;
            constexpr float cloudSpeed = 400.0f;
            constexpr float fourDays = 3600.0f * 96.0f;

            SkyClock shipped;
            shipped.step(step, sVanillaTimeScale, cloudSpeed);
            EXPECT_FLOAT_EQ(shipped.mCloudScroll, step);
            EXPECT_FLOAT_EQ(shipped.mStarRoll, sVanillaTimeScale * step * osg::DegreesToRadians(360.0f) / fourDays);
            EXPECT_DOUBLE_EQ(shipped.mSeconds, static_cast<double>(step));

            SkyClock doubled;
            doubled.step(step, 2.0f * sVanillaTimeScale, cloudSpeed);
            EXPECT_FLOAT_EQ(doubled.mCloudScroll, 2.0f * step);
            EXPECT_FLOAT_EQ(doubled.mStarRoll, 2.0f * shipped.mStarRoll);
            EXPECT_DOUBLE_EQ(doubled.mSeconds, 2.0 * shipped.mSeconds);

            SkyClock wrapping;
            wrapping.mCloudScroll = 3.99f;
            wrapping.step(step, sVanillaTimeScale, cloudSpeed);
            EXPECT_FLOAT_EQ(wrapping.mCloudScroll, 3.99f + step - 4.0f);
        }
    }
}
