#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxbench/codesettle.hpp>
#include <components/rtxbench/threadwatch.hpp>

namespace Rtx
{
    namespace
    {
        constexpr double sCap = 20.0;

        /// Windows closing every two seconds from 2.0 s, one busiest share each, with a view.
        ThreadWindows windowsOf(const std::span<const double> shares)
        {
            ThreadWindows windows;
            std::vector<ThreadCpu> threads{ { 7, 0.0 } };
            windows.take(0.0, threads);
            for (std::size_t at = 0; at < shares.size(); ++at)
            {
                threads[0].mSeconds += shares[at] * ThreadWindows::sWindowSeconds;
                windows.take(static_cast<double>(at + 1) * ThreadWindows::sWindowSeconds, threads);
            }

            return windows;
        }

        TEST(RtxCodeSettleTest, twoQuietWindowsAfterABusyOneSettle)
        {
            CodeSettle settle(sCap);

            // The pause at the start, then a core flat out, the share itself, and a window under
            // it, which is one quiet window and a pause.
            settle.judge(
                windowsOf(std::vector<double>{ 0.1, 1.0, CodeSettle::sBusyShare, CodeSettle::sBusyShare - 0.01 }));
            EXPECT_FALSE(settle.isSettled()) << "one quiet window, closed at 8.0 s, is a pause";

            // Flat out again, then two quiet in a row.
            settle.judge(windowsOf(std::vector<double>{ 0.1, 1.0, 0.25, 0.24, 1.0, 0.1 }));
            EXPECT_FALSE(settle.isSettled()) << "the first quiet window after that, closed at 12.0 s";

            settle.judge(windowsOf(std::vector<double>{ 0.1, 1.0, 0.25, 0.24, 1.0, 0.1, 0.1 }));
            EXPECT_TRUE(settle.isSettled()) << "the second, closed at 14.0 s";
            EXPECT_TRUE(settle.wentQuiet());

            // (0.1 + 1.0 + 0.25 + 0.24 + 1.0 + 0.1 + 0.1) * 2 s.
            EXPECT_EQ(settle.describe(), "the busiest other thread went quiet at 14.0 s after 5.6 s of CPU");

            const std::string report = settle.describe();
            settle.judge(windowsOf(std::vector<double>{ 0.1, 1.0, 0.25, 0.24, 1.0, 0.1, 0.1, 1.0 }));
            EXPECT_EQ(settle.describe(), report) << "a judgement after the settle is nothing";
        }

        TEST(RtxCodeSettleTest, quietBeforeAnyBusyWindowIsNotSettled)
        {
            CodeSettle settle(sCap);
            settle.judge(windowsOf(std::vector<double>{ 0.1, 0.1, 0.1 }));
            EXPECT_FALSE(settle.isSettled());
            EXPECT_EQ(settle.describe(),
                "no other thread was busy in 6.0 s, so the cap stood; the windows were 0.10 0.10 0.10")
                << "not settled and not busy: the compile has not shown yet";
        }

        TEST(RtxCodeSettleTest, theCapSettlesARunWithNoQuietWindowsAndSaysSo)
        {
            CodeSettle settle(sCap);

            settle.judge(windowsOf(std::vector<double>(9, 1.0)));
            EXPECT_FALSE(settle.isSettled()) << "18 s of 20";

            settle.judge(windowsOf(std::vector<double>(10, 1.0)));
            EXPECT_TRUE(settle.isSettled());
            EXPECT_FALSE(settle.wentQuiet()) << "settled by the cap is not settled by the thread";
            EXPECT_EQ(settle.describe(),
                "the busiest other thread had no quiet window in 20.0 s (20.0 s of CPU), so the cap stood; the windows "
                "were 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00");
        }

        TEST(RtxCodeSettleTest, aPlatformWithNoViewWaitsTheCapOutAndSaysSo)
        {
            CodeSettle settle(sCap);

            ThreadWindows blind;
            blind.take(19.5, {});
            settle.judge(blind);
            EXPECT_FALSE(settle.isSettled()) << "19.5 s of 20";

            blind.take(sCap, {});
            settle.judge(blind);
            EXPECT_TRUE(settle.isSettled());
            EXPECT_FALSE(settle.wentQuiet());
            EXPECT_EQ(
                settle.describe(), "no view of the process's other threads on this platform, so the cap of 20 s stood");
        }
    }
}
