#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

#include <components/rtxbench/framerate.hpp>

#include "../rtx/allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// A line closes on the frame that fills a second, describes exactly that second, and the
        /// next second starts from nothing.
        TEST(RtxFrameRateTest, aSecondOfFramesClosesOneLineAndTheNextStartsFromNothing)
        {
            FrameRate rate;
            EXPECT_TRUE(rate.getText().empty()) << "nothing has closed";

            // Ninety-nine frames of ten milliseconds are 990 ms, which is short of a second.
            for (int at = 0; at < 99; ++at)
                EXPECT_FALSE(rate.add(10.0)) << "frame " << at + 1 << " closed a line early";
            EXPECT_TRUE(rate.getText().empty()) << "an open second has no line";

            const std::size_t before = Testing::getAllocationCount();
            const bool closed = rate.add(10.0);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_TRUE(closed) << "the hundredth frame is the second";
            EXPECT_EQ(spent, 0u) << "closing a line reached the heap " << spent << " times";
            EXPECT_EQ(rate.getText(), "100 fps, 10.0 ms, worst 10.0 ms");

            // Forty frames alternating 5 and 45 ms sum to 20 x 5 + 20 x 45 = 1000 exactly, with a
            // mean of 25 — so the worst is the figure the mean hides. The first thirty-nine are
            // 20 x 5 + 19 x 45 = 955, which is still open.
            for (int at = 0; at < 39; ++at)
                EXPECT_FALSE(rate.add(at % 2 == 0 ? 5.0 : 45.0)) << "frame " << at + 1 << " of the second second";
            EXPECT_EQ(rate.getText(), "100 fps, 10.0 ms, worst 10.0 ms") << "an open second leaves the line alone";

            EXPECT_TRUE(rate.add(45.0));
            EXPECT_EQ(rate.getText(), "40 fps, 25.0 ms, worst 45.0 ms") << "the first second's worst did not carry over";

            // A rate that does not divide a second: 117 x 8.5 = 994.5 is open and 118 x 8.5 = 1003
            // closes, at 1000 / 8.5 = 117.6 frames a second rounded to the nearest whole one.
            for (int at = 0; at < 117; ++at)
                EXPECT_FALSE(rate.add(8.5));
            EXPECT_TRUE(rate.add(8.5));
            EXPECT_EQ(rate.getText(), "118 fps, 8.5 ms, worst 8.5 ms");

            // One frame longer than a second is a second on its own: 1000 / 1500 rounds to one.
            EXPECT_TRUE(rate.add(1500.0));
            EXPECT_EQ(rate.getText(), "1 fps, 1500.0 ms, worst 1500.0 ms");
        }
    }
}
