#include <chrono>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include <components/rtx/frameclock.hpp>

namespace Rtx
{
    namespace
    {
        /// A stated step is what every frame stands for, and what the time is counted in.
        ///
        /// **The measurement is handed in and ignored**, which is the whole of what makes a run
        /// repeatable: the clock says the same thing on a fast machine and a slow one.
        TEST(RtxFrameClockTest, aStatedStepIgnoresWhatTheWallSaid)
        {
            // **The step is a `float` and the time is a `double`**, because the step is a setting
            // and the time is a sum of thousands of them. So what is expected is the widened float
            // and not the double a reader would write out.
            constexpr float sixtieth = 1.0f / 60.0f;
            constexpr double widened = static_cast<double>(sixtieth);

            FrameClock clock(sixtieth);
            EXPECT_EQ(clock.getStep(), 0.0) << "nothing stands for anything before the first frame";
            EXPECT_EQ(clock.getNow(), 0.0);

            clock.advance(0.5);
            EXPECT_DOUBLE_EQ(clock.getStep(), widened) << "half a second on the wall, and a sixtieth here";
            EXPECT_DOUBLE_EQ(clock.getNow(), widened);

            clock.advance(0.001);
            EXPECT_DOUBLE_EQ(clock.getStep(), widened) << "and a millisecond on the wall is the same sixtieth";
            EXPECT_DOUBLE_EQ(clock.getNow(), widened * 2.0) << "the time is the frames counted";

            EXPECT_EQ(clock.getStatedStep(), std::optional<float>(sixtieth));
        }

        /// With nothing stated the wall decides, and the two answers come from two different places.
        ///
        /// **A stall should age a player's caches**, which is why the time is the wall's rather than
        /// the frames added up: `MWWorld::Scene` expires what it holds against this.
        TEST(RtxFrameClockTest, withNoStepStatedTheWallDecidesBothAnswers)
        {
            FrameClock clock;
            EXPECT_EQ(clock.getStatedStep(), std::nullopt) << "which is what says the run does not repeat itself";

            clock.advance(0.25);
            EXPECT_DOUBLE_EQ(clock.getStep(), 0.25) << "the frame stands for what it was measured at";

            // Slept rather than asserted against a constant: what `getNow` reports is elapsed time,
            // so the claim is that it moved and not by how much.
            const double before = clock.getNow();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            clock.advance(0.25);

            EXPECT_GT(clock.getNow(), before) << "the wall moved while the frame did not";

            // Half a second of steps went in and milliseconds of wall passed, so a clock adding its
            // steps up would be past 0.5 and this one is nowhere near it.
            EXPECT_LT(clock.getNow(), 0.1) << "the time is the wall's and not the two steps added up";
        }

        /// The two modes disagree, which is what makes the choice worth making.
        TEST(RtxFrameClockTest, aStatedStepAndTheWallGiveDifferentAnswersForOneFrame)
        {
            FrameClock stated(1.0f / 60.0f);
            FrameClock walled;

            stated.advance(0.5);
            walled.advance(0.5);

            EXPECT_NE(stated.getStep(), walled.getStep());
            EXPECT_DOUBLE_EQ(walled.getStep(), 0.5);
        }
    }
}
