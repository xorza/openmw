#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include <apps/openmw/mwrender/rtx/frametimer.hpp>
#include <components/rtx/latencyreport.hpp>

namespace MWRender
{
    namespace
    {
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
