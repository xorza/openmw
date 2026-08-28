#include <gtest/gtest.h>

#include <apps/rtxtool/worldclock.hpp>

namespace RtxTool
{
    namespace
    {
        /// The clock runs at thirty times the game, which is what its two readings are apart by.
        ///
        /// **Derived and not picked**: a quarter of an hour a second is nine hundred game seconds a
        /// real one, over Morrowind's timescale of thirty.
        TEST(RtxToolWorldClockTest, theRunningClockIsThirtyTimesTheGame)
        {
            EXPECT_FLOAT_EQ(WorldClock::sSpeed, 30.0f);
        }

        /// Stopped, the world runs at the wall and the hour stands still.
        TEST(RtxToolWorldClockTest, stoppedTheWorldRunsAtTheWallAndTheHourStandsStill)
        {
            WorldClock clock(3, 12.0f);
            for (int step = 0; step < 4; ++step)
                clock.advance(0.05f);

            EXPECT_FLOAT_EQ(clock.getStep(), 0.05f);
            EXPECT_FLOAT_EQ(clock.getWallSeconds(), 0.2f);
            EXPECT_FLOAT_EQ(clock.getWorldSeconds(), 0.2f);
            EXPECT_FLOAT_EQ(clock.getHour(), 12.0f);
            EXPECT_EQ(clock.getDay(), 3);
            EXPECT_EQ(clock.getTimeScale(), 0.0f) << "the stars stand with the hour";
        }

        /// Running, the world runs thirty times the wall and the hour a quarter a second — and the
        /// wall keeps counting at its own rate underneath, which is what the actors walk on.
        TEST(RtxToolWorldClockTest, runningTheWorldRunsThirtyTimesTheWallAndTheHourAQuarterASecond)
        {
            WorldClock clock(0, 12.0f);
            clock.toggle();
            for (int step = 0; step < 10; ++step)
                clock.advance(0.1f);

            EXPECT_FLOAT_EQ(clock.getWallSeconds(), 1.0f);
            EXPECT_FLOAT_EQ(clock.getWorldSeconds(), 30.0f);
            // Ten steps through two `fmod`s each, so a hundredth of a second of drift is the float.
            EXPECT_NEAR(clock.getHour(), 12.25f, 1e-4f);
            EXPECT_FLOAT_EQ(clock.getTimeScale(), 900.0f) << "nine hundred game seconds a real one";
        }

        /// A frame that spent five seconds loading a cell is a stall, and not thirty times five
        /// seconds of weather.
        TEST(RtxToolWorldClockTest, aStallIsNotAMinuteOfWeather)
        {
            WorldClock clock(0, 0.0f);
            clock.toggle();
            clock.advance(5.0f);

            EXPECT_FLOAT_EQ(clock.getStep(), WorldClock::sLongestStep);
            EXPECT_FLOAT_EQ(clock.getWallSeconds(), 0.1f);
            EXPECT_FLOAT_EQ(clock.getWorldSeconds(), 3.0f);
        }

        /// A nudge moves the hour round the clock and the day never before the first, and neither
        /// moves a second of the world: that is what makes two hours comparable.
        TEST(RtxToolWorldClockTest, aNudgeMovesTheHourAndNothingElse)
        {
            WorldClock clock(0, 0.5f);
            clock.advance(0.05f);

            clock.nudgeHour(-1.0f);
            EXPECT_FLOAT_EQ(clock.getHour(), 23.5f) << "an hour back from half past midnight";
            clock.nudgeHour(1.0f);
            EXPECT_FLOAT_EQ(clock.getHour(), 0.5f);

            clock.nudgeDay(-1);
            EXPECT_EQ(clock.getDay(), 0) << "no day before the world began";
            clock.nudgeDay(2);
            EXPECT_EQ(clock.getDay(), 2);

            EXPECT_FLOAT_EQ(clock.getWorldSeconds(), 0.05f);
            EXPECT_FLOAT_EQ(clock.getWallSeconds(), 0.05f);
        }

        /// The running clock's own hour wraps the same way a nudge does.
        TEST(RtxToolWorldClockTest, theRunningHourWrapsAtMidnight)
        {
            WorldClock clock(0, 23.99f);
            clock.toggle();
            clock.advance(0.1f);

            EXPECT_NEAR(clock.getHour(), 0.015f, 1e-4f) << "23.99 plus a fortieth, round the clock";
        }
    }
}
