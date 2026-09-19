#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <components/rtxbench/codesettle.hpp>

namespace Rtx
{
    namespace
    {
        /// Every image the same word but `moved`, which carries `seed` so a change is one image.
        CodeSettle::Traced tracedOf(const std::uint64_t seed, const std::size_t moved = 0)
        {
            CodeSettle::Traced traced{};
            for (std::size_t at = 0; at < traced.size(); ++at)
                traced[at] = { 7, at };
            traced[moved] = { seed, moved };

            return traced;
        }

        constexpr double sCap = 10.0;

        /// Four takes a window, so the window closes on the fourth.
        constexpr double sStep = CodeSettle::sWindowSeconds / 4.0;

        /// A run's clocks: the wall stepping by `sStep` a take, and the other threads' CPU by
        /// `share` of it.
        struct Clocks
        {
            double mSeconds = 0.0;
            double mCpu = 0.0;

            void take(CodeSettle& settle, const CodeSettle::Traced& traced, const double share)
            {
                settle.take(traced, mSeconds, mCpu);
                mSeconds += sStep;
                mCpu += share * sStep;
            }

            void window(CodeSettle& settle, const CodeSettle::Traced& traced, const double share)
            {
                for (int at = 0; at < 4; ++at)
                    take(settle, traced, share);
            }
        };

        TEST(RtxCodeSettleTest, aWindowSettlesOnceTheOtherThreadsTakeUnderTheShare)
        {
            CodeSettle settle(sCap);
            Clocks clocks;

            // The anchor is the first take, so the first window closes on the take at 2.0 s: the
            // fifth, which is the first of the second call.
            clocks.window(settle, tracedOf(1), 1.0);
            EXPECT_FALSE(settle.isSettled()) << "a core flat out";

            clocks.window(settle, tracedOf(1), CodeSettle::sBusyShare);
            EXPECT_FALSE(settle.isSettled()) << "the share itself is busy";

            clocks.window(settle, tracedOf(1), CodeSettle::sBusyShare - 0.01);
            EXPECT_FALSE(settle.isSettled()) << "the window under the share has not closed yet";

            clocks.take(settle, tracedOf(1), 0.0);
            EXPECT_TRUE(settle.isSettled()) << "closed at 6.0 s";

            // (1.0 + 0.5 + 0.49) * 2 s of CPU across the three windows.
            const std::string report = settle.describe();
            EXPECT_NE(report.find("went quiet at 6.0 s after 4.0 s of CPU"), std::string::npos) << report;
            EXPECT_NE(report.find("did not change in 13 traces"), std::string::npos) << report;
        }

        TEST(RtxCodeSettleTest, aDigestThatMovedInsideAQuietWindowKeepsItFromSettling)
        {
            CodeSettle settle(sCap);
            Clocks clocks;

            clocks.take(settle, tracedOf(1), 0.0);
            clocks.take(settle, tracedOf(1), 0.0);
            clocks.take(settle, tracedOf(2, 4), 0.0);
            clocks.take(settle, tracedOf(2, 4), 0.0);
            clocks.take(settle, tracedOf(2, 4), 0.0);
            EXPECT_FALSE(settle.isSettled()) << "the code moved at 1.0 s, inside the window closed at 2.0 s";

            clocks.take(settle, tracedOf(2, 4), 0.0);
            clocks.take(settle, tracedOf(2, 4), 0.0);
            clocks.take(settle, tracedOf(2, 4), 0.0);
            EXPECT_FALSE(settle.isSettled());

            clocks.take(settle, tracedOf(2, 4), 0.0);
            EXPECT_TRUE(settle.isSettled()) << "the window closed at 4.0 s held still";

            const std::string report = settle.describe();
            EXPECT_NE(report.find("went quiet at 4.0 s after 0.0 s of CPU"), std::string::npos) << report;
            EXPECT_NE(report.find("changed at trace 3 of 9"), std::string::npos) << report;
            EXPECT_NE(report.find("g-guide"), std::string::npos) << report << " names the image that moved";
            EXPECT_EQ(report.find("g-albedo"), std::string::npos) << report;

            clocks.take(settle, tracedOf(3), 0.0);
            EXPECT_EQ(settle.describe(), report) << "a take after the settle is nothing";
        }

        TEST(RtxCodeSettleTest, aSecondChangeIsCounted)
        {
            CodeSettle settle(sCap);
            Clocks clocks;

            clocks.take(settle, tracedOf(1), 0.0);
            clocks.take(settle, tracedOf(2), 0.0);
            clocks.take(settle, tracedOf(3), 0.0);
            EXPECT_NE(settle.describe().find("changed 2 times, last at trace 3 of 3"), std::string::npos)
                << settle.describe();
        }

        TEST(RtxCodeSettleTest, theCapSettlesARunWithNoQuietWindowAndSaysSo)
        {
            CodeSettle settle(sCap);
            Clocks clocks;

            while (clocks.mSeconds < sCap)
                clocks.take(settle, tracedOf(1), 1.0);
            EXPECT_FALSE(settle.isSettled()) << "9.5 s of 10";

            clocks.take(settle, tracedOf(1), 1.0);
            EXPECT_TRUE(settle.isSettled());
            EXPECT_NE(settle.describe().find("had no quiet window in 10.0 s (10.0 s of CPU), so the cap stood"),
                std::string::npos)
                << settle.describe();
        }

        TEST(RtxCodeSettleTest, aPlatformWithNoClockWaitsTheCapOutAndSaysSo)
        {
            CodeSettle settle(sCap);

            for (int at = 0; at < 20; ++at)
                settle.take(tracedOf(1), at * sStep, std::nullopt);
            EXPECT_FALSE(settle.isSettled()) << "9.5 s of 10";

            settle.take(tracedOf(1), sCap, std::nullopt);
            EXPECT_TRUE(settle.isSettled());
            EXPECT_NE(settle.describe().find(
                          "no clock of the process's other threads on this platform, so the cap of 10 s stood"),
                std::string::npos)
                << settle.describe();
        }
    }
}
