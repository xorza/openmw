#include <cstddef>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include <apps/components_tests/rtx/allocations.hpp>
#include <apps/openmw/mwrender/rtx/frametimer.hpp>
#include <components/rtx/latencyreport.hpp>

namespace MWRender
{
    namespace
    {
        /// A second closes on the frame that fills it, the title describes exactly that second, and
        /// the next second starts from nothing — without reaching the heap on the frame that closes
        /// one, which is a frame like any other.
        TEST(RtxFrameTimerTest, aSecondOfFramesClosesOneRateAndTheNextStartsFromNothing)
        {
            FrameTimer timer;
            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW") << "no second has run out";

            // Ninety-nine frames of ten milliseconds are 990 ms, which is short of a second.
            for (int at = 0; at < 99; ++at)
                EXPECT_FALSE(timer.addFrame(10.0)) << "frame " << at + 1 << " closed a second early";
            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW") << "an open second has no rate";

            const std::size_t before = Rtx::Testing::getAllocationCount();
            const bool closed = timer.addFrame(10.0);
            const std::string_view title = timer.writeTitle(std::nullopt, {});
            const std::size_t spent = Rtx::Testing::getAllocationCount() - before;

            EXPECT_TRUE(closed) << "the hundredth frame is the second";
            EXPECT_EQ(spent, 0u) << "closing a second reached the heap " << spent << " times";
            EXPECT_EQ(title, "OpenMW - 100 fps, 10.0 ms, worst 10.0 ms");

            // Forty frames alternating 5 and 45 ms sum to 20 x 5 + 20 x 45 = 1000 exactly, with a
            // mean of 25 — so the worst is the figure the mean hides. The first thirty-nine are
            // 20 x 5 + 19 x 45 = 955, which is still open.
            for (int at = 0; at < 39; ++at)
                EXPECT_FALSE(timer.addFrame(at % 2 == 0 ? 5.0 : 45.0)) << "frame " << at + 1 << " of the second second";
            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW - 100 fps, 10.0 ms, worst 10.0 ms")
                << "an open second leaves the rate alone";

            EXPECT_TRUE(timer.addFrame(45.0));
            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW - 40 fps, 25.0 ms, worst 45.0 ms")
                << "the first second's worst did not carry over";

            // A rate that does not divide a second: 117 x 8.5 = 994.5 is open and 118 x 8.5 = 1003
            // closes, at 1000 / 8.5 = 117.6 frames a second rounded to the nearest whole one.
            for (int at = 0; at < 117; ++at)
                EXPECT_FALSE(timer.addFrame(8.5));
            EXPECT_TRUE(timer.addFrame(8.5));
            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW - 118 fps, 8.5 ms, worst 8.5 ms");

            // One frame longer than a second is a second on its own: 1000 / 1500 rounds to one.
            EXPECT_TRUE(timer.addFrame(1500.0));
            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW - 1 fps, 1500.0 ms, worst 1500.0 ms");
        }

        /// Ten frames of a hundred milliseconds close a second, and the title of that second is the
        /// rate, then the driver's latency where it paces, then the run's note where it has one —
        /// each part left out where there is nothing to say, so a played game's title is the rate
        /// alone.
        TEST(RtxFrameTimerTest, theTitleIsTheRateThenTheLatencyThenTheNote)
        {
            FrameTimer timer;
            for (int frame = 0; frame < 9; ++frame)
                EXPECT_FALSE(timer.addFrame(100.0)) << "frame " << frame << " of a second not yet run out";
            ASSERT_TRUE(timer.addFrame(100.0));

            EXPECT_EQ(timer.writeTitle(std::nullopt, {}), "OpenMW - 10 fps, 100.0 ms, worst 100.0 ms");

            const Rtx::LatencyReport latency{ .mPresentId = 7, .mInputToPresentUs = 3240 };
            EXPECT_EQ(timer.writeTitle(latency, {}), "OpenMW - 10 fps, 100.0 ms, worst 100.0 ms, 3.2 ms latency");
            EXPECT_EQ(timer.writeTitle(std::nullopt, "Thunderstorm, 14:32"),
                "OpenMW - 10 fps, 100.0 ms, worst 100.0 ms - Thunderstorm, 14:32");

            const std::string_view whole = timer.writeTitle(latency, "Thunderstorm, 14:32");
            EXPECT_EQ(whole, "OpenMW - 10 fps, 100.0 ms, worst 100.0 ms, 3.2 ms latency - Thunderstorm, 14:32");
            EXPECT_EQ(whole.data()[whole.size()], '\0') << "ends in the nought a C string wants";
        }
    }
}
