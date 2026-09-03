#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <apps/rtxtool/settling.hpp>

namespace RtxTool
{
    namespace
    {
        using namespace std::chrono_literals;

        /// A frame that changes on its third drawing is seen on the third, the settled picture is
        /// the changed one, and a frame that never changes is ended by the cap with the early
        /// picture still in hand.
        TEST(RtxSettlingTest, aChangeIsSeenWhenItComesAndAHoldEndsAtTheCap)
        {
            const std::vector<std::uint8_t> early(4, 1);
            const std::vector<std::uint8_t> changed(4, 7);

            int drawn = 0;
            const auto changesOnTheThird = [&](std::vector<std::uint8_t>& pixels) {
                ++drawn;
                pixels = drawn >= 3 ? changed : early;
            };

            std::vector<std::uint8_t> settled;
            const std::optional<double> seen = watchSettling(changesOnTheThird, early, settled, 1ms, 5s);
            EXPECT_TRUE(seen.has_value());
            EXPECT_GE(*seen, 0.0);
            EXPECT_EQ(drawn, 3);
            EXPECT_EQ(settled, changed);

            drawn = 0;
            const auto holds = [&](std::vector<std::uint8_t>& pixels) {
                ++drawn;
                pixels = early;
            };

            EXPECT_FALSE(watchSettling(holds, early, settled, 1ms, 20ms).has_value());
            EXPECT_GE(drawn, 2);
            EXPECT_EQ(settled, early);

            EXPECT_EQ(describeSettling(1.5), "settled at 1.5 s");
            EXPECT_EQ(describeSettling(std::nullopt), "held for 10 s");
        }
    }
}
